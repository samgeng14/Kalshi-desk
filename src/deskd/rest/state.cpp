#include "deskd/rest/state.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <limits>
#include <sstream>
#include <thread>

#include "deskd/app/log.h"
#include "deskd/net/http.h"
#include "deskd/web/contract.h"

namespace deskd::rest {
using json = nlohmann::json;
namespace {
constexpr std::int64_t kDay = 86400000;
const std::array<std::string, 5> kCats{{"SPORTS", "WEATHER", "FIN", "ECON", "OTHER"}};

std::string upper(std::string text) {
  for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}
bool starts(const std::string& text, const std::string& prefix) { return text.rfind(prefix, 0) == 0; }
bool any_prefix(const std::string& text, std::initializer_list<const char*> prefixes) {
  for (const char* prefix : prefixes) if (starts(text, prefix)) return true;
  return false;
}
bool is_open(const kalshi::MarketRow& m) { return m.status == "open" || m.status == "active"; }
std::string series(const kalshi::MarketRow& m) { return m.ticker.substr(0, m.ticker.find('-')); }
std::string event(const kalshi::MarketRow& m) {
  if (!m.event_ticker.empty()) return m.event_ticker;
  const auto dash = m.ticker.rfind('-');
  return dash == std::string::npos ? m.ticker : m.ticker.substr(0, dash);
}
std::string label(const kalshi::MarketRow& m) {
  const auto& value = !m.yes_sub_title.empty() ? m.yes_sub_title : !m.subtitle.empty() ? m.subtitle : m.ticker;
  return value.substr(0, 24);
}
std::string kind(const kalshi::MarketRow& m) {
  if (category(m) == "WEATHER") return "WEATHER";
  if (category(m) == "SPORTS") return "GAME";
  // A 15-minute ticker alone is not evidence that it pays a Liquidity Incentive Program.
  return "OTHER";
}
std::string group(const kalshi::MarketRow& m) { return event(m); }
std::int64_t effective_close(const kalshi::MarketRow& m) {
  // expected_expiration_time is an outcome/settlement estimate, never the trading cutoff.
  return m.close_ms.value_or(0);
}
json optional_px(std::optional<Px> px) { return px ? json(kalshi::price_wire(*px)) : json(nullptr); }
json fee(const kalshi::MarketRow& market) {
  if(market.fee_multiplier && (market.fee_type=="quadratic" || market.fee_type=="quadratic_with_maker_fees"))
    return {{"type",*market.fee_multiplier==0 ? "FREE" : market.fee_type=="quadratic" ? "QUAD" : "QUAD_MAKER"},
            {"mult",*market.fee_multiplier}};
  return {{"type","QUAD"},{"mult",1}}; // display fallback only; fee_ready() refuses admission
}

template<class Row, class Build, class Parse, class Get>
bool pages(std::vector<Row>* rows, Build build, Parse parse, Get get, int max_pages, std::string* why) {
  std::set<std::string> cursors;
  std::string cursor;
  rows->clear();
  for (int page = 0; page < max_pages; ++page) {
    std::string body;
    if (!get(build(cursor), &body, why)) return false;
    auto result = parse(body, why);
    if (!result) return false;
    rows->insert(rows->end(), result->rows.begin(), result->rows.end());
    if (result->cursor.empty()) return true;
    if (!cursors.insert(result->cursor).second) {
      *why = "pagination cursor repeated; refusing an incomplete snapshot";
      return false;
    }
    cursor = std::move(result->cursor);
  }
  *why = "pagination limit reached; refusing an incomplete snapshot";
  return false;
}

std::optional<std::int64_t> http_date_ms(const std::string& date) {
  std::tm tm{};
  const char* end = ::strptime(date.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  if (!end || *end || tm.tm_year < 100) return std::nullopt;
  return static_cast<std::int64_t>(::timegm(&tm)) * 1000;
}

json universe_row(const kalshi::MarketRow& m, const Snapshot& state, const FairOf& fair_of) {
  // Kalshi uses 0/1 for absent sides on some listings; a zero-sized side is absent as well.
  auto bid = m.yes_bid, ask = m.yes_ask;
  if (bid && (*bid <= 0 || (m.yes_bid_size && *m.yes_bid_size == 0))) bid.reset();
  if (ask && (*ask >= 10000 || (m.yes_ask_size && *m.yes_ask_size == 0))) ask.reset();
  json spread = nullptr;   // not an optional: GCC 13 warns that one may be read uninitialized
  if (bid && ask && *ask >= *bid) spread = kalshi::price_wire(*ask - *bid);
  const auto fair = fair_of ? fair_of(m.ticker) : std::nullopt;
  const auto edge = fair ? take_edge(m, bid, ask, *fair) : std::nullopt;
  return {{"v", "K"}, {"tk", m.ticker}, {"ev", event(m)}, {"title", m.title.empty() ? m.ticker : m.title},
          {"short", label(m)}, {"cat", category(m)}, {"grp", group(m)}, {"gk", group(m)}, {"kind", kind(m)},
          {"date", outcome_date(m)}, {"close_ms", effective_close(m)}, {"bid", optional_px(bid)},
          {"ask", optional_px(ask)}, {"sprd", spread},
          {"vol", static_cast<double>(m.volume_24h.value_or(0)) / 100.0}, {"fee", fee(m)},
          {"fair", fair ? json(price_str(*fair)) : json(nullptr)},
          {"edge", edge ? json(money_str(*edge)) : json(nullptr)}, {"pin", state.pins.count(m.ticker) != 0},
          {"live", state.live.count(m.ticker) != 0}};
}
}  // namespace

void ClockCheck::observe(const std::string& date, std::int64_t sent, std::int64_t received,
                         std::int64_t elapsed) {
  const auto parsed = http_date_ms(date);
  if (received < sent || std::llabs((received - sent) - elapsed) > 100) {
    samples_.clear();  // a local wall-clock step invalidates every earlier bound
    return;
  }
  if (!parsed) return;
  samples_.erase(std::remove_if(samples_.begin(), samples_.end(), [received](const Sample& s) {
    return s.at > received || received - s.at > 60000;
  }), samples_.end());
  const Sample sample{sent - (*parsed + 1000), received - *parsed, received};
  std::int64_t lo = sample.lo, hi = sample.hi;
  for (const auto& old : samples_) {
    const auto drift = (received - old.at) / 1000 + 1;  // conservative 1000ppm between samples
    lo = std::max(lo, old.lo - drift);
    hi = std::min(hi, old.hi + drift);
  }
  if (lo > hi) samples_.clear();  // incompatible server/local clocks: relearn, never average away
  samples_.push_back(sample);
  if (samples_.size() > 64) samples_.erase(samples_.begin());
}

ClockEstimate ClockCheck::estimate(std::int64_t now) const {
  ClockEstimate out;
  std::int64_t lo = std::numeric_limits<std::int64_t>::min(), hi = std::numeric_limits<std::int64_t>::max();
  for (const auto& sample : samples_) {
    if (sample.at > now || now - sample.at > 60000) continue;
    const auto drift = (now - sample.at) / 1000 + 1;
    lo = std::max(lo, sample.lo - drift);
    hi = std::min(hi, sample.hi + drift);
    out.measured_ms = std::max(out.measured_ms, sample.at);
    ++out.samples;
  }
  if (!out.samples || lo > hi) return ClockEstimate{};
  out.offset_ms = lo + (hi - lo) / 2;
  out.uncertainty_ms = (hi - lo + 1) / 2;
  out.post_trusted = lo >= -500 && hi <= 500;
  return out;
}

State::State(net::ConnFactory& factory, net::Endpoint endpoint, const KalshiSigner* signer,
             Callback callback, Options options)
    : factory_(factory), endpoint_(std::move(endpoint)), signer_(signer), callback_(std::move(callback)),
      options_(options), published_(std::make_shared<const Snapshot>()), started_ms_(now_ms()) {
  options_.poll_ms = std::max(100, options_.poll_ms);
  options_.listing_poll_ms = std::max(100, options_.listing_poll_ms);
  options_.timeout_ms = std::max(100, options_.timeout_ms);
  options_.page_size = std::clamp(options_.page_size, 1, 1000);
  options_.max_pages = std::clamp(options_.max_pages, 1, 1000);
}

std::shared_ptr<const Snapshot> State::snapshot() const { return std::atomic_load(&published_); }
void State::request_refresh() { refresh_requested_.store(true); }
void State::set_pins(std::set<std::string> pins) { controls(true, std::move(pins)); }
void State::set_live(std::set<std::string> live) { controls(false, std::move(live)); }
void State::set_held(std::set<std::string> held) {
  std::lock_guard<std::mutex> lock(controls_mu_);
  held_ = std::move(held);
}

void State::controls(bool pins, std::set<std::string> values) {
  std::shared_ptr<const Snapshot> result;
  {
    std::lock_guard<std::mutex> lock(controls_mu_);
    auto& target = pins ? pins_ : live_;
    if (target == values) return;
    target = std::move(values);
    Snapshot next = *std::atomic_load(&published_);
    next.pins = pins_;
    next.live = live_;
    next.live.insert(pins_.begin(), pins_.end());
    for (const auto& p : next.positions) if (p.position) next.live.insert(p.ticker);
    for (const auto& o : next.orders) if (o.status == kalshi::OrderStatus::RESTING && o.remaining) next.live.insert(o.ticker);
    ++next.revision;
    result = std::make_shared<const Snapshot>(std::move(next));
    std::atomic_store(&published_, result);
  }
  if (callback_) callback_(result);
  request_refresh();
}

void State::publish(Snapshot next) {
  std::shared_ptr<const Snapshot> result;
  {
    std::lock_guard<std::mutex> lock(controls_mu_);
    next.pins = pins_;
    next.live = live_;
    next.live.insert(pins_.begin(), pins_.end());
    for (const auto& p : next.positions) if (p.position) next.live.insert(p.ticker);
    for (const auto& o : next.orders) if (o.status == kalshi::OrderStatus::RESTING && o.remaining) next.live.insert(o.ticker);
    next.revision = std::atomic_load(&published_)->revision + 1;
    result = std::make_shared<const Snapshot>(std::move(next));
    std::atomic_store(&published_, result);
  }
  if (callback_) callback_(result);
}

namespace {
// sleeps ms in short slices, beating, and gives up early when the worker stops
bool rest_wait(Worker* worker, std::int64_t ms) {
  const auto until = mono_ms() + ms;
  while (mono_ms() < until) {
    if (worker) { worker->beat(); if (worker->stopping()) return false; }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(50, std::max<std::int64_t>(1, until - mono_ms()))));
  }
  return true;
}
}  // namespace

