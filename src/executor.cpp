#include "desk/executor.h"

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

std::string Executor::submit(Proposal& p) {
  if (p.strategy == "close") throw std::runtime_error("A close proposal cannot be approved as new entries; use the reduce-only Close path");
  if (is_exit_strategy(p.strategy)) return submit_exit(p);
  if (!client_.authenticated()) throw std::runtime_error("no API key configured; cannot submit");
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  Market market = client_.get_market(p.ticker);
  store_.upsert_market(market);
  store_.upsert_series(client_.get_series_one(market.series_ticker));
  refresh_account();
  validate_entry(store_, cfg_, p, market);
  // Fresh from the exchange: never add to or flip a position from a strategy proposal (the
  // engine's own check reads a snapshot that can be up to a reconcile interval old).
  if (p.strategy != "ticket") {
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
  if (cost > available + 1e-8) {
    // Kalshi keeps collateral per exchange shard: crypto and commodities on 2, some sports on 3.
    // A shard you have never funded refuses every order in its whole category, which is a strange
    // way to discover that MLB lives somewhere else. Move the shortfall from shard 0 and say so;
    // the transfer is asynchronous, so this order still does not go.
    std::string extra = ". Allocate collateral in Kalshi Account > Exchange indexes. No order sent.";
    if (cfg_.risk.shard_topup_on_demand && market.exchange_index != 0 && client_.authenticated()) {
      long long now = now_s();
      std::string key = "ondemand_ts:" + std::to_string(market.exchange_index);
      long long last = std::stoll(store_.kv_get(key).value_or("0"));
      if (now - last >= 60) {
        store_.kv_set(key, std::to_string(now));
        try {
          auto s0 = parse_balance(client_.call("GET", "/portfolio/balance", {{"exchange_index", "0"}}));
          double keep = 5.0;                                  // never drain the default shard
          double want = std::min(cfg_.risk.shard_topup_max, std::max(cost - available, 1.0) + 2.0);
          double can = std::min(want, s0.balance - keep);
          if (can >= 1.0) {
            std::string id = client_.transfer_between_shards(0, market.exchange_index, can);
            store_.log_event("warn", "shard " + std::to_string(market.exchange_index) + " was short for " + p.ticker +
                                         ": moved $" + fmt2(can) + " from shard 0 (transfer " + id + ")");
            extra = ". Moved $" + fmt2(can) + " there from shard 0 (transfer " + id.substr(0, 8) +
                    "); Kalshi settles that in a few seconds, so click again shortly. No order sent yet.";
          } else {
            extra = ". Shard 0 has only $" + fmt2(std::max(0.0, s0.balance - keep)) +
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
  req.client_order_id = uuid4();
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
    LOG_INFO("submitted %s %s %s x%.2f @ %.2f -> %s (filled %.2f)", p.strategy.c_str(),
             p.outcome_side.c_str(), p.ticker.c_str(), req.count, req.price, r.order_id.c_str(),
             r.fill_count);
    store_.log_event("info", "order " + p.strategy + " " + p.outcome_side + " " + p.ticker + " x" + fmt2(req.count) +
                                 " @ " + fmt2(req.price) + (r.fill_count > 0 ? " filled " + fmt2(r.fill_count) : " resting"));
    return r.order_id;
  } catch (const ApiError& e) {
    p.status = e.status >= 500 ? "unknown" : "failed";
    if(p.status=="unknown") store_.kv_set("uncertain_client_order_id", req.client_order_id);
    p.note = std::string("api ") + std::to_string(e.status) + ": " + e.body.substr(0, 200);
    store_.update_proposal(p);
    store_.log_event("error", "REJECTED " + p.ticker + " " + p.outcome_side + ": " + e.body.substr(0, 120));
    throw;
  } catch (const std::exception& e) {
    p.status="unknown"; p.note=std::string("Order outcome uncertain; reconcile before retry: ")+e.what();
    store_.update_proposal(p); store_.kv_set("uncertain_client_order_id",req.client_order_id);
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
  req.reduce_only = true;
  req.post_only = (p.exec_style == "maker");
  req.tif = (p.exec_style == "maker") ? "good_till_canceled" : "immediate_or_cancel";
  req.client_order_id = uuid4();
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
    p.note = std::string("api ") + std::to_string(e.status) + ": " + e.body.substr(0, 200);
    store_.update_proposal(p);
    store_.log_event("error", "EXIT REJECTED " + p.ticker + ": " + e.body.substr(0, 120));
    throw;
  } catch (const std::exception& e) {
    p.status = "unknown";
    p.note = std::string("Exit outcome uncertain; reconcile before retry: ") + e.what();
    store_.update_proposal(p);
    store_.kv_set("uncertain_client_order_id", req.client_order_id);
    throw;
  }
}

json Executor::cancel(const std::string& order_id) {
  std::lock_guard<std::recursive_mutex> lock(order_mu_);
  auto known = store_.query("SELECT ticker FROM orders WHERE order_id=?", {order_id});
  if (known.empty()) throw std::runtime_error("Order ticker unavailable; sync before canceling");
  WriteWatermark watermark(store_);
  json r = client_.cancel_order(order_id, known[0]["ticker"].get<std::string>());
  store_.log_event("info", "canceled " + order_id.substr(0, 8));
  auto rows = store_.query("SELECT * FROM orders WHERE order_id=?", {order_id});
  if (!rows.empty()) {
    store_.exec("UPDATE orders SET status='canceled',updated_ts=? WHERE order_id=?", {now_s(), order_id});
    long long pid = store_.proposal_for_order(order_id);
    if (pid > 0) {
      if (auto p = store_.get_proposal(pid)) {
        double filled = rows[0].value("fill_count", 0.0);
        p->status = filled > 0 ? "filled" : "canceled";
        if (filled > 0) p->note += " | partial " + fmt2(filled) + ", rest canceled";
        store_.update_proposal(*p);
      }
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

Proposal Executor::ticket(const OrderRequest& req, const std::string& note) {
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
  store_.insert_proposal(p);
  submit(p);
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
  for (const auto& [shard, target] : cfg_.risk.shard_topup) {
    if (shard == 0 || target <= 0) continue;
    double have = bal.count(shard) ? bal[shard] : 0.0;
    double deficit = target - have;
    if (deficit < 1.0) continue;  // within a dollar of the target
    long long last = std::stoll(store_.kv_get("topup_ts:" + std::to_string(shard)).value_or("0"));
    if (now - last < 600) continue;  // transfers are asynchronous; do not stack them
    double keep = 5.0;  // never drain shard 0 below this
    double can = bal.count(0) ? bal[0] - keep : 0.0;
    double amount = std::floor(std::min(deficit, can) * 100.0) / 100.0;
    if (amount < 1.0) {
      store_.log_event("warn", "shard " + std::to_string(shard) + " needs $" + fmt2(deficit) + " but shard 0 has only $" + fmt2(std::max(0.0, can)) + " to spare");
      continue;
    }
    try {
      std::string id = client_.transfer_between_shards(0, shard, amount);
      store_.kv_set("topup_ts:" + std::to_string(shard), std::to_string(now));
      store_.log_event("warn", "moved $" + fmt2(amount) + " from shard 0 to shard " + std::to_string(shard) + " (transfer " + id + ")");
      LOG_WARN("topup: $%.2f shard 0 -> %d (%s)", amount, shard, id.c_str());
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
    for (const char* st : {"executed", "canceled"}) {
      json j = client_.call("GET", "/portfolio/orders",
                            {{"limit", "1000"}, {"status", st}, {"min_ts", std::to_string(min_ts)}});
      for (const auto& oj : j.value("orders", json::array())) store_.upsert_order(parse_order(oj), 0);
    }
    if (auto id=store_.kv_get("uncertain_client_order_id")) {
      if (!id->empty()) {
        bool found = !store_.query("SELECT order_id FROM orders WHERE client_order_id=?",{*id}).empty();
        // Elapsed time and an incomplete history page do not prove an ambiguous order failed.
        if (found) {
          store_.kv_set("uncertain_client_order_id", "");
          store_.kv_set("uncertain_since", "0");
          store_.log_event("info", "uncertain order located; entries unblocked");
        }
      }
    }
    // Anything we thought was resting but no longer is: mark from the exchange's view.
    store_.exec("UPDATE orders SET status='done' WHERE status='unknown'");
    // Proposals whose orders finished.
    store_.exec(
        "UPDATE proposals SET status=CASE WHEN o.fill_count>0 THEN 'filled' ELSE 'canceled' END,"
        "updated_ts=? FROM orders o WHERE proposals.order_id=o.order_id AND proposals.status='submitted' "
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
