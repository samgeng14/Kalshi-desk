#include "desk/executor.h"
#include <cmath>

#include "desk/edge.h"
#include "desk/reject.h"
#include "desk/decision_log.h"

#include <algorithm>
#include <random>
#include <set>
#include <thread>
#include <chrono>

#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/log.h"
#include "desk/risk.h"
#include "desk/timeutil.h"

namespace desk {
namespace {
// Covers request start, processing latency, and ambiguous transport failures.
struct WriteWatermark {
  Store& store;
  explicit WriteWatermark(Store& s):store(s) { store.kv_set("last_write_ts",std::to_string(now_s())); }
  ~WriteWatermark() { try { store.kv_set("last_write_ts",std::to_string(now_s())); } catch (...) {} }
};
// Kalshi answers a refused request with {"error":{"code":...,"message":...}}. A body without
// that shape (an HTML page, an empty body) came from something in between, not the exchange.
bool kalshi_error_body(const std::string& body) {
  const json j = json::parse(body, nullptr, false);
  return !j.is_discarded() && j.is_object() && j.contains("error") && j["error"].is_object();
}
}

static std::string uuid4() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned long long> d;
  unsigned long long a = d(rng), b = d(rng);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%08llx-%04llx-4%03llx-%04llx-%012llx", a & 0xffffffffULL,
                (a >> 32) & 0xffff, (a >> 48) & 0xfff, 0x8000ULL | (b & 0x3fff),
                (b >> 16) & 0xffffffffffffULL);
  return buf;
}

Executor::Executor(const Config& cfg, Store& store, KalshiClient& client)
    : cfg_(cfg), store_(store), client_(client) {}