bool State::get(kalshi::WireRequest request, std::string* body, std::string* why, Worker* worker, int timeout_ms) {
  if (timeout_ms <= 0) timeout_ms = options_.timeout_ms;
  if (worker && worker->stopping()) { *why = "REST worker stopping"; return false; }
  if (request.auth && !signer_) { *why = "demo signing key not loaded"; return false; }
  if (starts(request.path, kalshi::kBasePath)) request.path.replace(0, std::string(kalshi::kBasePath).size(), endpoint_.base_path);
  for (int attempt = 0;; ++attempt) {
    // at most one request every request_gap_ms from this owner (Kalshi's read rate limit)
    if (last_request_mono_ && options_.request_gap_ms > 0 &&
        !rest_wait(worker, last_request_mono_ + options_.request_gap_ms - mono_ms())) {
      *why = "REST worker stopping"; return false;
    }
    if (!conn_ || !conn_->usable()) {
      conn_ = factory_.open(endpoint_, timeout_ms, why);
      if (!conn_) return false;
    }
    const auto auth = request.auth ? signer_->headers(now_ms(), request.method, request.path) : net::HeaderList{};
    if (request.auth && auth.size() != 3) { *why = "request signing failed"; return false; }
    const auto bytes = kalshi::request_bytes(request, endpoint_.host_header(), auth);
    const auto sent = now_ms(), mono = mono_ms();
    last_request_mono_ = mono;
    const auto reply = net::exchange(*conn_, bytes, net::mono_us() + timeout_ms * 1000LL, false,
                                   [worker] { if (worker) worker->beat(); });
    const auto received = now_ms();
    if (!reply.reusable) conn_.reset();
    if (worker) worker->beat();
    if (reply.fate != net::Exchange::Fate::REPLIED) { *why = request.path + ": " + reply.why; return false; }
    // Cached replies' Date is the origin generation time, not a measurement of this round trip.
    if (reply.res.header("age").empty() || reply.res.header("age") == "0")
      clock_.observe(reply.res.header("date"), sent, received, mono_ms() - mono);
    if (reply.res.status == 429 && attempt < options_.retries_429) {
      std::int64_t wait = std::min<std::int64_t>(8000, static_cast<std::int64_t>(options_.backoff_429_ms) << attempt);
      const auto after = reply.res.header("retry-after");
      if (!after.empty() && after.size() < 4 && std::all_of(after.begin(), after.end(), ::isdigit))
        wait = std::max<std::int64_t>(wait, std::min<std::int64_t>(10000, std::stoll(after) * 1000));
      if (!rest_wait(worker, wait)) { *why = "REST worker stopping"; return false; }
      continue;
    }
    if (reply.res.status != 200) {
      *why = request.path + ": HTTP " + std::to_string(reply.res.status);
      return false;  // no response bodies/headers in logs: they can contain account identifiers
    }
    *body = reply.res.body;
    return true;
  }
}

