#include "desk/reference_feed.h"

#include <algorithm>
#include <set>
#include <cctype>
#include <cmath>

#include "desk/auth.h"
#include "desk/log.h"

namespace desk {
namespace {

using nlohmann::json;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

double as_number(const json& j) {
  if (j.is_number()) return j.get<double>();
  if (j.is_string()) {
    try {
      return std::stod(j.get<std::string>());
    } catch (const std::exception&) {
      return 0;
    }
  }
  return 0;
}

// Kalshi's event socket has sent seconds on one channel and milliseconds on another. Read the
// magnitude rather than the field name: a 2026 timestamp in seconds is ~1.79e9, in milliseconds
// ~1.79e12.
long long to_ms(const json& j) {
  const double v = as_number(j);
  if (!(v > 0)) return 0;
  if (v > 1e17) return static_cast<long long>(v / 1e6);   // nanoseconds
  if (v > 1e14) return static_cast<long long>(v / 1e3);   // microseconds
  if (v > 1e11) return static_cast<long long>(v);         // milliseconds
  return static_cast<long long>(v * 1000);                // seconds
}

}  // namespace

ReferenceFeedSettings reference_feed_settings(const json& params, const std::string& env) {
  ReferenceFeedSettings s;
  s.ws_url = env == "prod" ? "wss://external-api-ws.kalshi.com/trade-api/ws/v2"
                           : "wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2";
  if (!params.is_object()) return s;
  const json& r = params.contains("reference") && params["reference"].is_object() ? params["reference"] : params;
  if (r.contains("enabled") && r["enabled"].is_boolean()) s.enabled = r["enabled"].get<bool>();
  if (r.contains("ws_url") && r["ws_url"].is_string() && !r["ws_url"].get<std::string>().empty())
    s.ws_url = r["ws_url"].get<std::string>();
  if (r.contains("sign_path") && r["sign_path"].is_string()) s.sign_path = r["sign_path"].get<std::string>();
  if (r.contains("subscribe") && r["subscribe"].is_array()) {
    s.subscribe.clear();
    for (const auto& v : r["subscribe"]) if (v.is_string()) s.subscribe.push_back(v.get<std::string>());
    if (s.subscribe.empty()) s.subscribe = {"all"};
  }
  auto num = [&r](const char* key, auto& out) {
    if (r.contains(key) && r[key].is_number()) out = static_cast<std::decay_t<decltype(out)>>(r[key].get<double>());
  };
  num("record_min_interval_ms", s.record_min_interval_ms);
  num("history_seconds", s.history_seconds);
  num("stale_after_seconds", s.stale_after_seconds);
  num("ws_stale_after_seconds", s.ws_stale_after_seconds);
  num("ping_interval_seconds", s.ping_interval_seconds);
  return s;
}

ReferenceFeed::ReferenceFeed(Store& store, ReferenceFeedSettings settings, std::shared_ptr<Signer> signer)
    : store_(store), s_(std::move(settings)), signer_(std::move(signer)) {
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_reference(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts_received INTEGER, ts_exchange INTEGER, symbol TEXT, source TEXT, price REAL,"
      "sequence INTEGER, raw_json TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_reference ON commodity_reference(symbol, ts_received)");
  // Session 17: how long the feed had been silent before this observation, and whether that
  // silence was longer than this symbol's own cadence. The contract settles on a carried-forward
  // value when the source publishes nothing, so a stale value is not an error - but it is not a
  // new observation either, and a volatility estimate that counts it as one is reading silence.
  for (const char* col : {"gap_ms INTEGER", "fresh INTEGER"})
    try { store_.exec(std::string("ALTER TABLE commodity_reference ADD COLUMN ") + col); }
    catch (const std::exception&) {}   // already migrated
  // Session 17, priority 3: the model's probability at fixed moments before each close, with the
  // state behind it. Created here because the feed's constructor is the one place guaranteed to
  // run before any commodity model prices anything.
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_calibration(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts INTEGER, ticker TEXT, event_ticker TEXT, series_ticker TEXT, model TEXT, model_version TEXT,"
      "checkpoint_seconds REAL, seconds_to_close REAL, tau_seconds REAL, p_model REAL, sigma_model REAL,"
      "band REAL, spot REAL, strike REAL, distance_sigma REAL, vol_sigma REAL, vol_warm INTEGER,"
      "vol_rv_5m REAL, vol_ewma REAL, vol_samples INTEGER, reference_ts INTEGER, reference_age REAL,"
      "reference_gap_ms INTEGER, reference_fresh INTEGER, replication_consecutive INTEGER,"
      "settlement_verified INTEGER, yes_bid REAL, yes_ask REAL, no_bid REAL, no_ask REAL,"
      "quote_ts INTEGER, strike_type TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_calibration ON commodity_calibration(ticker, checkpoint_seconds)");
  // The frames themselves, byte for byte: the first of each run and every one that surprised the
  // parser. A schema inferred from your own parser is not evidence about the wire.
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_reference_raw(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts_received INTEGER, sequence INTEGER, symbol TEXT, reason TEXT, frame TEXT)");
}

ReferenceFeed::~ReferenceFeed() { stop(); }

bool ReferenceFeed::running() const { return ws_ && ws_->running(); }

void ReferenceFeed::start() {
  if (ws_ || !s_.enabled || s_.ws_url.empty()) return;
  if (!signer_) {
    LOG_WARN("reference feed: no API key, so no %s feed (it is an authenticated channel)", s_.name.c_str());
    return;
  }
  WsClient::Options o;
  o.url = s_.ws_url;
  o.name = "reference/" + s_.name;
  o.ping_interval_seconds = s_.ping_interval_seconds;
  o.stale_after_seconds = s_.ws_stale_after_seconds;
  auto signer = signer_;
  const std::string path = s_.sign_path;
  o.auth_headers = [signer, path] { return signer->headers("GET", path, now_ms()); };
  const std::vector<std::string> subs = s_.subscribe;
  o.on_connect = [subs] {
    json params{{"channels", json::array({"pyth_value"})}, {"underlying_tickers", subs}};
    // And ask the channel what it actually carries. `underlying_list` returns every underlying
    // streamed in the last two hours, which is the venue's own answer to the question this desk
    // was previously answering by pattern-matching a guess.
    return std::vector<std::string>{
        json{{"id", 1}, {"cmd", "subscribe"}, {"params", params}}.dump(),
        json{{"id", 2}, {"cmd", "update_subscription"},
             {"params", json{{"sid", 1}, {"action", "underlying_list"}}}}.dump()};
  };
  ws_ = std::make_unique<WsClient>(o, [this](const std::string& text, long long ms) { on_text(text, ms); });
  ws_->start();
  LOG_INFO("reference feed: %s subscribed on %s", s_.name.c_str(), s_.ws_url.c_str());
}

void ReferenceFeed::stop() {
  if (ws_) ws_->stop();
  ws_.reset();
}

// Every field the stream has actually sent under `msg`, with its JSON type. Called under mu_.
void ReferenceFeed::note_schema(const json& msg, const std::string& text, long long received_ms) {
  static const std::set<std::string> kKnown{"underlying_ticker", "value_usd", "source_ts_ms", "received_at"};
  for (auto it = msg.begin(); it != msg.end(); ++it) {
    const std::string t = it.value().type_name();
    auto have = schema_.find(it.key());
    if (have == schema_.end()) {
      schema_[it.key()] = {t, 1};
      // A field nobody has seen before is worth one line and one kept frame, not a rejection: an
      // exchange adding an informational field must not take this desk's settlement feed down.
      if (!kKnown.count(it.key())) {
        ++schema_surprises_;
        LOG_WARN("reference feed: %s carries a field this desk does not know: %s (%s)",
                 s_.name.c_str(), it.key().c_str(), t.c_str());
        keep_raw(text, received_ms, msg.value("underlying_ticker", std::string()), 0, "unknown field");
      } else {
        LOG_INFO("reference feed: %s field %s observed as %s", s_.name.c_str(), it.key().c_str(), t.c_str());
      }
      continue;
    }
    ++have->second.second;
    if (have->second.first != t) {
      // A KNOWN field changing type is different in kind. value_usd going from string to number
      // silently changes what every price on this desk means, so it is kept and counted.
      ++schema_surprises_;
      LOG_WARN("reference feed: %s changed type on %s: %s -> %s", s_.name.c_str(), it.key().c_str(),
               have->second.first.c_str(), t.c_str());
      keep_raw(text, received_ms, msg.value("underlying_ticker", std::string()), 0, "type changed");
      have->second.first = t;
    }
  }
}

// Called under mu_.
void ReferenceFeed::keep_raw(const std::string& text, long long received_ms, const std::string& symbol,
                             long long seq, const char* why) {
  if (raw_kept_ >= s_.raw_frames_to_keep && std::string(why) == "first frames") return;
  if (pending_raw_.size() >= s_.max_pending_rows) return;
  ++raw_kept_;
  pending_raw_.push_back(json{{"ts_received", received_ms},
                              {"sequence", seq},
                              {"symbol", symbol},
                              {"reason", why},
                              {"frame", text.substr(0, 8192)}});
}

void ReferenceFeed::on_text(const std::string& text, long long received_ms) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    keep_raw(text, received_ms, "", 0, "unparseable");
    return;
  }
  const std::string type = j.value("type", std::string());
  // The channel's own answer to "what are the underlyings called": asked for on every connect,
  // so the desk records the venue's list rather than a pattern match against a guess.
  if (type == "pyth_value_underlying_list") {
    const json& m = j.contains("msg") && j["msg"].is_object() ? j["msg"] : j;
    std::lock_guard<std::mutex> lk(mu_);
    advertised_.clear();
    if (m.contains("underlying_tickers") && m["underlying_tickers"].is_array())
      for (const auto& u : m["underlying_tickers"]) if (u.is_string()) advertised_.push_back(u.get<std::string>());
    LOG_INFO("reference feed: the venue advertises %zu underlyings on this channel", advertised_.size());
    keep_raw(text, received_ms, "", 0, "underlying list");
    return;
  }
  if (type != "pyth_value") {
    // Subscription acknowledgements and errors are worth seeing once, not every time.
    if (type == "error") LOG_WARN("reference feed: %s", text.substr(0, 200).c_str());
    return;
  }
  const json& m = j.contains("msg") && j["msg"].is_object() ? j["msg"] : j;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (raw_kept_ < s_.raw_frames_to_keep) keep_raw(text, received_ms, m.value("underlying_ticker", std::string()),
                                                    j.value("seq", 0LL), "first frames");
    note_schema(m, text, received_ms);
  }
  const std::string symbol = m.value("underlying_ticker", std::string());
  const double price = as_number(m.contains("value_usd") ? m["value_usd"] : json());
  if (symbol.empty() || !(price > 0) || !std::isfinite(price)) {
    // Fail closed on the fields that decide what a price means. Everything else can be new.
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    keep_raw(text, received_ms, symbol, 0, "no usable price");
    return;
  }
  const long long source_ms = m.contains("source_ts_ms") ? to_ms(m["source_ts_ms"]) : 0;
  const long long venue_ms = m.contains("received_at") ? to_ms(m["received_at"]) : 0;
  const long long seq = j.contains("seq") && j["seq"].is_number() ? j["seq"].get<long long>() : 0;

  std::lock_guard<std::mutex> lk(mu_);
  ++messages_;
  if (seq > 0) {
    if (last_seq_ > 0 && seq > last_seq_ + 1) { ++seq_gaps_; last_gap_seq_ = seq; }
    last_seq_ = std::max(last_seq_, seq);
  }
  Series& s = series_[symbol];
  const bool first = s.messages == 0;
  ++s.messages;
  // Kalshi ignores duplicate and out-of-order source timestamps per underlying; so does this.
  if (source_ms > 0 && s.last.source_ts_ms > 0) {
    if (source_ms == s.last.source_ts_ms) { ++s.duplicates; return; }
    if (source_ms < s.last.source_ts_ms) { ++s.out_of_order; return; }
  }
  if (first)
    LOG_INFO("reference feed: first %s value for %s: %.6f", s_.name.c_str(), symbol.c_str(), price);
  // How long this symbol has been silent, against its own measured cadence. The feed publishes at
  // a fixed rate and Kalshi deduplicates it, so an unchanged price arrives as nothing at all: a
  // long gap means the value we were holding was being carried forward. The contract settles on
  // exactly that carried-forward value, so this is not an error - but it is not a new observation
  // either, and the volatility estimate must not read silence as a market that stopped moving.
  const long long previous_ms = s.last.received_ts_ms;
  const long long gap_ms = previous_ms > 0 ? received_ms - previous_ms : 0;
  bool fresh = true;
  if (previous_ms > 0) {
    s.gaps.push_back(gap_ms);
    while (s.gaps.size() > 200) s.gaps.pop_front();
    std::vector<long long> sorted(s.gaps.begin(), s.gaps.end());
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const long long cadence = sorted[sorted.size() / 2];
    const long long limit = std::max<long long>(s_.stale_gap_floor_ms,
                                                static_cast<long long>(s_.stale_gap_multiple * cadence));
    if (s.gaps.size() >= 20 && gap_ms > limit) {
      fresh = false;
      ++s.stale_periods;
    }
    s.longest_gap_ms = std::max(s.longest_gap_ms, gap_ms);
  }
  s.last = Value{price, source_ms, received_ms, seq, gap_ms, fresh};
  const long long ts = (source_ms > 0 ? source_ms : received_ms) / 1000;
  if (s.ticks.empty() || ts >= s.ticks.back().first) s.ticks.emplace_back(ts, price);
  const long long cutoff = ts - s_.history_seconds;
  while (!s.ticks.empty() && s.ticks.front().first < cutoff) s.ticks.pop_front();

  auto row = [&](double px, long long src_ms, long long recv_ms, long long sq) {
    return json{{"ts_received", recv_ms / 1000},
                {"ts_exchange", src_ms},
                {"symbol", symbol},
                {"source", "kalshi_ws:pyth_value"},
                {"price", px},
                {"sequence", sq},
                {"gap_ms", gap_ms},
                {"fresh", fresh ? 1 : 0},
                {"raw_json", json{{"underlying_ticker", symbol},
                                  {"value_usd", px},
                                  {"source_ts_ms", src_ms},
                                  {"received_at", venue_ms},
                                  {"seq", sq}}
                                 .dump()}};
  };
  auto queue = [&](json r) {
    if (pending_.size() >= s_.max_pending_rows) { ++dropped_rows_; return; }
    pending_.push_back(std::move(r));
  };

  // The candle that settles a contract is the LAST value in its minute, and which value that is
  // cannot be known until the next minute begins. So each minute's last observation is held back,
  // and written the moment a tick from a later minute proves it was the last. The sampling rule
  // below (one row per interval) then applies to everything else. Recording "the first tick after
  // :55", which is what a rule written on the arriving tick alone can manage, records a value the
  // contract does not settle on and misses the one it does.
  const long long minute = ts / 60;
  if (s.pending_minute > 0 && minute > s.pending_minute && s.pending_close) {
    queue(*s.pending_close);
    s.last_recorded_ms = s.pending_close_recv_ms;
    s.pending_close.reset();
  }
  if (minute != s.pending_minute) {
    s.pending_minute = minute;
    s.pending_close.reset();
  }

  const bool sample = s.last_recorded_ms == 0 || received_ms - s.last_recorded_ms >= s_.record_min_interval_ms;
  if (sample) {
    queue(row(price, source_ms, received_ms, seq));
    s.last_recorded_ms = received_ms;
    s.pending_close.reset();          // this minute's latest value is already on its way down
    s.pending_close_recv_ms = received_ms;
  } else {
    // Not sampled, but it may still turn out to be the minute's close: hold the newest one.
    s.pending_close = row(price, source_ms, received_ms, seq);
    s.pending_close_recv_ms = received_ms;
  }
}