std::string Executor::submit(Proposal& p, const std::string& client_order_id, long long deadline) {
  if (p.strategy == "close") throw std::runtime_error("A close proposal cannot be approved as new entries; use the reduce-only Close path");
  if (is_exit_strategy(p.strategy)) return submit_exit(p);
  if (!client_.authenticated()) throw std::runtime_error("no API key configured; cannot submit");
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  Market market = client_.get_market(p.ticker);
  store_.upsert_market(market);
  store_.upsert_series(client_.get_series_one(market.series_ticker));
  refresh_account();
  validate_entry(store_, cfg_, p, market);
  // A post-only price the book has already moved through would be refused by the exchange as
  // "post only cross". Say so here, from the book just read, as the soft refusal it is: nothing is
  // sent and nothing about the desk is wrong; the caller re-prices from a fresh book.
  if (p.exec_style == "maker") {
    const double ask = normalize_prices(market).ask(side_from(p.outcome_side));
    if (ask > 0 && ask < 1 && p.price >= ask - 1e-9) {
      // The book moved between the review and the send. Refusing threw the order away and made
      // the operator start over, which on a fast book means never getting filled at all.
      // Re-price to the best price that still posts instead. This can only move the bid DOWN, so
      // it is never worse than what was reviewed: a buyer filled lower paid less. If there is no
      // room left below - the offer is at the minimum tick - there is no non-crossing price and
      // the refusal stands.
      // Nor below the configured risk range: validate_entry above checked min_price against the
      // reviewed price, and the re-price must not step outside what that check allowed.
      const double tick = market.tick > 0 ? market.tick : 0.01;
      const double reprice = std::floor((ask - tick) / tick + 1e-9) * tick;
      if (reprice >= std::max(tick, cfg_.risk.min_price) - 1e-9 && reprice < p.price - 1e-9) {
        LOG_INFO("post-only %s %s: book moved to %s, re-pricing %s -> %s (never above the review)",
                 p.ticker.c_str(), p.outcome_side.c_str(), fmt2(ask).c_str(),
                 fmt2(p.price).c_str(), fmt2(reprice).c_str());
        p.price = reprice;
      } else {
        throw SoftRejection(RejectClass::SoftMarketState,
                            "book moved: a post-only " + p.outcome_side + " bid at " + fmt2(p.price) +
                                " would cross the " + fmt2(ask) + " offer, and there is no lower "
                                "price left to post at inside the risk range; not sent");
      }
    }
  }
  // Fresh from the exchange: an ordinary strategy proposal never silently adds to or flips a
  // position. Qualified additions and verified hedges were classified by entry_context above.
  if (p.entry_kind != "addition" && p.entry_kind != "hedge") {
    for (const auto& pos : store_.positions())
      if (pos.ticker == p.ticker && pos.position != 0)
        throw std::runtime_error("position already open in " + p.ticker + "; entries never add to or flip it");
  }
  // Collateral is local to each exchange shard. Reported cash includes resting reservations.
  auto sb = parse_balance(client_.call("GET", "/portfolio/balance", {{"exchange_index", std::to_string(market.exchange_index)}}));
  auto series = store_.get_series(market.series_ticker);
  double cost = p.count*p.price + FeeModel::from_series(*series).fee(p.price,p.count,p.exec_style=="taker");
  double reserved = 0;
  for (const auto& o : store_.orders("resting")) {
    auto om = store_.get_market(o.ticker);
    if (!om || om->exchange_index != market.exchange_index) continue;
    auto os = store_.get_series(om->series_ticker);
    if (!os) throw std::runtime_error("Resting-order fee metadata unavailable; sync before trading");
    double px = o.outcome_side == "yes" ? o.yes_price : o.no_price;
    reserved += o.remaining_count * px + FeeModel::from_series(*os).fee(px, o.remaining_count, false);
  }
  double available = std::max(0.0, sb.balance - reserved);
  if(p.strategy!="ticket")available=std::max(0.,available-shard_cash_view(store_,market,now_s()).value("queued_reserved",0.));
  if (cost > available + 1e-8) {
    // Kalshi keeps collateral per exchange shard: crypto and commodities on 2, some sports on 3.
    // A shard you have never funded refuses every order in its whole category, which is a strange
    // way to discover that MLB lives somewhere else. Move the shortfall from shard 0 and say so;
    // the transfer is asynchronous, so this order still does not go.
    std::string extra = ". Allocate collateral in Kalshi Account > Exchange indexes. No order sent.";
    // Which shard has the money is a question, not an assumption. This used to move from shard 0
    // and refuse to run at all when the target WAS shard 0, which is exactly backwards for an
    // account whose cash has drifted elsewhere: on 2026-09-11 shard 0 held $0.25 against $65.29
    // on shard 3, and every weather market on shard 0 was untradeable with a funded account.
    // Ask every shard, move from the richest one that is not this one.
    // NOT gated on client_order_id being empty any more. It used to be, and the effect was that
    // on-demand top-up never ran for the only orders that need it: every hand-approved order
    // arrives through the manual queue carrying its client_request_id, so the guard was false on
    // exactly those and true only for the algorithm's own sends. In the whole log the on-demand
    // path fired zero times while the operator was refused for collateral repeatedly.
    // The per-shard 60-second rate limit below is what stops a transfer storm, and it does that
    // job whether or not the order carries an id, so this condition was protecting nothing.
    if (cfg_.risk.shard_topup_on_demand && client_.authenticated()) {
      long long now = now_s();
      std::string key = "ondemand_ts:" + std::to_string(market.exchange_index);
      long long last = std::stoll(store_.kv_get(key).value_or("0"));
      if (now - last >= 60) {
        store_.kv_set(key, std::to_string(now));
        try {
          const double keep = 5.0;            // never drain the shard being borrowed from
          int from = -1;
          double spare = 0;
          for (int idx = 0; idx <= 3; ++idx) {
            if (idx == market.exchange_index) continue;
            double bal = 0;
            try { bal = parse_balance(client_.call("GET", "/portfolio/balance",
                                                  {{"exchange_index", std::to_string(idx)}})).balance; }
            catch (const std::exception&) { continue; }   // a shard that will not answer is not a source
            if (bal - keep > spare) { spare = bal - keep; from = idx; }
          }
          double want = std::min(cfg_.risk.shard_topup_max, std::max(cost - available, 1.0) + 2.0);
          double can = std::min(want, spare);
          if (from >= 0 && can >= 1.0) {
            std::string id = client_.transfer_between_shards(from, market.exchange_index, can);
            store_.log_event("warn", "shard " + std::to_string(market.exchange_index) + " was short for " + p.ticker +
                                         ": moved $" + fmt2(can) + " from shard " + std::to_string(from) +
                                         " (transfer " + id + ")");
            extra = ". Moved $" + fmt2(can) + " there from shard " + std::to_string(from) + " (transfer " +
                    id.substr(0, 8) + "); Kalshi settles that in a few seconds, so click again shortly. "
                    "No order sent yet.";
          } else if (from < 0) {
            extra = ". No other shard has more than $" + fmt2(keep) + " to spare, so nothing was moved. "
                    "No order sent.";
          } else {
            extra = ". The best other shard (shard " + std::to_string(from) + ") has only $" + fmt2(std::max(0.0, spare)) +
                    " to spare, so nothing was moved. No order sent.";
          }
        } catch (const std::exception& e) {
          store_.log_event("error", std::string("on-demand shard top-up failed: ") + e.what());
          extra = std::string(". Automatic transfer failed (") + e.what() +
                  "); move it by hand in Kalshi Account > Exchange indexes. No order sent.";
        }
      } else {
        extra = ". A transfer to that shard was already requested in the last minute; give it a moment and click again. No order sent.";
      }
    }
    throw std::runtime_error("Exchange shard " + std::to_string(market.exchange_index) + " has $" + fmt2(available) +
                             " available; this order needs $" + fmt2(cost) + extra);
  }
  OrderRequest req;
  req.ticker = p.ticker;
  req.outcome_side = p.outcome_side;
  req.price = clamp_price(p.price, market.tick);
  req.count = floor_contracts(p.count);
  req.exchange_index = market.exchange_index;
  req.post_only = (p.exec_style == "maker");
  req.tif = (p.exec_style == "maker") ? "good_till_canceled" : "immediate_or_cancel";
  req.client_order_id = client_order_id.empty() ? uuid4() : client_order_id;
  if(deadline>0 && now_s()>=deadline)throw std::runtime_error("Manual review expired before exchange dispatch; no order sent");
  // Every order leaves a snapshot of the probability and the book behind it (decision_log.h):
  // the decision's own when it came from the router, one built here for a hand ticket.
  record_order_snapshot(store_, p, market, p.strategy == "ticket" ? "ticket" : "entry", now_s());
  store_.update_proposal(p);
  if(!client_order_id.empty())store_.kv_set("manual_attempt:"+client_order_id,json{{"status","sending"},{"proposal_id",p.id}}.dump());
  // The write watermark makes the next account read wait for the exchange to reflect this order.
  // A definite refusal from Kalshi wrote nothing there, so the old watermark is put back (see the
  // catch below) rather than making the next order, a re-priced retry say, wait out a write that
  // never happened.
  const auto prior_write = store_.kv_get("last_write_ts");
  try {
    WriteWatermark watermark(store_);
    OrderResponse r = client_.create_order(req);
    p.order_id = r.order_id;
    p.status = "submitted";
    p.note = p.note.empty() ? "submitted" : p.note + " | submitted";
    store_.update_proposal(p);
    Order o;
    o.order_id = r.order_id;
    o.client_order_id = req.client_order_id;
    o.ticker = p.ticker;
    o.outcome_side = p.outcome_side;
    o.book_side = p.outcome_side == "yes" ? "bid" : "ask";
    o.type = "limit";
    o.status = r.remaining_count > 0 ? "resting" : "executed";
    o.yes_price = p.outcome_side == "yes" ? req.price : 1.0 - req.price;
    o.no_price = 1.0 - o.yes_price;
    o.initial_count = req.count;
    o.fill_count = r.fill_count;
    o.remaining_count = r.remaining_count;
    o.created_time = now_iso();
    store_.upsert_order(o, p.id);
    if(!client_order_id.empty())store_.kv_set("manual_attempt:"+client_order_id,json{{"status","submitted"},{"order_id",r.order_id},{"proposal_id",p.id}}.dump());
    LOG_INFO("submitted %s %s %s x%.2f @ %.2f -> %s (filled %.2f)", p.strategy.c_str(),
             p.outcome_side.c_str(), p.ticker.c_str(), req.count, req.price, r.order_id.c_str(),
             r.fill_count);
    store_.log_event("info", "order " + p.strategy + " " + p.outcome_side + " " + p.ticker + " x" + fmt2(req.count) +
                                 " @ " + fmt2(req.price) + (r.fill_count > 0 ? " filled " + fmt2(r.fill_count) : " resting"));
    return r.order_id;
  } catch (const ApiError& e) {
    // Only a definite refusal from Kalshi itself (a 4xx carrying its {"error":{...}} body) puts
    // the old watermark back. A 408, or a 4xx a proxy produced, does not prove nothing was
    // written, so the watermark stays and the next account read waits as usual.
    if (e.status >= 400 && e.status < 500 && e.status != 408 && kalshi_error_body(e.body)) {
      if (prior_write) store_.kv_set("last_write_ts", *prior_write);
      else store_.exec("DELETE FROM kv WHERE key='last_write_ts'");
    }
    p.status = e.status >= 500 ? "unknown" : "failed";
    if(!client_order_id.empty())store_.kv_set("manual_attempt:"+client_order_id,json{{"status",p.status},{"error",e.what()},{"proposal_id",p.id}}.dump());
    if(p.status=="unknown") store_.kv_set("uncertain_client_order_id", req.client_order_id);
    if (p.status == "unknown") store_.kv_set("uncertain_since", std::to_string(now_s()));
    const RejectClass cls = classify_rejection(e.status, e.body);
    p.note = std::string("api ") + std::to_string(e.status) + " (" + reject_class_name(cls) + "): " + e.body.substr(0, 200);
    store_.update_proposal(p);
    // A market-state or collateral refusal is the market moving, not the desk being wrong; the
    // event log says which, so a post-only cross never reads like a malformed order again.
    if (reject_is_soft(cls) || cls == RejectClass::Retryable)
      store_.log_event("warn", "NOT PLACED " + p.ticker + " " + p.outcome_side + " (" + reject_class_name(cls) + "): " + e.body.substr(0, 120));
    else
      store_.log_event("error", "REJECTED " + p.ticker + " " + p.outcome_side + " (" + reject_class_name(cls) + "): " + e.body.substr(0, 120));
    throw;
  } catch (const std::exception& e) {
    p.status="unknown"; p.note=std::string("Order outcome uncertain; reconcile before retry: ")+e.what();
    if(!client_order_id.empty())store_.kv_set("manual_attempt:"+client_order_id,json{{"status","unknown"},{"error",e.what()},{"proposal_id",p.id}}.dump());
    store_.update_proposal(p); store_.kv_set("uncertain_client_order_id",req.client_order_id);
    store_.kv_set("uncertain_since", std::to_string(now_s()));
    throw;
  }
}