bool State::account(Snapshot& next, std::string* why, Worker* worker) {
  auto fetch = [&](kalshi::WireRequest request, std::string* body, std::string* error) {
    return get(std::move(request), body, error, worker);
  };
  std::string body;
  if (!fetch(kalshi::build_get_exchange_status(), &body, why)) return false;
  auto exchange = kalshi::parse_exchange_status(body, why);
  if (!exchange) return false;
  if (!fetch(kalshi::build_get_balance(), &body, why)) return false;
  auto balance = kalshi::parse_balance(body, why);
  if (!balance) return false;
  kalshi::ListQuery q;
  q.limit = options_.page_size;
  q.count_filter = "position";
  q.settlement_status = "unsettled";
  std::vector<kalshi::PositionRow> positions;
  if (!pages(&positions, [&](const std::string& cursor) { q.cursor = cursor; return kalshi::build_get_positions(q); },
             kalshi::parse_positions, fetch, options_.max_pages, why)) return false;
  q = kalshi::ListQuery{};
  q.limit = options_.page_size;
  q.status = "resting";
  std::vector<kalshi::OrderRow> orders, recent;
  auto orders_builder = [&](const std::string& cursor) { q.cursor = cursor; return kalshi::build_get_orders(q); };
  if (!pages(&orders, orders_builder, kalshi::parse_orders, fetch, options_.max_pages, why)) return false;
  q.status.clear();
  const std::int64_t since = orders_since_ms_ > 0 ? std::min(orders_since_ms_, started_ms_ - 5000) : started_ms_ - 5000;
  q.min_ts = std::max(since, et_midnight_ms(now_ms()) - 86400000) / 1000;
  if (!pages(&recent, orders_builder, kalshi::parse_orders, fetch, options_.max_pages, why)) return false;
  std::map<std::string, kalshi::OrderRow> unique_orders;
  for (auto& row : orders) unique_orders[row.order_id] = std::move(row);
  for (auto& row : recent) unique_orders[row.order_id] = std::move(row);
  orders.clear();
  for (auto& entry : unique_orders) orders.push_back(std::move(entry.second));
  std::vector<kalshi::FillRow> fills;
  // Fills since the earlier of this start and today's ET midnight: the day's P&L is built from them.
  q.min_ts = std::min(started_ms_ - 5000, et_midnight_ms(now_ms())) / 1000;
  if (!pages(&fills, [&](const std::string& cursor) { q.cursor = cursor; return kalshi::build_get_fills(q); },
             kalshi::parse_fills, fetch, options_.max_pages, why)) return false;
  std::map<std::string, kalshi::FillRow> unique_fills;
  for (auto& row : fills) unique_fills[row.trade_id.empty() ? row.fill_id : row.trade_id] = std::move(row);
  fills.clear();
  for (auto& entry : unique_fills) fills.push_back(std::move(entry.second));
  std::set<int> shards{0};
  for (const auto& shard : exchange->shards) shards.insert(shard.exchange_index);
  for (const auto& shard : balance->by_shard) shards.insert(shard.first);
  for (const auto& position : positions) shards.insert(position.exchange_index);
  for (const auto& order : orders) if (order.exchange_index) shards.insert(*order.exchange_index);
  std::map<int, Money> balances;
  for (const auto shard : shards) {
    if (!fetch(kalshi::build_get_balance(shard), &body, why)) return false;
    auto shard_balance = kalshi::parse_balance(body, why);
    if (!shard_balance) return false;
    balances[shard] = shard_balance->available;
  }
  next.exchange = std::move(*exchange);
  next.balance = std::move(*balance);
  next.balances = std::move(balances);
  next.positions = std::move(positions);
  next.orders = std::move(orders);
  next.fills = std::move(fills);
  next.account_ready = true;
  next.account_ms = now_ms();
  return true;
}