std::optional<ReferenceFeed::Value> ReferenceFeed::latest(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = series_.find(symbol);
  if (it == series_.end() || !(it->second.last.price > 0)) return std::nullopt;
  return it->second.last;
}

std::vector<std::pair<long long, double>> ReferenceFeed::ticks(const std::string& symbol, long long from_ts) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::pair<long long, double>> out;
  auto it = series_.find(symbol);
  if (it == series_.end()) return out;
  out.reserve(it->second.ticks.size());
  for (const auto& t : it->second.ticks)
    if (t.first >= from_ts) out.push_back(t);
  return out;
}

std::optional<double> ReferenceFeed::candle_close(const std::string& symbol, long long minute_end) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = series_.find(symbol);
  if (it == series_.end()) return std::nullopt;
  std::optional<double> out;
  for (const auto& [ts, price] : it->second.ticks) {
    if (ts >= minute_end) break;
    if (ts >= minute_end - 60 && price > 0) out = price;
  }
  return out;
}

std::vector<std::string> ReferenceFeed::symbols() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> out;
  out.reserve(series_.size());
  for (const auto& [k, v] : series_) out.push_back(k);
  return out;
}

std::string ReferenceFeed::symbol_for(const std::vector<std::string>& patterns) const {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& pattern : patterns) {
    const std::string want = lower(pattern);
    if (want.empty()) continue;
    // An exact name wins over a pattern, so a configured symbol is never second-guessed.
    if (series_.count(pattern)) return pattern;
    for (const auto& [symbol, s] : series_)
      if (s.last.price > 0 && lower(symbol).find(want) != std::string::npos) return symbol;
  }
  return {};
}