std::string Executor::submit_exit(Proposal& p) {
  if (!is_exit_strategy(p.strategy)) throw std::runtime_error("not an exit proposal");
  if (!client_.authenticated()) throw std::runtime_error("no API key configured; cannot submit");
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  if (p.status != "pending") throw std::runtime_error("Exit proposal is " + p.status);
  if (!std::isfinite(p.price) || !std::isfinite(p.count) || p.price <= 0 || p.price >= 1 || p.count < .01 ||
      std::fabs(p.count - floor_contracts(p.count)) > 1e-8)
    throw std::runtime_error("Exit price must be between 0 and 1; quantity must be in 0.01 increments");
  if (p.exec_style != "maker" && p.exec_style != "taker") throw std::runtime_error("Invalid exit execution style");
  if (now_s() - p.updated_ts > cfg_.risk.proposal_ttl_seconds) throw std::runtime_error("Exit proposal expired; refresh before approving");
  if (store_.kv_get("kill").value_or("0") == "1" && p.exec_style != "taker") throw std::runtime_error("kill switch is on (taker exits still allowed)");
  refresh_account();  // an uncertain earlier order never blocks a risk-reducing order
  auto positions = store_.positions();
  auto it = std::find_if(positions.begin(), positions.end(), [&](const Position& x) { return x.ticker == p.ticker; });
  if (it == positions.end() || it->position == 0) throw std::runtime_error("no position to exit in " + p.ticker);
  bool long_yes = it->position > 0;
  // An exit sells the side we hold: p.outcome_side is the side we BUY (the opposite), p.price its price.
  if ((long_yes && p.outcome_side != "no") || (!long_yes && p.outcome_side != "yes"))
    throw std::runtime_error("exit side does not match the position");
  double n = floor_contracts(std::min(p.count, std::fabs(it->position)));
  if (p.strategy == "target" || p.strategy == "stop")
    n = std::min(n, manual_position_quantity(store_, *it));
  if (n < 0.01) throw std::runtime_error("exit quantity below 0.01");
  for (const auto& o : store_.orders("resting"))
    if (o.ticker == p.ticker)
      throw std::runtime_error("another order is resting in " + p.ticker + "; cancel it before exiting");
  Market market = client_.get_market(p.ticker);
  store_.upsert_market(market);
  if (!market.tradable() || (!market.close_time.empty() && parse_iso(market.close_time) <= now_s()))
    throw std::runtime_error("Market is closed to exits");
  OrderRequest req;
  req.ticker = p.ticker;
  req.outcome_side = p.outcome_side;
  req.price = clamp_price(p.price, market.tick);
  req.count = n;
  req.exchange_index = market.exchange_index;
  // Kalshi accepts reduce_only only on immediate-or-cancel orders: a resting exit carrying it is
  // refused outright with "reduce_only can only be used with IoC orders". That rejection killed
  // this desk four times over 2026-09-06 and 07 while the position it was trying to close stayed
  // open. A resting exit therefore goes without the flag, and the protection it would have given
  // is enforced here instead - the order is sized to the position that exists right now, and the
  // kill sweep cancels anything still resting.
  req.post_only = (p.exec_style == "maker");
  req.tif = (p.exec_style == "maker") ? "good_till_canceled" : "immediate_or_cancel";
  req.reduce_only = (req.tif == "immediate_or_cancel");
  if (!req.reduce_only && p.note.find("local reduce-only") == std::string::npos)
    p.note += " | local reduce-only: resting exits cannot carry the exchange flag";
  req.client_order_id = uuid4();
  record_order_snapshot(store_, p, market, "exit", now_s());
  store_.update_proposal(p);
  try {
    WriteWatermark watermark(store_);
    OrderResponse r = client_.create_order(req);
    p.order_id = r.order_id;
    p.status = "submitted";
    p.count = n;
    store_.update_proposal(p);
    Order o;
    o.order_id = r.order_id;
    o.client_order_id = req.client_order_id;
    o.ticker = p.ticker;
    o.outcome_side = req.outcome_side;
    o.book_side = req.outcome_side == "yes" ? "bid" : "ask";
    o.type = "limit";
    o.status = r.remaining_count > 0 ? "resting" : "executed";
    o.yes_price = req.outcome_side == "yes" ? req.price : 1.0 - req.price;
    o.no_price = 1.0 - o.yes_price;
    o.initial_count = req.count;
    o.fill_count = r.fill_count;
    o.remaining_count = r.remaining_count;
    o.created_time = now_iso();
    store_.upsert_order(o, p.id);
    LOG_INFO("exit %s sell %s x%.2f @ %.2f (%s) -> %s filled %.2f", p.ticker.c_str(), long_yes ? "YES" : "NO", n,
             1.0 - req.price, p.exec_style.c_str(), r.order_id.c_str(), r.fill_count);
    store_.log_event("info", "exit " + p.ticker + " sell " + (long_yes ? "YES" : "NO") + " x" + fmt2(n) + " @ " +
                                 fmt2(1.0 - req.price) + (r.fill_count > 0 ? " filled " + fmt2(r.fill_count) : " resting"));
    return r.order_id;
  } catch (const ApiError& e) {
    p.status = e.status >= 500 ? "unknown" : "failed";
    if (p.status == "unknown") store_.kv_set("uncertain_client_order_id", req.client_order_id);
    if (p.status == "unknown") store_.kv_set("uncertain_since", std::to_string(now_s()));
    p.note = std::string("api ") + std::to_string(e.status) + ": " + e.body.substr(0, 200);
    store_.update_proposal(p);
    store_.log_event("error", "EXIT REJECTED " + p.ticker + ": " + e.body.substr(0, 120));
    throw;
  } catch (const std::exception& e) {
    p.status = "unknown";
    p.note = std::string("Exit outcome uncertain; reconcile before retry: ") + e.what();
    store_.update_proposal(p);
    store_.kv_set("uncertain_client_order_id", req.client_order_id);
    store_.kv_set("uncertain_since", std::to_string(now_s()));
    throw;
  }
}