bool State::listing(Snapshot& next, std::string* why, Worker* worker) {
  const int deadline = std::max(options_.timeout_ms, options_.listing_timeout_ms);
  auto fetch = [&](kalshi::WireRequest request, std::string* body, std::string* error) {
    return get(std::move(request), body, error, worker, deadline);
  };
  kalshi::MarketsQuery q;
  q.limit = std::max(1, std::min(options_.page_size, options_.listing_page_size));
  q.status = "open";
  q.max_close_ts = (now_ms() + std::max(2, options_.listing_horizon_days) * kDay) / 1000;
  // Multivariate combo markets (KXMVE...) are most of an open listing and not traded here; a
  // position or order in one is still fetched by ticker below.
  q.mve_filter = mve_filter_refused_ ? "" : "exclude";
  std::vector<kalshi::MarketRow> markets;
  auto list = [&] {
    markets.clear();
    return pages(&markets, [&](const std::string& cursor) { q.cursor = cursor; return kalshi::build_get_markets(q); },
                 kalshi::parse_markets, fetch, options_.max_pages, why);
  };
  if (!list()) {
    if (mve_filter_refused_ || why->find(": HTTP 400") == std::string::npos) return false;
    mve_filter_refused_ = true;   // the venue refused the combination: list everything instead
    DLOG_WARN("Kalshi refused mve_filter=exclude on the market listing; listing without it");
    q.mve_filter.clear();
    q.cursor.clear();
    if (!list()) return false;
  }
  std::map<std::string, kalshi::MarketRow> rows;
  for (auto& market : markets) rows[market.ticker] = std::move(market);
  // A position/order can outlive the public OPEN listing. Fetch its actual status rather than
  // dropping it (or guessing settled from the wall clock). Include old live markets for the
  // contract's final live=false/closed transition and its 30-minute retention rule.
  std::set<std::string> needed = snapshot()->live, held;
  {
    std::lock_guard<std::mutex> lock(controls_mu_);
    held = held_;
  }
  for (const auto& p : next.positions) if (p.position) needed.insert(p.ticker);
  for (const auto& o : next.orders) if (o.status == kalshi::OrderStatus::RESTING && o.remaining) needed.insert(o.ticker);
  for (const auto& ticker : needed) if (!rows.count(ticker)) {
    std::string body;
    if (!fetch(kalshi::build_get_market(ticker), &body, why)) return false;
    auto market = kalshi::parse_market(body, why);
    if (!market) return false;
    rows[ticker] = std::move(*market);
  }
  // Systematic's held markets: one that cannot be read now costs only its own row this poll
  for (const auto& ticker : held) if (!rows.count(ticker)) {
    std::string body, miss;
    if (!fetch(kalshi::build_get_market(ticker), &body, &miss)) continue;
    if (auto market = kalshi::parse_market(body, &miss)) rows[ticker] = std::move(*market);
  }
  next.markets = std::move(rows);
  next.listing_ready = true;
  next.listing_ms = now_ms();
  last_listing_mono_ = mono_ms();
  return true;
}