void ReferenceFeed::note_replication(const std::string& symbol, double published_strike,
                                     double computed_close, double tolerance, long long now,
                                     long long window_open) {
  std::lock_guard<std::mutex> lk(mu_);
  Series& s = series_[symbol];
  Replication r;
  r.checked_ts = now;
  r.window_open = window_open;
  r.symbol = symbol;
  r.published_strike = published_strike;
  r.computed_close = computed_close;
  r.error = computed_close - published_strike;
  r.matched = std::fabs(r.error) <= tolerance;
  r.windows_checked = (s.replication ? s.replication->windows_checked : 0) + 1;
  r.windows_matched = (s.replication ? s.replication->windows_matched : 0) + (r.matched ? 1 : 0);
  // A single mismatch resets the run. The claim being made is "this feed reproduces the strike
  // every time", and one failure is enough to refute it.
  r.consecutive_matches = r.matched ? (s.replication ? s.replication->consecutive_matches : 0) + 1 : 0;
  s.replication = r;
}

json ReferenceFeed::observed_schema() const {
  std::lock_guard<std::mutex> lk(mu_);
  json out = json::object();
  for (const auto& [field, tc] : schema_) out[field] = json{{"type", tc.first}, {"count", tc.second}};
  return out;
}

std::vector<std::string> ReferenceFeed::advertised_underlyings() const {
  std::lock_guard<std::mutex> lk(mu_);
  return advertised_;
}