json Executor::cancel(const std::string& order_id) {
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  auto known = store_.query("SELECT ticker FROM orders WHERE order_id=?", {order_id});
  if (known.empty()) throw std::runtime_error("Order ticker unavailable; sync before canceling");
  WriteWatermark watermark(store_);
  auto rows = store_.query("SELECT * FROM orders WHERE order_id=?", {order_id});
  double resting_before = rows.empty() ? 0.0 : rows[0].value("remaining_count", 0.0);
  // Mark the intent before the call. A cancel that hangs or dies mid-flight then leaves a
  // proposal that says what is actually true - a cancel was asked for and the outcome is not
  // known - instead of one still claiming to be resting, and reconcile settles it from the
  // exchange's own view of the order.
  long long pid0 = store_.proposal_for_order(order_id);
  if (pid0 > 0)
    if (auto p0 = store_.get_proposal(pid0))
      if (p0->status == "submitted") { p0->status = "cancel_pending"; store_.update_proposal(*p0); }
  json r = client_.cancel_order(order_id, known[0]["ticker"].get<std::string>());

  // How many contracts the cancel actually took off the book. Anything that was resting a moment
  // ago and did not come back is a fill that landed while the cancel was in flight. Reading
  // fill_count out of the local table instead - which is what this did - reports whatever the
  // desk last synced and silently loses those contracts until some later reconcile finds them,
  // with the proposal meanwhile claiming it was cancelled.
  double reduced = -1;
  if (r.contains("reduced_by")) {
    const json& rb = r["reduced_by"];
    reduced = rb.is_string() ? std::atof(rb.get<std::string>().c_str()) : rb.get<double>();
  }
  double raced = (std::isfinite(reduced) && reduced >= 0 && reduced <= resting_before && resting_before > 0)
                     ? std::max(0.0, resting_before - reduced) : 0.0;
  store_.log_event("info", "canceled " + order_id.substr(0, 8) +
                               (raced > 1e-9 ? "; " + fmt2(raced) + " filled while cancelling" : ""));
  if (!rows.empty()) {
    // The order row records the race too: what was resting is gone, and the raced contracts are
    // fills, so the systematic trader and the exit sweep read the right count from the table.
    const double prior_fill = rows[0].value("fill_count", 0.0);
    const double filled = prior_fill + raced;
    store_.exec("UPDATE orders SET status='canceled',fill_count=?,remaining_count=0,updated_ts=? WHERE order_id=?",
                {filled, now_s(), order_id});
    long long pid = store_.proposal_for_order(order_id);
    if (pid > 0) {
      if (auto p = store_.get_proposal(pid)) {
        p->status = filled > 0 ? "filled" : "canceled";
        if (filled > 0) p->note += " | partial " + fmt2(filled) + ", rest canceled";
        store_.update_proposal(*p);
      }
    }
  }
  if (raced > 1e-9) {
    // The position moved between the desk's last read and this cancel. Everything downstream -
    // exposure, the exit sweep, the caps - is now working from a stale number, so go and get the
    // real one rather than waiting for the next scheduled sync.
    store_.log_event("warn", "fill during cancel on " + known[0]["ticker"].get<std::string>() +
                                 ": " + fmt2(raced) + " contracts; re-reading the portfolio");
    try { reconcile(false); } catch (const std::exception& e) {
      LOG_WARN("reconcile after cancel race: %s", e.what());
    }
  }
  return r;
}