void State::metadata(Snapshot& next,Worker* worker) {
  next.metadata_error.clear();
  auto wanted=snapshot()->live;
  for(const auto& p:next.positions) if(p.position) wanted.insert(p.ticker);
  for(const auto& o:next.orders) if(o.status==kalshi::OrderStatus::RESTING) wanted.insert(o.ticker);
  std::set<std::string> names;
  for(const auto& tk:wanted) {
    const auto it=next.markets.find(tk);
    if(it!=next.markets.end()) names.insert(series(it->second));
  }
  std::vector<std::string> due;
  for(const auto& name:names) {
    const auto it=series_.find(name);
    // Fetched again a minute before it goes stale: refetching only once stale left every order
    // on the market refused RO until the next poll (12 s), every five minutes (soak, 2026-10-10).
    if(it==series_.end() || !it->second.fetched_ms || now_ms()<it->second.fetched_ms ||
       now_ms()-it->second.fetched_ms>=kFeeRefreshMs) due.push_back(name);
  }
  std::sort(due.begin(),due.end(),[&](const auto& a,const auto& b) {
    const auto ia=series_.find(a),ib=series_.find(b);
    const auto ta=ia==series_.end() ? 0 : ia->second.attempted_ms;
    const auto tb=ib==series_.end() ? 0 : ib->second.attempted_ms;
    return ta!=tb ? ta<tb : a<b;
  });
  // The unfiltered demo /series response is already >18 MB. Bound work to the live watchlist,
  // use the single-series endpoint and cache it; never put discovery I/O on a click's path.
  std::size_t tried=0;
  for(const auto& name:due) {
    if(tried++==16 || (worker && worker->stopping())) break;
    auto& cached=series_[name]; cached.attempted_ms=now_ms();
    std::string body,why;
    if(!get(kalshi::build_get_series(name),&body,&why,worker)) {
      next.metadata_error="series metadata unavailable for "+name; continue;
    }
    auto row=kalshi::parse_series(body,&why);
    if(!row || row->ticker!=name) { next.metadata_error="invalid series metadata for "+name; continue; }
    cached.row=std::move(*row); cached.fetched_ms=now_ms();
  }
  for(auto& item:next.markets) {
    auto& m=item.second; const auto it=series_.find(series(m));
    m.fee_type.clear(); m.fee_multiplier.reset(); m.fee_ms=0;
    if(it==series_.end() || !it->second.fetched_ms) continue;
    m.fee_type=it->second.row.fee_type; m.fee_multiplier=it->second.row.fee_multiplier;
    m.fee_ms=it->second.fetched_ms;
    if(m.category.empty()) m.category=it->second.row.category;
  }
}
bool fee_ready(const kalshi::MarketRow& m,std::int64_t now) {
  return m.fee_ms>0 && now>=m.fee_ms && now-m.fee_ms<kFeeFreshMs && m.fee_multiplier &&
      std::isfinite(*m.fee_multiplier) && *m.fee_multiplier>=0 && *m.fee_multiplier<=1000 &&
      (m.fee_type=="quadratic" || m.fee_type=="quadratic_with_maker_fees");
}
bool State::refresh_once() { return refresh(nullptr); }
bool State::refresh(Worker* worker) {
  const auto fence = begin_ ? begin_() : 0;
  Snapshot next = *snapshot();
  next.error.clear();
  std::string why;
  const std::int64_t account_began = now_ms();
  // account() and listing() change next only once every page is in: a failure leaves the last read.
  auto young = [](std::int64_t at, int keep_ms) { return at > 0 && now_ms() >= at && now_ms() - at <= keep_ms; };
  if (!account(next, &why, worker)) {
    next.account_ready = next.account_ready && young(next.account_ms, options_.account_keep_ms);
    next.error = "account: " + why;
  } else next.account_began_ms = account_began;
  if (!next.listing_ready || mono_ms() - last_listing_mono_ >= options_.listing_poll_ms) {
    if (!listing(next, &why, worker)) {
      next.listing_ready = next.listing_ready && young(next.listing_ms, options_.listing_keep_ms);
      if (!next.error.empty()) next.error += "; ";
      next.error += "listing: " + why;
    }
  }
  metadata(next,worker);
  next.ts_ms = now_ms();
  if (next.account_ready && commit_ && !commit_(fence)) {
    // These pages may straddle a private event, so they never become the account. The last
    // consistent read stays (ready if it was): gw counts every order of its own that a read
    // cannot reflect at worst case, and a private feed that is down or unreconciled blocks
    // orders by itself. Marking the account not ready here instead refused clicks for a poll
    // after every fill or cancel during active trading.
    const auto prev = snapshot();
    next.exchange = prev->exchange;
    next.balance = prev->balance;
    next.balances = prev->balances;
    next.positions = prev->positions;
    next.orders = prev->orders;
    next.fills = prev->fills;
    next.account_ready = prev->account_ready;
    next.account_ms = prev->account_ms;
    next.account_began_ms = prev->account_began_ms;
    if (!next.error.empty()) next.error += "; ";
    next.error += "private feed changed during account read; reconciling again";
    if (fence) request_refresh(); // a disconnected socket requests a refresh after subscribing
  }
  next.clock = clock_.estimate(next.ts_ms);
  next.ready = next.account_ready && next.listing_ready;
  const bool ready = next.ready;
  publish(std::move(next));
  return ready;
}