std::optional<ReferenceFeed::Replication> ReferenceFeed::replication(const std::string& symbol) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = series_.find(symbol);
  if (it == series_.end()) return std::nullopt;
  return it->second.replication;
}

int ReferenceFeed::flush(long long now) {
  std::vector<json> rows, raw;
  {
    std::lock_guard<std::mutex> lk(mu_);
    rows.swap(pending_);
    raw.swap(pending_raw_);
  }
  if (!raw.empty()) {
    try {
      for (const auto& r : raw)
        store_.exec("INSERT INTO commodity_reference_raw(ts_received,sequence,symbol,reason,frame)"
                    " VALUES(?,?,?,?,?)",
                    {r.value("ts_received", 0LL), r.value("sequence", 0LL), r.value("symbol", std::string()),
                     r.value("reason", std::string()), r.value("frame", std::string())});
    } catch (const std::exception& e) {
      LOG_WARN("reference feed: could not keep %zu raw frames: %s", raw.size(), e.what());
    }
  }
  if (rows.empty()) return 0;
  store_.exec("BEGIN");
  try {
    for (const auto& r : rows)
      store_.exec(
          "INSERT INTO commodity_reference(ts_received,ts_exchange,symbol,source,price,sequence,raw_json,gap_ms,fresh)"
          " VALUES(?,?,?,?,?,?,?,?,?)",
          {r.value("ts_received", 0LL), r.value("ts_exchange", 0LL), r.value("symbol", std::string()),
           r.value("source", std::string()), r.value("price", 0.0), r.value("sequence", 0LL),
           r.value("raw_json", std::string()), r.value("gap_ms", 0LL), r.value("fresh", 1LL)});
    store_.exec("COMMIT");
  } catch (const std::exception& e) {
    store_.exec("ROLLBACK");
    LOG_WARN("reference feed: could not record %zu observations: %s (they stay queued)", rows.size(), e.what());
    // The rows were taken off the queue before the write, so a rolled-back transaction would
    // otherwise throw them away silently: `recorded` simply stops rising and `status()` says
    // nothing is wrong. This table is the entire replay sample - put them back, oldest first, and
    // count the failure where somebody will see it.
    std::lock_guard<std::mutex> lk(mu_);
    ++write_failures_;
    const size_t room = s_.max_pending_rows > pending_.size() ? s_.max_pending_rows - pending_.size() : 0;
    if (rows.size() > room) {
      dropped_rows_ += static_cast<long long>(rows.size() - room);
      rows.resize(room);
    }
    rows.insert(rows.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
    pending_.swap(rows);
    return 0;
  }
  std::lock_guard<std::mutex> lk(mu_);
  recorded_ += static_cast<long long>(rows.size());
  (void)now;
  return static_cast<int>(rows.size());
}

json ReferenceFeed::status() const {
  json out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    const long long now = now_ms();
    json symbols = json::object();
    for (const auto& [symbol, s] : series_) {
      json one{{"price", s.last.price},
               {"source_ts_ms", s.last.source_ts_ms},
               {"received_ts_ms", s.last.received_ts_ms},
               {"age_seconds", s.last.received_ts_ms > 0 ? (now - s.last.received_ts_ms) / 1000.0 : -1.0},
               {"messages", s.messages},
               {"duplicates", s.duplicates},
               {"out_of_order", s.out_of_order},
               {"ticks_in_memory", static_cast<long long>(s.ticks.size())},
               {"gap_ms", s.last.gap_ms},
               {"fresh", s.last.fresh},
               {"stale_periods", s.stale_periods},
               {"longest_gap_ms", s.longest_gap_ms}};
      if (!s.gaps.empty()) {
        std::vector<long long> g(s.gaps.begin(), s.gaps.end());
        std::nth_element(g.begin(), g.begin() + g.size() / 2, g.end());
        one["cadence_ms"] = g[g.size() / 2];   // measured, not configured
      }
      if (s.replication)
        one["replication"] = json{{"checked_ts", s.replication->checked_ts},
                                  {"window_open", s.replication->window_open},
                                  {"consecutive_matches", s.replication->consecutive_matches},
                                  {"windows_checked", s.replication->windows_checked},
                                  {"windows_matched", s.replication->windows_matched},
                                  {"published_strike", s.replication->published_strike},
                                  {"computed_close", s.replication->computed_close},
                                  {"error", s.replication->error},
                                  {"matched", s.replication->matched}};
      symbols[symbol] = one;
    }
    out = json{{"enabled", s_.enabled},
               {"url", s_.ws_url},
               {"messages", messages_},
               {"unparsed", unparsed_},
               {"recorded", recorded_},
               {"dropped_rows", dropped_rows_},
               {"write_failures", write_failures_},
               {"raw_frames_kept", raw_kept_},
               {"schema_surprises", schema_surprises_},
               {"advertised_underlyings", advertised_},
               {"pending_rows", static_cast<long long>(pending_.size())},
               {"sequence_gaps", seq_gaps_},
               {"stale_after_seconds", s_.stale_after_seconds},
               {"symbols", symbols}};
  }
  out["socket"] = ws_ ? ws_->status() : json{{"running", false}};
  return out;
}