json Executor::close_position(const std::string& ticker, double count) {
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  if (!std::isfinite(count) || count < 0) throw std::runtime_error("Invalid close quantity");
  refresh_account();
  for (const auto& o: store_.orders("resting"))
    if (o.ticker == ticker) throw std::runtime_error("Cancel the remaining orders in this market before closing; they could refill your position");
  auto positions = store_.positions();
  auto it = std::find_if(positions.begin(), positions.end(),
                         [&](const Position& p) { return p.ticker == ticker; });
  if (it == positions.end() || it->position == 0) throw std::runtime_error("no position in " + ticker);
  Market m = client_.get_market(ticker);
  long long tclose = parse_iso(m.close_time);
  if (!m.tradable() || (tclose > 0 && tclose <= now_s())) {
    std::string when = m.expected_expiration_time.empty() ? "soon" : m.expected_expiration_time;
    throw std::runtime_error("Trading in " + ticker + " closed at " + m.close_time + "; the position settles at " + when +
                             " (payout if right, nothing if wrong). Nothing to close.");
  }
  double n = count > 0 ? std::min(count, std::fabs(it->position)) : std::fabs(it->position);
  OrderRequest req;
  req.ticker = ticker;
  req.count = floor_contracts(n);
  if (req.count < .01) throw std::runtime_error("Close quantity is below 0.01");
  req.exchange_index = m.exchange_index;
  req.reduce_only = true;
  req.tif = "immediate_or_cancel";
  req.client_order_id = uuid4();
  if (it->position > 0) {
    // long YES: sell YES = buy NO at (1 - yes_bid)
    if (m.yes_bid <= 0) throw std::runtime_error("no bid to close into");
    req.outcome_side = "no";
    req.price = clamp_price(1.0 - m.yes_bid, m.tick);
  } else {
    // long NO: buy YES at yes_ask
    if (m.yes_ask <= 0 || m.yes_ask >= 1) throw std::runtime_error("no ask to close into");
    req.outcome_side = "yes";
    req.price = clamp_price(m.yes_ask, m.tick);
  }
  Proposal p;
  p.ticker = ticker;
  p.event_ticker = m.event_ticker;
  p.series_ticker = m.series_ticker;
  p.strategy = "close";
  p.mode = "manual";
  p.outcome_side = req.outcome_side;
  p.exec_style = "taker";
  p.status = "pending";
  p.price = req.price;
  p.count = req.count;
  p.yes_bid = m.yes_bid;
  p.yes_ask = m.yes_ask;
  p.note = "close position";
  store_.insert_proposal(p);
  record_order_snapshot(store_, p, m, "close", now_s());
  store_.update_proposal(p);
  try {
    WriteWatermark watermark(store_);
    OrderResponse r = client_.create_order(req);
    p.order_id = r.order_id;
    p.status = "submitted";
    store_.update_proposal(p);
    Order o;
    o.order_id = r.order_id;
    o.client_order_id = req.client_order_id;
    o.ticker = ticker;
    o.outcome_side = req.outcome_side;
    o.book_side = req.outcome_side == "yes" ? "bid" : "ask";
    o.type = "limit";
    o.status = r.remaining_count > 0 ? "resting" : "executed";
    o.yes_price = req.outcome_side == "yes" ? req.price : 1.0 - req.price;
    o.no_price = 1.0 - o.yes_price;
    o.initial_count = req.count;
    o.fill_count = r.fill_count;
    o.remaining_count = r.remaining_count;
    o.created_time = now_iso();
    store_.upsert_order(o, p.id);
    LOG_INFO("close %s x%.2f -> %s filled %.2f", ticker.c_str(), req.count, r.order_id.c_str(), r.fill_count);
    store_.log_event("warn", "close " + ticker + " x" + fmt2(req.count) + " filled " + fmt2(r.fill_count));
    return r.raw;
  } catch (const ApiError& e) {
    p.status = e.status >= 500 ? "unknown" : "failed";
    p.note = std::string("Close was not confirmed: ") + e.what();
    store_.update_proposal(p);
    if (p.status=="unknown") store_.kv_set("uncertain_client_order_id", req.client_order_id);
    throw;
  } catch (const std::exception& e) {
    p.status="unknown"; p.note=std::string("Close outcome uncertain; sync before retry: ")+e.what();
    store_.update_proposal(p);
    store_.kv_set("uncertain_client_order_id", req.client_order_id);
    throw;
  }
}