void State::request_trades(const std::string& ticker, std::int64_t since_ms) {
  std::lock_guard<std::mutex> lock(controls_mu_);
  const auto failed = trades_failed_.find(ticker);
  if (failed != trades_failed_.end() && mono_ms() - failed->second < 60000) return;
  auto& at = trades_wanted_[ticker];
  if (!at || since_ms < at) at = since_ms;
}

void State::backfill_trades(Worker* worker) {
  // A backfill never delays the account: a refresh asked for (a private event to reconcile, and
  // until then Manual and Systematic orders are refused STALE) goes first, and one asked for
  // while a market's pages are being read stops them; the market is read again from the start.
  if (refresh_requested_.load()) return;
  std::string ticker;
  std::int64_t since = 0;
  {
    std::lock_guard<std::mutex> lock(controls_mu_);
    if (trades_wanted_.empty()) return;
    ticker = trades_wanted_.begin()->first;
    since = trades_wanted_.begin()->second;
    trades_wanted_.erase(trades_wanted_.begin());
  }
  std::vector<kalshi::TradeRow> rows;
  kalshi::TradesQuery q;
  q.ticker = ticker;
  q.min_ts = since / 1000;
  q.limit = 1000;
  std::string why;
  for (int page = 0; page < 5; ++page) {
    if (refresh_requested_.load()) {
      std::lock_guard<std::mutex> lock(controls_mu_);
      auto& at = trades_wanted_[ticker];
      if (!at || since < at) at = since;
      return;
    }
    std::string body;
    if (!get(kalshi::build_get_trades(q), &body, &why, worker)) break;
    auto p = kalshi::parse_trades(body, &why);
    if (!p) break;
    for (auto& r : p->rows) rows.push_back(std::move(r));
    if (p->cursor.empty()) {
      why.clear();
      if (trades_cb_) trades_cb_(ticker, since, std::move(rows));   // every print since `since`
      return;
    }
    q.cursor = p->cursor;
  }
  if (why.empty()) {   // five full pages: the tape is complete from the oldest print read
    const std::int64_t from = rows.empty() ? since : rows.back().ts_ms;
    if (trades_cb_) trades_cb_(ticker, from, std::move(rows));
    return;
  }
  DLOG_WARN("rest: trades for %s: %s", ticker.c_str(), why.c_str());
  std::lock_guard<std::mutex> lock(controls_mu_);
  trades_failed_[ticker] = mono_ms();
}