// ---- the exchange's own quotes, at millisecond resolution --------------------------------------

MarketQuoteFeed::Settings market_quote_settings(const json& params, const std::string& env) {
  MarketQuoteFeed::Settings s;
  s.ws_url = env == "prod" ? "wss://external-api-ws.kalshi.com/trade-api/ws/v2"
                           : "wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2";
  if (!params.is_object()) return s;
  const json& q = params.contains("quotes") && params["quotes"].is_object() ? params["quotes"] : json::object();
  if (q.contains("enabled") && q["enabled"].is_boolean()) s.enabled = q["enabled"].get<bool>();
  if (q.contains("ws_url") && q["ws_url"].is_string() && !q["ws_url"].get<std::string>().empty())
    s.ws_url = q["ws_url"].get<std::string>();
  if (q.contains("ws_stale_after_seconds") && q["ws_stale_after_seconds"].is_number())
    s.ws_stale_after_seconds = q["ws_stale_after_seconds"].get<int>();
  if (q.contains("ping_interval_seconds") && q["ping_interval_seconds"].is_number())
    s.ping_interval_seconds = q["ping_interval_seconds"].get<int>();
  return s;
}

MarketQuoteFeed::MarketQuoteFeed(Store& store, Settings settings, std::shared_ptr<Signer> signer)
    : store_(store), s_(std::move(settings)), signer_(std::move(signer)) {
  // One row per update the venue sends, with both clocks. `ts_exchange_ms` is the venue's own
  // millisecond stamp and `ts_received_ms` is ours; the difference between them is the only
  // honest way to say whether a lag we measure is theirs or the network's.
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_quotes(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts_received_ms INTEGER, ts_exchange_ms INTEGER, ticker TEXT, yes_bid REAL, yes_ask REAL,"
      "yes_bid_size REAL, yes_ask_size REAL, last REAL, volume REAL, open_interest REAL, raw_json TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_quotes ON commodity_quotes(ticker, ts_received_ms)");
}