Proposal Executor::ticket(const OrderRequest& req, const std::string& note, long long deadline,
                          const std::string& decision_id) {
  auto m = store_.get_market(req.ticker);
  Market mk = m ? *m : client_.get_market(req.ticker);
  Proposal p;
  p.ticker = req.ticker;
  p.event_ticker = mk.event_ticker;
  p.series_ticker = mk.series_ticker;
  p.strategy = "ticket";
  p.mode = "manual";
  p.outcome_side = req.outcome_side;
  p.exec_style = req.post_only ? "maker" : "taker";
  p.status = "pending";
  p.price = req.price;
  p.count = req.count;
  p.yes_bid = mk.yes_bid;
  p.yes_ask = mk.yes_ask;
  p.note = note;
  auto fv = store_.fairs();
  for (const auto& f : fv)
    if (f.ticker == req.ticker) {
      p.p_model = f.p;
      p.sigma = f.sigma;
    }
  if (auto s = store_.get_series(mk.series_ticker)) {
    FeeModel fm = FeeModel::from_series(*s);
    p.fee_per = fm.fee_per_contract(req.price, !req.post_only);
    if (p.p_model > 0) {
      double pw = req.outcome_side == "yes" ? p.p_model : 1.0 - p.p_model;
      p.net_edge = pw - req.price - p.fee_per;
    }
  }
  if (note.rfind("hedge for ", 0) == 0) p.entry_kind = "hedge";
  EntryContext admission = entry_context(store_, cfg_, p, mk);
  p.entry_kind = admission.kind;
  p.required_edge = admission.required_edge;
  p.approval_required = admission.approval_required;
  p.decision_id = decision_id;
  store_.insert_proposal(p);
  try {
    submit(p,req.client_order_id,deadline);
  } catch (const std::exception& e) {
    // A rejected hand ticket used to remain pending and block every later order in the event.
    // The send did not happen, so make that terminal and keep the reason in the audit trail.
    if(p.status!="unknown")p.status = "failed";
    p.note += " | not sent: " + std::string(e.what());
    store_.update_proposal(p);
    throw;
  }
  return p;
}

