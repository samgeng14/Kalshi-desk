#include <algorithm>
#include "desk/client.h"

#include <cmath>

#include <chrono>
#include <thread>

#include "desk/fixed.h"
#include "desk/log.h"

namespace desk {

KalshiClient::KalshiClient(std::string base, std::shared_ptr<Signer> signer, std::string user_agent)
    : base_(std::move(base)), signer_(std::move(signer)), http_(std::move(user_agent), 20) {
  auto pos = base_.find("/trade-api");
  prefix_ = pos == std::string::npos ? "" : base_.substr(pos);
}

// Every exchange call waits its turn here, whichever path it comes from (call() or the batch
// orderbook request), so the pacing and its 429 adaptation cover all of them.
void KalshiClient::pace() {
  // A monotonic clock: if the wall clock stepped backward, "time since the last call" would go
  // negative and every exchange call from every thread would wait out the difference.
  std::lock_guard<std::mutex> lk(mu_);
  auto mono = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  long long now = mono();
  if (interval_ms_ > 0 && now - last_call_ms_ < interval_ms_)
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_ - (now - last_call_ms_)));
  last_call_ms_ = mono();
}

void KalshiClient::note_status(long status) {
  HttpResponse r;
  r.status = status;
  std::lock_guard<std::mutex> lk(mu_);
  if (r.status == 429) {
    const int before = interval_ms_;
    interval_ms_ = std::min(std::max(1000, min_interval_ms_), std::max(interval_ms_ * 2, 100));
    if (interval_ms_ != before)
      LOG_WARN("rate limited: spacing exchange calls %dms -> %dms", before, interval_ms_);
  } else if (r.ok() && interval_ms_ > min_interval_ms_) {
    // 5% per success: back from a second to the floor in about 55 calls (a few seconds).
    interval_ms_ = std::max(min_interval_ms_, static_cast<int>(interval_ms_ * 0.95));
  }
}

json KalshiClient::call(const std::string& method, const std::string& path,
                        const std::map<std::string, std::string>& query, const json* body) {
  std::string url = base_ + path;
  bool first = true;
  for (const auto& [k, v] : query) {
    url += (first ? "?" : "&") + k + "=" + url_encode(v);
    first = false;
  }
  HttpResponse r;
  // Reads are retried on 429 / 5xx with backoff. Writes (orders) are never retried blindly:
  // a timeout on POST may still have placed the order, and reconcile will pick it up.
  int attempts = method == "GET" ? 4 : 1;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    std::map<std::string, std::string> headers;
    if (signer_) headers = signer_->headers(method, prefix_ + path, now_ms());
    // Pacing, shared by every thread that calls the exchange: the sleep happens under the lock
    // on purpose, so concurrent callers queue behind it instead of all firing at once.
    pace();
    if (method == "GET") r = http_.get(url, headers);
    else if (method == "POST") r = http_.post(url, body ? body->dump() : "{}", headers);
    else if (method == "DELETE") r = http_.del(url, headers);
    else throw std::runtime_error("unsupported method " + method);
    note_status(r.status);
    if (r.ok()) break;
    bool retryable = r.status == 429 || r.status >= 500;
    if (!retryable || attempt == attempts) break;
    int wait_ms = 700 * attempt * attempt;
    LOG_WARN("%s %s -> HTTP %ld, retry %d in %dms", method.c_str(), path.c_str(), r.status, attempt, wait_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  }

  if (!r.ok()) {
    throw ApiError(r.status, r.body, method + " " + path + " -> HTTP " + std::to_string(r.status) + " " +
                                         r.body.substr(0, 300));
  }
  if (r.body.empty()) return json::object();
  return json::parse(r.body, nullptr, false);
}

// ---- market data --------------------------------------------------------------