MarketQuoteFeed::~MarketQuoteFeed() { stop(); }
bool MarketQuoteFeed::running() const { return ws_ && ws_->running(); }

void MarketQuoteFeed::start() {
  if (ws_ || !s_.enabled || s_.ws_url.empty()) return;
  if (!signer_) {
    LOG_WARN("%s: no API key, so no quote feed (the socket is authenticated)", s_.name.c_str());
    return;
  }
  WsClient::Options o;
  o.url = s_.ws_url;
  o.name = "quotes/" + s_.name;
  o.ping_interval_seconds = s_.ping_interval_seconds;
  o.stale_after_seconds = s_.ws_stale_after_seconds;
  auto signer = signer_;
  const std::string path = s_.sign_path;
  o.auth_headers = [signer, path] { return signer->headers("GET", path, now_ms()); };
  // Re-subscribed on every connect, from whatever the cycle last asked for: a reconnect must not
  // leave the desk watching the windows that were open twenty minutes ago.
  o.on_connect = [this] {
    std::vector<std::string> tickers;
    {
      std::lock_guard<std::mutex> lk(mu_);
      tickers.assign(watching_.begin(), watching_.end());
    }
    if (tickers.empty()) return std::vector<std::string>{};
    json params{{"channels", json::array({"ticker"})}, {"market_tickers", tickers}};
    return std::vector<std::string>{json{{"id", 1}, {"cmd", "subscribe"}, {"params", params}}.dump()};
  };
  ws_ = std::make_unique<WsClient>(o, [this](const std::string& text, long long ms) { on_text(text, ms); });
  ws_->start();
}