void Executor::refresh_account() {
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  if (auto written = store_.kv_get("last_write_ts")) {
    // Give the exchange's user-data feed a moment to reflect our last write. This is a bounded
    // wait, not a refusal: a throw here used to trip the kill switch on every cancel-then-submit.
    long long ts=std::stoll(*written);
    bool fresh = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
      fresh = now_s() - ts >= 3;
      if (fresh) {
        try {
          auto freshness=client_.call("GET", "/exchange/user_data_timestamp");
          long long as_of = parse_iso(jstr(freshness,"as_of_time"));
          fresh = as_of > ts;
        } catch (const std::exception& e) {
          LOG_WARN("user_data_timestamp: %s", e.what());
          fresh = false;
        }
      }
      if (fresh) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!fresh) throw std::runtime_error("Exchange account data has not caught up with the last order; retry after reconciliation");
  }
  auto raw = client_.call("GET", "/portfolio/balance");
  Balance b = parse_balance(raw);
  // Orders first: a fill between these reads may be counted twice, but must not disappear from both.
  auto orders = client_.get_orders("resting");
  auto all_positions = client_.get_positions(true);
  std::vector<Position> positions;
  for (const auto& p : all_positions) {
    if (p.position != 0) positions.push_back(p);
    // Realized P&L ledger: Kalshi's own realized number per market (gross; fees separate), kept
    // for flat markets too. It is the authoritative P&L for every market it covers, including
    // the settlement once that happens; the settlements table is only used for markets that
    // predate the ledger.
    store_.exec("INSERT INTO realized(ticker,realized_pnl,fees_paid,updated_ts) VALUES(?,?,?,?) ON CONFLICT(ticker) DO UPDATE SET "
                "realized_pnl=excluded.realized_pnl, fees_paid=excluded.fees_paid, updated_ts=excluded.updated_ts",
                {p.ticker, p.realized_pnl, p.fees_paid, now_s()});
  }
  // Reference data is required to aggregate external and cross-shard holdings correctly.
  std::set<std::string> tickers;
  for (const auto& p:positions) tickers.insert(p.ticker);
  for (const auto& o:orders) tickers.insert(o.ticker);
  for (const auto& t:tickers) {
    auto m=client_.get_market(t); store_.upsert_market(m);
    if(!store_.get_series(m.series_ticker)) store_.upsert_series(client_.get_series_one(m.series_ticker));
  }
  long long now=now_s();
  store_.replace_risk_snapshot(b,positions,orders,raw.value("balance_breakdown",json::array()),now);
}

// Keep configured shards funded: Kalshi keeps crypto/commodities (2) and some sports (3) on
// separate exchange shards that need their own collateral. Moves cash from shard 0 through the
// intra-account transfer; never touches anything outside the account.
void Executor::topup_shards() {
  if (cfg_.risk.shard_topup.empty() || !client_.authenticated()) return;
  json shards = json::parse(store_.kv_get("shard_balances").value_or("[]"), nullptr, false);
  if (!shards.is_array()) return;
  std::map<int, double> bal;
  for (const auto& sh : shards) bal[sh.value("exchange_index", 0)] = std::atof(sh.value("balance", "0").c_str());
  long long now = now_s();
  // The richest shard funds the others. Shard 0 used to be assumed to be that shard, and to be
  // the only legal target, so an account whose cash had drifted to shard 3 could not fund
  // anything and could not be funded: shard 0 held $0.25 against $65.29 on shard 3 while this
  // logged "shard 2 needs $8.82 but shard 0 has only $0.00 to spare" every ten minutes.
  const double keep = 5.0;              // never drain the shard being borrowed from
  auto richest_other = [&](int target_shard) {
    int best = -1; double spare = 0;
    for (const auto& [idx, amount] : bal) {
      if (idx == target_shard) continue;
      if (amount - keep > spare) { spare = amount - keep; best = idx; }
    }
    return std::make_pair(best, spare);
  };
  for (const auto& [shard, target] : cfg_.risk.shard_topup) {
    if (target <= 0) continue;
    double have = bal.count(shard) ? bal[shard] : 0.0;
    double deficit = target - have;
    if (deficit < 1.0) continue;  // within a dollar of the target
    long long last = std::stoll(store_.kv_get("topup_ts:" + std::to_string(shard)).value_or("0"));
    if (now - last < 600) continue;  // transfers are asynchronous; do not stack them
    auto [from, can] = richest_other(shard);
    double amount = std::floor(std::min(deficit, can) * 100.0) / 100.0;
    if (from < 0 || amount < 1.0) {
      store_.log_event("warn", "shard " + std::to_string(shard) + " needs $" + fmt2(deficit) +
                                   " but no other shard has more than $" + fmt2(keep) + " to spare" +
                                   (from >= 0 ? " (best is shard " + std::to_string(from) + " with $" +
                                                    fmt2(std::max(0.0, can)) + ")" : ""));
      continue;
    }
    try {
      std::string id = client_.transfer_between_shards(from, shard, amount);
      store_.kv_set("topup_ts:" + std::to_string(shard), std::to_string(now));
      store_.log_event("warn", "moved $" + fmt2(amount) + " from shard " + std::to_string(from) +
                                   " to shard " + std::to_string(shard) + " (transfer " + id + ")");
      LOG_WARN("topup: $%.2f shard %d -> %d (%s)", amount, from, shard, id.c_str());
    } catch (const std::exception& e) {
      store_.log_event("error", "shard top-up failed: " + std::string(e.what()));
      LOG_WARN("topup failed: %s", e.what());
      store_.kv_set("topup_ts:" + std::to_string(shard), std::to_string(now));  // back off
    }
  }
}

