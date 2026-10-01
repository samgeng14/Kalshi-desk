#include "desk/decision_log.h"

#include <cmath>

#include "desk/decision_router.h"
#include "desk/edge.h"
#include "desk/fees.h"
#include "desk/systematic.h"

namespace desk {
namespace {

double num(const json& j, const char* k, double fallback = 0) {
  return j.is_object() && j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>()) ? j[k].get<double>() : fallback;
}
long long whole(const json& j, const char* k) {
  return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<long long>() : 0;
}
std::string text(const json& j, const char* k) {
  return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}
SqlValue opt(const json& j, const char* k) {
  if (j.is_object() && j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>())) return j[k].get<double>();
  return nullptr;
}

}  // namespace

long long record_decision(Store& store, const json& row, const std::string& event, long long ts, long long proposal_id,
                          const std::string& note, double count, double price, const std::string& style) {
  const json quote = row.contains("quote") && row["quote"].is_object() ? row["quote"] : json::object();
  const json inputs = quote.contains("diagnostics") && quote["diagnostics"].is_object() &&
                              quote["diagnostics"].contains("inputs") ? quote["diagnostics"]["inputs"] : json::object();
  json reasons = row.contains("reasons") && row["reasons"].is_array() ? row["reasons"] : json::array();
  const double p_win = num(row, "p_win", NAN);
  const long long new_id = store.insert(
      "INSERT INTO trade_decisions(ts,decision_id,event,ticker,event_ticker,side,category,model_name,model_version,"
      "p_yes,fair_probability,sigma,bid,ask,entry_price,fee,slippage,uncertainty_buffer,raw_edge,executable_edge,"
      "route,verdict,style,count,reasons_json,inputs_json,quote_json,model_ts,book_ts,proposal_id,note,"
      "source_decision_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
      {ts, text(row, "decision_id"), event, text(row, "ticker"), text(row, "event_ticker"), text(row, "side"),
       text(row, "category"), text(row, "model").empty() ? text(row, "fair_strategy") : text(row, "model"),
       text(row, "model_version"), opt(row, "p_yes"), std::isfinite(p_win) ? SqlValue(p_win) : SqlValue(nullptr),
       opt(row, "sigma"), opt(row, "bid"), opt(row, "ask"), price >= 0 ? SqlValue(price) : opt(row, "price"),
       opt(row, "fee"), opt(row, "slippage"), opt(row, "uncertainty_buffer"), opt(row, "raw_edge"),
       opt(row, "executable_edge"), text(row, "route"), text(row, "verdict"),
       style.empty() ? text(row, "style") : style, count, reasons.dump(), inputs.dump(), quote.dump(),
       whole(row, "model_ts"), whole(row, "book_ts"), proposal_id, note, text(row, "approved_from")});
  return new_id;
}

json last_decision_snapshot(Store& store, const std::string& decision_id) {
  if (decision_id.empty()) return nullptr;
  json r = store.query("SELECT * FROM trade_decisions WHERE decision_id=? ORDER BY id DESC LIMIT 1", {decision_id});
  return r.empty() ? json(nullptr) : r[0];
}

void record_order_snapshot(Store& store, Proposal& p, const Market& m, const std::string& kind, long long ts) {
  if (!p.decision_id.empty()) {
    json last = last_decision_snapshot(store, p.decision_id);
    if (last.is_object()) {
      store.exec(
          "INSERT INTO trade_decisions(ts,decision_id,event,ticker,event_ticker,side,category,model_name,model_version,"
          "p_yes,fair_probability,sigma,bid,ask,entry_price,fee,slippage,uncertainty_buffer,raw_edge,executable_edge,"
          "route,verdict,style,count,reasons_json,inputs_json,quote_json,model_ts,book_ts,proposal_id,note,"
          "source_decision_id) "
          "SELECT ?,decision_id,'submitted',ticker,event_ticker,side,category,model_name,model_version,p_yes,"
          "fair_probability,sigma,bid,ask,?,fee,slippage,uncertainty_buffer,raw_edge,executable_edge,route,verdict,?,?,"
          "reasons_json,inputs_json,quote_json,model_ts,book_ts,?,?,source_decision_id FROM trade_decisions WHERE id=?",
          {ts, p.price, p.exec_style, p.count, p.id, kind + " " + p.strategy, last.value("id", 0LL)});
      return;
    }
  }
  // No decision behind it: a hand ticket, an exit or a close. Snapshot what the executor knows:
  // the book it just read and the model number on the proposal, if any.
  const MarketSidePrices px = normalize_prices(m);
  const Side side = side_from(p.outcome_side);
  const bool has_model = p.p_model > 0 && p.p_model < 1;
  const double fair = has_model ? (side == Side::Yes ? p.p_model : 1.0 - p.p_model) : NAN;
  const double fee = p.fee_per;
  json row = {{"ticker", p.ticker}, {"event_ticker", p.event_ticker}, {"side", p.outcome_side},
              {"category", systematic_category(json{{"series_ticker", p.series_ticker}})},
              {"model", has_model ? p.strategy : std::string()}, {"model_version", has_model ? std::string("v1") : std::string()},
              {"bid", px.bid(side)}, {"ask", px.ask(side)}, {"price", p.price}, {"fee", fee},
              {"route", kind}, {"verdict", kind},
              {"reasons", json::array({kind == "ticket" ? "hand ticket: no model decision behind it" : kind + " order"})},
              {"book_ts", m.quote_ts > 0 ? m.quote_ts : ts}};
  if (has_model) {
    row["p_yes"] = p.p_model;
    row["p_win"] = fair;
    row["sigma"] = p.sigma;
    row["raw_edge"] = fair - p.price;
    row["executable_edge"] = fair - p.price - fee;
  }
  row["decision_id"] = decision_id_for(p.ticker, side, kind + ":" + p.strategy, ts, row.value("book_ts", ts));
  p.decision_id = row["decision_id"].get<std::string>();
  record_decision(store, row, "submitted", ts, p.id, kind + " " + p.strategy, p.count, p.price, p.exec_style);
}

json decision_trace(Store& store, const std::string& order_id) {
  json out = {{"order_id", order_id}, {"proposal", nullptr}, {"decisions", json::array()}};
  json o = store.query("SELECT proposal_id, decision_id FROM orders WHERE order_id=?", {order_id});
  if (o.empty()) return out;
  const long long pid = o[0].value("proposal_id", 0LL);
  std::string did = o[0]["decision_id"].is_string() ? o[0]["decision_id"].get<std::string>() : std::string();
  if (pid > 0) {
    if (auto p = store.get_proposal(pid)) {
      out["proposal"] = p->to_json();
      if (did.empty()) did = p->decision_id;
    }
  }
  out["decision_id"] = did;
  if (did.empty()) return out;
  // The decision the order went on, and the one it was approved from when that differs (a manual
  // request is re-priced on a fresh book just before it goes, which is a new decision): what you
  // were shown, what you approved, what was re-checked, what was sent.
  std::string source;
  for (const auto& r : store.query("SELECT source_decision_id FROM trade_decisions WHERE decision_id=? AND "
                                   "source_decision_id<>'' ORDER BY id LIMIT 1", {did}))
    source = r.value("source_decision_id", std::string());
  out["source_decision_id"] = source;
  out["decisions"] = source.empty() || source == did
                         ? store.query("SELECT * FROM trade_decisions WHERE decision_id=? ORDER BY id", {did})
                         : store.query("SELECT * FROM trade_decisions WHERE decision_id IN (?,?) ORDER BY id", {source, did});
  return out;
}

}  // namespace desk