void MarketQuoteFeed::stop() {
  if (ws_) ws_->stop();
  ws_.reset();
}

std::vector<std::string> MarketQuoteFeed::watching() const {
  std::lock_guard<std::mutex> lk(mu_);
  return std::vector<std::string>(watching_.begin(), watching_.end());
}

void MarketQuoteFeed::send_delta(const std::vector<std::string>& add, const std::vector<std::string>& remove) {
  if (!ws_) return;
  auto cmd = [&](const char* action, const std::vector<std::string>& tickers) {
    if (tickers.empty()) return;
    json params{{"sid", 1}, {"action", action}, {"market_tickers", tickers}};
    if (std::string(action) == "add_markets") params["send_initial_snapshot"] = true;
    ws_->send(json{{"id", ++command_id_}, {"cmd", "update_subscription"}, {"params", params}}.dump());
  };
  cmd("add_markets", add);
  cmd("delete_markets", remove);
}

void MarketQuoteFeed::watch(const std::vector<std::string>& tickers) {
  std::vector<std::string> add, remove;
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::set<std::string> want(tickers.begin(), tickers.end());
    for (const auto& t : want) if (!watching_.count(t)) add.push_back(t);
    for (const auto& t : watching_) if (!want.count(t)) remove.push_back(t);
    if (add.empty() && remove.empty()) return;
    watching_ = std::move(want);
  }
  // A first set with nothing subscribed yet means the socket has not been told anything: the
  // connect handler will subscribe from watching_, so only a live socket needs the delta.
  if (ws_ && ws_->running()) send_delta(add, remove);
}