std::vector<Series> KalshiClient::get_series(const std::string& category) {
  std::vector<Series> out;
  std::map<std::string, std::string> q{{"limit", "1000"}};
  if (!category.empty()) q["category"] = category;
  for (int page = 0; page < 30; ++page) {
    json j = call("GET", "/series", q);
    for (const auto& s : j.value("series", json::array())) out.push_back(parse_series(s));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

Series KalshiClient::get_series_one(const std::string& ticker) {
  json j = call("GET", "/series/" + ticker);
  return parse_series(j.contains("series") ? j["series"] : j);
}

std::vector<Market> KalshiClient::get_markets(const std::string& series_ticker,
                                              const std::string& status, int max_pages) {
  std::vector<Market> out;
  std::map<std::string, std::string> q{{"limit", "1000"}, {"series_ticker", series_ticker}};
  if (!status.empty()) q["status"] = status;
  for (int page = 0; page < max_pages; ++page) {
    json j = call("GET", "/markets", q);
    for (const auto& m : j.value("markets", json::array())) out.push_back(parse_market(m));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

std::vector<Market> KalshiClient::get_markets_by_event(const std::string& event_ticker) {
  std::vector<Market> out;
  json j = call("GET", "/markets", {{"limit", "1000"}, {"event_ticker", event_ticker}});
  for (const auto& m : j.value("markets", json::array())) out.push_back(parse_market(m));
  return out;
}

KalshiClient::EventMeta KalshiClient::get_event(const std::string& event_ticker) {
  EventMeta e;
  e.event_ticker = event_ticker;
  json j = call("GET", "/events/" + event_ticker, {{"with_nested_markets", "false"}});
  const json& ev = j.contains("event") && j["event"].is_object() ? j["event"] : j;
  if (!ev.is_object() || !ev.contains("event_ticker")) return e;
  e.found = true;
  e.title = jstr(ev, "title");
  e.series_ticker = jstr(ev, "series_ticker");
  const json& mx = ev.contains("mutually_exclusive") ? ev["mutually_exclusive"] : json();
  e.mutually_exclusive = mx.is_boolean() && mx.get<bool>();
  return e;
}

std::vector<Market> KalshiClient::get_markets_by_tickers(const std::vector<std::string>& tickers) {
  std::vector<Market> out;
  for (size_t i = 0; i < tickers.size(); i += 100) {
    std::string list;
    for (size_t k = i; k < tickers.size() && k < i + 100; ++k) {
      if (!list.empty()) list += ",";
      list += tickers[k];
    }
    json j = call("GET", "/markets", {{"limit", "1000"}, {"tickers", list}});
    for (const auto& m : j.value("markets", json::array())) out.push_back(parse_market(m));
  }
  return out;
}

Market KalshiClient::get_market(const std::string& ticker) {
  json j = call("GET", "/markets/" + ticker);
  return parse_market(j.contains("market") ? j["market"] : j);
}

Orderbook KalshiClient::get_orderbook(const std::string& ticker, int depth) {
  std::map<std::string, std::string> q;
  if (depth > 0) q["depth"] = std::to_string(depth);
  json j = call("GET", "/markets/" + ticker + "/orderbook", q);
  return parse_orderbook(j, ticker);
}

std::vector<Orderbook> KalshiClient::get_orderbooks(const std::vector<std::string>& tickers) {
  std::vector<Orderbook> out;
  for (size_t i = 0; i < tickers.size(); i += 100) {
    std::string url = base_ + "/markets/orderbooks";
    std::string qs;
    for (size_t k = i; k < tickers.size() && k < i + 100; ++k)
      qs += (qs.empty() ? "?" : "&") + std::string("tickers=") + url_encode(tickers[k]);
    std::map<std::string, std::string> headers;
    if (signer_) headers = signer_->headers("GET", prefix_ + "/markets/orderbooks", now_ms());
    pace();
    HttpResponse r = http_.get(url + qs, headers);
    note_status(r.status);
    if (!r.ok()) throw ApiError(r.status, r.body, "GET /markets/orderbooks -> " + std::to_string(r.status));
    json j = json::parse(r.body, nullptr, false);
    for (const auto& ob : j.value("orderbooks", json::array()))
      out.push_back(parse_orderbook(ob, jstr(ob, "ticker")));
  }
  return out;
}

std::vector<Trade> KalshiClient::get_trades(const std::string& ticker, long long min_ts,
                                            long long max_ts, int max_pages) {
  std::vector<Trade> out;
  std::map<std::string, std::string> q{{"limit", "1000"}};
  if (!ticker.empty()) q["ticker"] = ticker;
  if (min_ts > 0) q["min_ts"] = std::to_string(min_ts);
  if (max_ts > 0) q["max_ts"] = std::to_string(max_ts);
  for (int page = 0; page < max_pages; ++page) {
    json j = call("GET", "/markets/trades", q);
    for (const auto& t : j.value("trades", json::array())) out.push_back(parse_trade(t));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

// ---- portfolio -------------------------------------------------------------------

Balance KalshiClient::get_balance() { return parse_balance(call("GET", "/portfolio/balance")); }

std::vector<Position> KalshiClient::get_positions(bool include_flat) {
  std::vector<Position> out;
  std::map<std::string, std::string> q{{"limit", "1000"}, {"count_filter", "position"}};
  for (int page = 0; page < 10; ++page) {
    json j = call("GET", "/portfolio/positions", q);
    for (const auto& p : j.value("market_positions", json::array())) {
      Position pos = parse_position(p);
      if (pos.position != 0 || (include_flat && (pos.realized_pnl != 0 || pos.fees_paid != 0))) out.push_back(pos);
    }
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

std::vector<Order> KalshiClient::get_orders(const std::string& status, const std::string& ticker) {
  std::vector<Order> out;
  std::map<std::string, std::string> q{{"limit", "1000"}};
  if (!status.empty()) q["status"] = status;
  if (!ticker.empty()) q["ticker"] = ticker;
  for (int page = 0; page < 10; ++page) {
    json j = call("GET", "/portfolio/orders", q);
    for (const auto& o : j.value("orders", json::array())) out.push_back(parse_order(o));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

std::vector<Fill> KalshiClient::get_fills(long long min_ts, int max_pages) {
  std::vector<Fill> out;
  std::map<std::string, std::string> q{{"limit", "1000"}};
  if (min_ts > 0) q["min_ts"] = std::to_string(min_ts);
  for (int page = 0; page < max_pages; ++page) {
    json j = call("GET", "/portfolio/fills", q);
    for (const auto& f : j.value("fills", json::array())) out.push_back(parse_fill(f));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

std::vector<Settlement> KalshiClient::get_settlements(long long min_ts, int max_pages) {
  std::vector<Settlement> out;
  std::map<std::string, std::string> q{{"limit", "1000"}};
  if (min_ts > 0) q["min_ts"] = std::to_string(min_ts);
  for (int page = 0; page < max_pages; ++page) {
    json j = call("GET", "/portfolio/settlements", q);
    for (const auto& s : j.value("settlements", json::array())) out.push_back(parse_settlement(s));
    std::string cursor = j.value("cursor", "");
    if (cursor.empty()) break;
    q["cursor"] = cursor;
  }
  return out;
}

// ---- orders ------------------------------------------------------------------------

json KalshiClient::order_body(const OrderRequest& req) {
  // V2 vocabulary is in YES-book terms: bid = buy YES (== sell NO),
  // ask = sell YES (== buy NO). Price is always the YES price.
  bool yes = req.outcome_side == "yes";
  double yes_price = yes ? req.price : 1.0 - req.price;
  json b{{"ticker", req.ticker},
         {"side", yes ? "bid" : "ask"},
         {"count", fmt_fp(req.count)},
         {"price", fmt_dollars(yes_price)},
         {"time_in_force", req.tif},
         {"self_trade_prevention_type", "taker_at_cross"},
         {"post_only", req.post_only}, {"exchange_index", req.exchange_index}};
  if (req.reduce_only) b["reduce_only"] = true;
  if (!req.client_order_id.empty()) b["client_order_id"] = req.client_order_id;
  return b;
}

OrderResponse KalshiClient::create_order(const OrderRequest& req) {
  if (!signer_) throw std::runtime_error("create_order: no API key configured");
  json body = order_body(req);
  LOG_INFO("order -> %s", body.dump().c_str());
  json j = call("POST", "/portfolio/events/orders", {}, &body);
  return parse_order_response(j);
}

json KalshiClient::get_cashflows() {
  json out = json::array();
  auto add = [&](const std::string& id, long long ts, const char* kind, double amount) {
    if (id.empty() || amount == 0) return;
    out.push_back({{"id", id}, {"ts", ts}, {"kind", kind}, {"amount", amount}});
  };
  try {
    json j = call("GET", "/portfolio/intra_exchange_instance_transfers", {{"limit", "200"}});
    for (const auto& t : j.value("transfers", json::array())) {
      if (t.value("status", "") != "complete") continue;
      std::string src = t.value("source", ""), dst = t.value("destination", "");
      double amt = parse_num(t.value("amount", "0"));
      if (src == "event_contract" && dst == "margined") add(t.value("transfer_id", ""), t.value("created_ts", 0LL), "to_margined", -amt);
      else if (src == "margined" && dst == "event_contract") add(t.value("transfer_id", ""), t.value("created_ts", 0LL), "from_margined", amt);
      // event_contract -> event_contract (shard to shard) stays inside the balance we read: no flow.
    }
  } catch (const std::exception& e) { LOG_WARN("transfers: %s", e.what()); }
  try {
    json j = call("GET", "/portfolio/deposits", {{"limit", "100"}});
    for (const auto& d : j.value("deposits", json::array())) {
      if (d.value("status", "") != "applied") continue;
      add(d.value("id", ""), d.value("created_ts", 0LL), "deposit", d.value("amount_cents", 0.0) / 100.0);
    }
  } catch (const std::exception& e) { LOG_WARN("deposits: %s", e.what()); }
  try {
    json j = call("GET", "/portfolio/withdrawals", {{"limit", "100"}});
    for (const auto& w : j.value("withdrawals", json::array())) {
      std::string st = w.value("status", "");
      if (st == "cancelled" || st == "canceled" || st == "failed") continue;
      add(w.value("id", ""), w.value("created_ts", 0LL), "withdrawal", -w.value("amount_cents", 0.0) / 100.0);
    }
  } catch (const std::exception& e) { LOG_WARN("withdrawals: %s", e.what()); }
  return out;
}

std::string KalshiClient::transfer_between_shards(int from_shard, int to_shard, double dollars) {
  if (!signer_) throw std::runtime_error("transfer: no API key configured");
  if (dollars <= 0 || from_shard == to_shard) throw std::runtime_error("transfer: bad amount or shards");
  json body{{"source", "event_contract"}, {"destination", "event_contract"},
            {"amount", static_cast<long long>(std::llround(dollars * 10000.0))},  // centicents
            {"source_exchange_shard", from_shard}, {"destination_exchange_shard", to_shard}};
  json r = call("POST", "/portfolio/intra_exchange_instance_transfer", {}, &body);
  return r.value("transfer_id", "");
}

json KalshiClient::cancel_order(const std::string& order_id, const std::string& ticker) {
  if (!signer_) throw std::runtime_error("cancel_order: no API key configured");
  if (ticker.empty()) throw std::runtime_error("cancel requires market ticker for exchange routing");
  return call("DELETE", "/portfolio/events/orders/" + order_id, {{"market_ticker", ticker}, {"exchange_index", "-1"}});
}

}  // namespace desk