void State::run(Worker& worker) {
  auto next = mono_ms();
  std::int64_t last_attempt = 0;
  while (!worker.stopping()) {
    worker.beat();
    const auto now = mono_ms();
    if ((refresh_requested_.load() && now - last_attempt >= 1000) || now >= next) {
      refresh_requested_.store(false);
      last_attempt = now;
      refresh(&worker);
      next = mono_ms() + options_.poll_ms;
    } else {
      backfill_trades(&worker);   // between polls, one market at a time
    }
    if (!worker.stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  conn_.reset();
}

std::string category(const kalshi::MarketRow& market) {
  const auto c = upper(market.category);
  if (c == "SPORTS") return "SPORTS";
  if (c == "WEATHER" || c == "CLIMATE AND WEATHER") return "WEATHER";
  if (c == "FIN" || c == "FINANCIALS" || c == "CRYPTO" || c == "FINANCE") return "FIN";
  if (c == "ECON" || c == "ECONOMICS") return "ECON";
  const auto s = upper(series(market));
  if (any_prefix(s, {"KXHIGH", "KXLOW", "KXRAIN", "KXSNOW", "KXHURRICANE", "KXTEMP"})) return "WEATHER";
  if (any_prefix(s, {"KXNBA", "KXWNBA", "KXNFL", "KXMLB", "KXNHL", "KXNCAA", "KXCFB", "KXCBB",
                     "KXEPL", "KXUCL", "KXMLS", "KXSOCCER", "KXTENNIS", "KXATP", "KXWTA", "KXUFC"})) return "SPORTS";
  if (any_prefix(s, {"KXBTC", "KXETH", "KXSOL", "KXCRYPTO", "KXSP", "KXNASDAQ", "KXINX", "KXGOLD",
                     "KXSILVER", "KXCOPPER", "KXNATGAS", "KXOIL", "KXWTI", "KXBRENT", "KXEUR", "KXUSD", "KXPLAT"})) return "FIN";
  if (any_prefix(s, {"KXCPI", "KXGDP", "KXFED", "KXPCE", "KXPAYROLL", "KXUNEMP", "KXJOB"})) return "ECON";
  return "OTHER";
}

std::string outcome_date(const kalshi::MarketRow& market) {
  // Daily weather's ticker names the observed local day. Its close/settlement is commonly
  // after midnight ET; using that timestamp labels today's weather as tomorrow's.
  if (category(market) == "WEATHER") {
    const auto dash = market.ticker.find('-');
    if (dash != std::string::npos && market.ticker.size() >= dash + 8) {
      const std::string d = upper(market.ticker.substr(dash + 1, 7));
      const std::string months = "JANFEBMARAPRMAYJUNJULAUGSEPOCTNOVDEC";
      const auto at = months.find(d.substr(2, 3));
      if (std::isdigit(static_cast<unsigned char>(d[0])) && std::isdigit(static_cast<unsigned char>(d[1])) &&
          std::isdigit(static_cast<unsigned char>(d[5])) && std::isdigit(static_cast<unsigned char>(d[6])) &&
          at != std::string::npos && at % 3 == 0) {
        const int day = std::stoi(d.substr(5, 2));
        if (day >= 1 && day <= 31) {
          const int month = static_cast<int>(at / 3) + 1;
          return "20" + d.substr(0, 2) + "-" + (month < 10 ? "0" : "") + std::to_string(month) + "-" + d.substr(5, 2);
        }
      }
    }
  }
  const auto outcome = market.expected_expiration_ms.value_or(market.close_ms.value_or(market.expiration_ms.value_or(0)));
  return outcome ? et_date(outcome) : "";
}

json market_json(const kalshi::MarketRow& market, bool pin, bool live, std::int64_t now) {
  json grid = json::array();
  for (const auto& range : market.price_ranges)
    grid.push_back({{"start", kalshi::price_wire(range.start)}, {"end", kalshi::price_wire(range.end)},
                    {"step", kalshi::price_wire(range.step)}});
  const std::string status = is_open(market) ? "OPEN" : market.status == "settled" || market.status == "finalized" ? "SETTLED" : "CLOSED";
  json result = nullptr;
  if (market.result == "yes") result = "YES";
  if (market.result == "no") result = "NO";
  if (market.result == "void") result = "VOID";
  const auto close = effective_close(market);
  return {{"v", "K"}, {"tk", market.ticker}, {"ev", event(market)},
          {"title", market.title.empty() ? market.ticker : market.title}, {"short", label(market)},
          {"grp", group(market)}, {"kind", kind(market)}, {"og", event(market)}, {"oc", label(market)},
          // When open_time is absent, an active listing proves it opened by the snapshot;
          // the fallback is presentation only. The gateway still has the optional raw field.
          {"open_ms", market.open_ms.value_or(std::min(now, close ? close : now))},
          {"close_ms", close ? close : now}, {"status", status}, {"result", result}, {"grid", grid}, {"fee", fee(market)},
          {"shard", market.exchange_index ? json("S" + std::to_string(*market.exchange_index)) : json(nullptr)},
          {"lip", false}, {"start_ms", nullptr}, {"man", false}, {"cat", category(market)},
          {"date", outcome_date(market).empty() ? et_date(now) : outcome_date(market)},
          {"pin", pin}, {"gk", group(market)}, {"live", live}};
}

std::optional<Money> take_edge(const kalshi::MarketRow& m, std::optional<Px> bid, std::optional<Px> ask, Px fair) {
  const double mult = m.fee_multiplier.value_or(1.0);
  auto fee = [mult](Px p) {   // per contract, in 1/10,000 dollar, rounded up
    const double x = static_cast<double>(p) / kOneDollar;
    return static_cast<Money>(std::ceil(0.07 * mult * x * (1 - x) * kOneDollar - 1e-9));
  };
  std::optional<Money> best;
  if (ask) best = static_cast<Money>(fair - *ask) - fee(*ask);
  if (bid) { const Money e = static_cast<Money>(*bid - fair) - fee(*bid); if (!best || e > *best) best = e; }
  return best;
}

json universe(const Snapshot& state, const json& query, std::int64_t now, const FairOf& fair_of) {
  const auto errors = check_universe_query(query);
  if (!errors.empty()) return err_body("BAD_REQUEST", errors.front());
  const auto day = query.at("day").get<std::string>();
  const auto today = et_date(now), date = day == "T" ? today : et_date_after(today);
  const auto cat = query.value("cat", std::string()), venue = query.value("v", std::string());
  const auto text = upper(query.value("q", std::string())), sort = query.value("sort", std::string("close"));
  const std::size_t offset = query.value("offset", std::size_t{0}), limit = query.value("limit", std::size_t{100});
  json cats = json::object();
  for (const auto& c : kCats) cats[c] = 0;
  std::map<std::pair<std::string, std::string>, std::vector<json>> grouped;
  if (venue.empty() || venue == "K") for (const auto& pair : state.markets) {
    const auto& market = pair.second;
    if (!is_open(market) || (market.open_ms && *market.open_ms > now) || !market.close_ms || *market.close_ms <= now) continue;
    const auto outcome = outcome_date(market);
    if (outcome.empty() || (day == "T" ? outcome > today : outcome != date)) continue;
    auto row = universe_row(market, state, fair_of);
    if (!text.empty() && upper(market.ticker + " " + market.title + " " + label(market) + " " + group(market)).find(text) == std::string::npos) continue;
    const auto c = category(market);
    cats[c] = cats[c].get<int>() + 1;
    if (!cat.empty() && c != cat) continue;
    grouped[{c, group(market)}].push_back(std::move(row));
  }
  auto less = [&](const json& a, const json& b) {
    const auto& av = a.at(sort == "vol" ? "vol" : sort == "sprd" ? "sprd" : sort == "edge" ? "edge" : "close_ms");
    const auto& bv = b.at(sort == "vol" ? "vol" : sort == "sprd" ? "sprd" : sort == "edge" ? "edge" : "close_ms");
    if (av.is_null() != bv.is_null()) return !av.is_null();
    if (!av.is_null() && av != bv) {
      const double x = av.is_string() ? std::stod(av.get<std::string>()) : av.get<double>();
      const double y = bv.is_string() ? std::stod(bv.get<std::string>()) : bv.get<double>();
      if (x != y) return sort == "vol" || sort == "edge" ? x > y : x < y;
    }
    return a.at("tk").get<std::string>() < b.at("tk").get<std::string>();
  };
  for (auto& group_rows : grouped) std::sort(group_rows.second.begin(), group_rows.second.end(), less);
  std::vector<const std::vector<json>*> groups;
  for (const auto& c : kCats) {
    std::vector<const std::vector<json>*> in_category;
    for (const auto& entry : grouped) if (entry.first.first == c) in_category.push_back(&entry.second);
    std::sort(in_category.begin(), in_category.end(), [&](const auto* a, const auto* b) {
      // Group ties use gk, not the best row's ticker.
      json aa = a->front(), bb = b->front();
      aa["tk"] = ""; bb["tk"] = "";
      if (less(aa, bb)) return true;
      if (less(bb, aa)) return false;
      return a->front().at("gk").template get<std::string>() < b->front().at("gk").template get<std::string>();
    });
    groups.insert(groups.end(), in_category.begin(), in_category.end());
  }
  json group_rows = json::array(), rows = json::array();
  std::size_t total = 0;
  for (const auto* group_rows_ptr : groups) {
    group_rows.push_back({{"gk", group_rows_ptr->front().at("gk")}, {"cat", group_rows_ptr->front().at("cat")}, {"n", group_rows_ptr->size()}});
    for (const auto& row : *group_rows_ptr) { if (total >= offset && total - offset < limit) rows.push_back(row); ++total; }
  }
  // Stable FNV-1a over exactly the ordered shape: quote changes do not invalidate groups,
  // unless they actually reorder them under this query's sort.
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char c : group_rows.dump()) { hash ^= c; hash *= 1099511628211ULL; }
  std::ostringstream id;
  id << std::hex << hash;
  const auto grev = id.str();
  return {{"ok", true}, {"day", day}, {"date", date}, {"total", total}, {"offset", offset}, {"rows", rows},
          {"groups", query.value("grev", std::string()) == grev ? json(nullptr) : group_rows}, {"grev", grev},
          {"cats", cats}, {"ts_ms", now}};
}
}  // namespace deskd::rest