void MarketQuoteFeed::on_text(const std::string& text, long long received_ms) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    return;
  }
  if (j.value("type", std::string()) != "ticker") return;
  const json& m = j.contains("msg") && j["msg"].is_object() ? j["msg"] : j;
  const std::string ticker = m.value("market_ticker", std::string());
  if (ticker.empty()) {
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    return;
  }
  // Prices arrive as dollar strings; sizes as fixed point with two decimals. Both are read
  // tolerantly, because a number that fails to parse must cost one field rather than the row.
  auto num = [&m](const char* key) -> double {
    if (!m.contains(key)) return 0;
    const json& v = m[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) { try { return std::stod(v.get<std::string>()); } catch (...) { return 0; } }
    return 0;
  };
  const long long exchange_ms = m.contains("ts_ms") && m["ts_ms"].is_number()
                                    ? m["ts_ms"].get<long long>()
                                    : (m.contains("ts") && m["ts"].is_number() ? m["ts"].get<long long>() * 1000 : 0);
  std::lock_guard<std::mutex> lk(mu_);
  ++messages_;
  if (pending_.size() >= s_.max_pending_rows) { ++dropped_rows_; return; }
  pending_.push_back(json{{"ts_received_ms", received_ms},
                          {"ts_exchange_ms", exchange_ms},
                          {"ticker", ticker},
                          {"yes_bid", num("yes_bid_dollars")},
                          {"yes_ask", num("yes_ask_dollars")},
                          {"yes_bid_size", num("yes_bid_size_fp") / 100.0},
                          {"yes_ask_size", num("yes_ask_size_fp") / 100.0},
                          {"last", num("price_dollars")},
                          {"volume", num("volume_fp") / 100.0},
                          {"open_interest", num("open_interest_fp") / 100.0},
                          {"raw_json", text.substr(0, 4096)}});
}

int MarketQuoteFeed::flush(long long now) {
  (void)now;
  std::vector<json> rows;
  {
    std::lock_guard<std::mutex> lk(mu_);
    rows.swap(pending_);
  }
  if (rows.empty()) return 0;
  store_.exec("BEGIN");
  try {
    for (const auto& r : rows)
      store_.exec(
          "INSERT INTO commodity_quotes(ts_received_ms,ts_exchange_ms,ticker,yes_bid,yes_ask,"
          "yes_bid_size,yes_ask_size,last,volume,open_interest,raw_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
          {r.value("ts_received_ms", 0LL), r.value("ts_exchange_ms", 0LL), r.value("ticker", std::string()),
           r.value("yes_bid", 0.0), r.value("yes_ask", 0.0), r.value("yes_bid_size", 0.0),
           r.value("yes_ask_size", 0.0), r.value("last", 0.0), r.value("volume", 0.0),
           r.value("open_interest", 0.0), r.value("raw_json", std::string())});
    store_.exec("COMMIT");
  } catch (const std::exception& e) {
    store_.exec("ROLLBACK");
    LOG_WARN("%s: could not record %zu quotes: %s (they stay queued)", s_.name.c_str(), rows.size(), e.what());
    std::lock_guard<std::mutex> lk(mu_);
    rows.insert(rows.end(), std::make_move_iterator(pending_.begin()), std::make_move_iterator(pending_.end()));
    pending_.swap(rows);
    return 0;
  }
  std::lock_guard<std::mutex> lk(mu_);
  recorded_ += static_cast<long long>(rows.size());
  return static_cast<int>(rows.size());
}

json MarketQuoteFeed::status() const {
  json out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    out = json{{"enabled", s_.enabled},
               {"messages", messages_},
               {"recorded", recorded_},
               {"unparsed", unparsed_},
               {"dropped_rows", dropped_rows_},
               {"pending_rows", static_cast<long long>(pending_.size())},
               {"watching", std::vector<std::string>(watching_.begin(), watching_.end())}};
  }
  out["socket"] = ws_ ? ws_->status() : json{{"running", false}};
  return out;
}

}  // namespace desk