void Executor::reconcile(bool full) {
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  if (!client_.authenticated()) return;
  long long now = now_s();
  try {
    refresh_account();
    for (const auto& f : client_.get_cashflows())
      store_.exec("INSERT OR REPLACE INTO cashflows(id,ts,kind,amount) VALUES(?,?,?,?)",
                  {f.value("id", ""), f.value("ts", 0LL), f.value("kind", ""), f.value("amount", 0.0)});
    topup_shards();
  } catch (const std::exception& e) {
    store_.kv_set("portfolio_sync_ts", "0");
    store_.log_event("error", std::string("Account sync incomplete; entries blocked: ")+e.what());
    throw;
  }
  try {
    long long min_ts = now - 7 * 86400;
    if (full) min_ts = 0;
    bool history_complete = true;
    for (const char* st : {"executed", "canceled"}) {
      json j = client_.call("GET", "/portfolio/orders",
                            {{"limit", "1000"}, {"status", st}, {"min_ts", std::to_string(min_ts)}});
      const json& page = j.value("orders", json::array());
      // A full page, or a cursor, means there is more history than we read: absence from what we
      // did read would then prove nothing.
      if (page.size() >= 1000 || !j.value("cursor", "").empty()) history_complete = false;
      for (const auto& oj : page) store_.upsert_order(parse_order(oj), 0);
    }
    if (auto id=store_.kv_get("uncertain_client_order_id")) {
      if (!id->empty()) {
        bool found = !store_.query("SELECT order_id FROM orders WHERE client_order_id=?",{*id}).empty();
        long long since = std::stoll(store_.kv_get("uncertain_since").value_or("0"));
        if (found) {
          store_.kv_set("uncertain_client_order_id", "");
          store_.kv_set("uncertain_since", "0");
          store_.log_event("info", "uncertain order located; entries unblocked");
        } else if (history_complete && since > 0 && min_ts < since && now - since >= 120) {
          // Absence is only evidence when we have actually looked properly: a complete order
          // history reaching back before the submission, read at least two minutes after it. An
          // order that never reached the exchange leaves no trace, and without this the desk
          // blocks its own entries for ever and can only be freed by hand.
          store_.kv_set("uncertain_client_order_id", "");
          store_.kv_set("uncertain_since", "0");
          store_.log_event("warn", "uncertain order " + id->substr(0, 8) +
                                       " is absent from a complete order history taken " +
                                       std::to_string((now - since) / 60) +
                                       " min after it was sent: treating it as never placed");
        }
      }
    }
    // Anything we thought was resting but no longer is: mark from the exchange's view.
    store_.exec("UPDATE orders SET status='done' WHERE status='unknown'");
    // Proposals whose orders finished.
    store_.exec(
        "UPDATE proposals SET status=CASE WHEN o.fill_count>0 THEN 'filled' ELSE 'canceled' END,"
        "updated_ts=? FROM orders o WHERE proposals.order_id=o.order_id "
        "AND proposals.status IN ('submitted','cancel_pending') "
        "AND o.status IN ('executed','canceled','done')",
        {now});
  } catch (const std::exception& e) {
    LOG_WARN("reconcile orders: %s", e.what());
  }
  try {
    long long since = 0;
    if (auto v = store_.kv_get("fills_since")) since = std::stoll(*v);
    if (full) since = 0;
    auto fills = client_.get_fills(since > 60 ? since - 60 : 0);
    long long maxts = since;
    for (const auto& f : fills) {
      bool fresh = f.ts > since;
      store_.upsert_fill(f);
      if (fresh && since > 0)
        store_.log_event("info", "FILL " + f.ticker + " " + f.outcome_side + " x" + fmt2(f.count) + " @ " +
                                     fmt2(f.outcome_side == "yes" ? f.yes_price : f.no_price) + (f.is_taker ? " T" : " M"));
      maxts = std::max(maxts, f.ts);
    }
    if (maxts > 0) store_.kv_set("fills_since", std::to_string(maxts));
  } catch (const std::exception& e) {
    LOG_WARN("reconcile fills: %s", e.what());
  }
  try {
    long long since = 0;
    if (auto v = store_.kv_get("settlements_since")) since = std::stoll(*v);
    if (full) since = 0;
    auto sts = client_.get_settlements(since > 3600 ? since - 3600 : 0);
    std::vector<std::string> tickers;
    for (const auto& s : sts) {
      store_.upsert_settlement(s);
      tickers.push_back(s.ticker);
    }
    if (!sts.empty()) store_.kv_set("settlements_since", std::to_string(now));
    // Pull final results for settled markets we have not recorded a result for.
    std::vector<std::string> need;
    for (const auto& t : tickers) {
      auto m = store_.get_market(t);
      if (!m || m->result.empty()) need.push_back(t);
    }
    if (!need.empty())
      for (const auto& m : client_.get_markets_by_tickers(need)) store_.upsert_market(m);
  } catch (const std::exception& e) {
    LOG_WARN("reconcile settlements: %s", e.what());
  }
}

}  // namespace desk
