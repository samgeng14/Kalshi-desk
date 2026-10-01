#include "desk/reference_feed.h"

#include <algorithm>
#include <set>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>

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

// A documented field read as a number, or nothing. Nothing is not zero: a column that says 0 when
// the field never arrived is how commodity_book_events came to record a price and a size of 0 on
// every delta from 2026-09-12 to 2026-09-24, because the parser asked for keys the venue does not
// send and nothing noticed.
std::optional<double> field_num(const json& m, const char* key) {
  auto it = m.find(key);
  if (it == m.end()) return std::nullopt;
  if (it->is_number()) return it->get<double>();
  if (!it->is_string()) return std::nullopt;
  const std::string& s = it->get_ref<const std::string&>();
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return std::nullopt;
  return v;
}

std::optional<long long> field_int(const json& m, const char* key) {
  auto it = m.find(key);
  if (it == m.end() || !it->is_number()) return std::nullopt;
  return it->get<long long>();
}

json or_null(const std::optional<double>& v) { return v ? json(*v) : json(nullptr); }
json or_null(const std::optional<long long>& v) { return v ? json(*v) : json(nullptr); }

// One queued row of commodity_book_events. JSON null becomes SQL NULL when it is written.
json book_row(long long received_ms, json exchange_ms, json ticker, const std::string& kind, json seq,
              json side, json price, json delta, std::string raw) {
  return json{{"ts_received_ms", received_ms}, {"ts_exchange_ms", std::move(exchange_ms)},
              {"ticker", std::move(ticker)},     {"kind", kind},
              {"sequence", std::move(seq)},      {"side", std::move(side)},
              {"price", std::move(price)},       {"delta", std::move(delta)},
              {"raw_json", std::move(raw)}};
}

// A queued value as the store binds it: a JSON null (or a missing key) stays NULL.
SqlValue sql_value(const json& r, const char* key) {
  auto it = r.find(key);
  if (it == r.end() || it->is_null()) return nullptr;
  if (it->is_number_integer()) return it->get<long long>();
  if (it->is_number()) return it->get<double>();
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
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
      "sequence INTEGER, raw_json TEXT, message_gap_ms INTEGER, unchanged_age_ms INTEGER,"
      "value_changed INTEGER, observed_value_fresh INTEGER)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_reference ON commodity_reference(symbol, ts_received)");
  // How long since the previous MESSAGE for this symbol, how long since the value last CHANGED,
  // whether this message changed it, and whether we can vouch for the recency of what we hold.
  // Four fields because they are four different facts, and the one thing the desk cannot see from
  // here is whether a silence was the source going quiet or the venue having nothing new to send.
  // Session 18: these say only what was observed. `gap_ms` and `fresh` came before, and asserted
  // something about the upstream source that this desk cannot see; they are left in place on an
  // existing database (empty from here on) rather than rewritten, because a column that silently
  // changes meaning is worse than one that stops being filled.
  for (const char* col : {"gap_ms INTEGER", "fresh INTEGER", "message_gap_ms INTEGER",
                          "unchanged_age_ms INTEGER", "value_changed INTEGER",
                          "observed_value_fresh INTEGER"})
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
      "reference_message_gap_ms INTEGER, reference_unchanged_age_ms INTEGER,"
      "reference_observed_fresh INTEGER, replication_consecutive INTEGER,"
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
    // Backwards is a new subscription, not a late message: the venue starts again at 1 on every
    // connection. Holding on to the old maximum, as this did until 2026-09-24, hid every gap after
    // a reconnect until the new count passed the old one, which takes about as long as the
    // previous connection lived.
    else if (last_seq_ > 0 && seq <= last_seq_) ++seq_restarts_;
    last_seq_ = seq;
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
  // Two clocks, two questions.
  //
  //   message_gap_ms    how long since we last heard ANYTHING about this symbol
  //   unchanged_age_ms  how long since the value we are holding last changed
  //
  // They are not the same, and the difference is the whole point. A long message gap means we
  // cannot vouch for the recency of the number we hold; it does NOT establish that the source
  // stopped publishing, because a deduplicated channel might simply have had nothing new to send.
  // The desk refuses to trade on a value whose recency it cannot verify, and says exactly that
  // rather than diagnosing the venue.
  const long long previous_ms = s.last.received_ts_ms;
  const long long message_gap_ms = previous_ms > 0 ? received_ms - previous_ms : 0;
  const bool value_changed = previous_ms <= 0 || price != s.last.price;
  if (previous_ms > 0) {
    if (value_changed) ++s.value_changes;
    // The measurement that would settle the ambiguity above. A channel that suppresses unchanged
    // values can never send one, so a single occurrence proves it does not suppress them.
    else ++s.unchanged_value_messages;
  }
  if (value_changed) s.value_change_ts_ms = received_ms;
  bool observed_value_fresh = true;
  if (previous_ms > 0) {
    s.gaps.push_back(message_gap_ms);
    while (s.gaps.size() > 200) s.gaps.pop_front();
    std::vector<long long> sorted(s.gaps.begin(), s.gaps.end());
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const long long cadence = sorted[sorted.size() / 2];
    const long long limit = std::max<long long>(s_.long_gap_floor_ms,
                                                static_cast<long long>(s_.long_gap_multiple * cadence));
    if (s.gaps.size() >= 20 && message_gap_ms > limit) {
      observed_value_fresh = false;
      ++s.long_message_gaps;
    }
    s.longest_gap_ms = std::max(s.longest_gap_ms, message_gap_ms);
  }
  const long long unchanged_age_ms =
      s.value_change_ts_ms > 0 ? received_ms - s.value_change_ts_ms : 0;
  s.last = Value{price,           source_ms,          received_ms,        seq, message_gap_ms,
                 unchanged_age_ms, s.value_change_ts_ms, value_changed,    observed_value_fresh};
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
                {"message_gap_ms", message_gap_ms},
                {"unchanged_age_ms", unchanged_age_ms},
                {"value_changed", value_changed ? 1 : 0},
                {"observed_value_fresh", observed_value_fresh ? 1 : 0},
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
  try {
    // Store::transaction holds the store lock for the whole batch: another thread's statement
    // cannot join this transaction (or be rolled back with it).
    store_.transaction([&] {
    for (const auto& r : rows)
      store_.exec(
          "INSERT INTO commodity_reference(ts_received,ts_exchange,symbol,source,price,sequence,raw_json,"
          "message_gap_ms,unchanged_age_ms,value_changed,observed_value_fresh)"
          " VALUES(?,?,?,?,?,?,?,?,?,?,?)",
          {r.value("ts_received", 0LL), r.value("ts_exchange", 0LL), r.value("symbol", std::string()),
           r.value("source", std::string()), r.value("price", 0.0), r.value("sequence", 0LL),
           r.value("raw_json", std::string()), r.value("message_gap_ms", 0LL),
           r.value("unchanged_age_ms", 0LL), r.value("value_changed", 1LL),
           r.value("observed_value_fresh", 1LL)});
    });
  } catch (const std::exception& e) {
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
               {"message_gap_ms", s.last.message_gap_ms},
               {"unchanged_age_ms", s.last.unchanged_age_ms},
               {"observed_value_fresh", s.last.observed_value_fresh},
               {"long_message_gaps", s.long_message_gaps},
               {"longest_gap_ms", s.longest_gap_ms},
               {"value_changes", s.value_changes},
               // Non-zero proves the channel emits unchanged values, and therefore that a silence
               // really is the source going quiet. Zero over a long recording leaves the two
               // explanations indistinguishable from here, which is why nothing claims otherwise.
               {"unchanged_value_messages", s.unchanged_value_messages}};
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
               {"sequence_restarts", seq_restarts_},
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
  if (q.contains("min_free_gb") && q["min_free_gb"].is_number())
    s.min_free_gb = std::max(0.0, q["min_free_gb"].get<double>());
  if (q.contains("keep_delta_raw") && q["keep_delta_raw"].is_boolean())
    s.keep_delta_raw = q["keep_delta_raw"].get<bool>();
  return s;
}

namespace {
// Whether a delta frame holds nothing the columns do not. Its columns keep the market, seq, side,
// price, size and venue millisecond; what else it carries is the sid (in the 'subscribed' and
// 'connect' rows), market_id (one per market, and in the snapshot and ticker rows), the type (the
// kind), and the microseconds of `ts`, which agree with ts_ms to the millisecond on 499,986 of
// 499,986 deltas checked. Any other key keeps the frame whole: the venue documents two optional ones,
// client_order_id and subaccount, present only when our own order moved the book, and they exist
// nowhere else.
bool delta_fully_held(const json& j, const json& m) {
  static const std::set<std::string> top = {"type", "sid", "seq", "msg"};
  static const std::set<std::string> inner = {"market_ticker", "market_id", "price_dollars", "delta_fp",
                                              "side", "ts", "ts_ms"};
  for (auto it = j.begin(); it != j.end(); ++it)
    if (!top.count(it.key())) return false;
  for (auto it = m.begin(); it != m.end(); ++it)
    if (!inner.count(it.key())) return false;
  return true;
}
}  // namespace

MarketQuoteFeed::MarketQuoteFeed(Store& store, Settings settings, std::shared_ptr<Signer> signer)
    : store_(store), s_(std::move(settings)), signer_(std::move(signer)) {
  // One row per update the venue sends, with both clocks. `ts_exchange_ms` is the venue's own
  // millisecond stamp and `ts_received_ms` is ours, from this machine's clock. Their difference is
  // latency PLUS the offset between the two clocks, and the offset is not small: on 2026-09-23/24
  // our receipt time was earlier than the venue's stamp on 99.4% of book deltas, by a median of
  // 34 ms one evening and 8 ms the next afternoon. So the difference cannot say whose lag is whose
  // below a few tens of milliseconds; for that, compare the venue's stamps with each other.
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_quotes(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts_received_ms INTEGER, ts_exchange_ms INTEGER, ticker TEXT, yes_bid REAL, yes_ask REAL,"
      "yes_bid_size REAL, yes_ask_size REAL, last REAL, volume REAL, open_interest REAL, raw_json TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_quotes ON commodity_quotes(ticker, ts_received_ms)");
  // The book, kept as the venue sent it: snapshots and deltas with their sequence numbers, not a
  // reconstruction. Reconstructing here would bake one interpretation into the record and leave
  // no way to check it; done offline, the reconstruction is testable and the raw stream survives.
  store_.exec(
      "CREATE TABLE IF NOT EXISTS commodity_book_events(id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "ts_received_ms INTEGER, ts_exchange_ms INTEGER, ticker TEXT, kind TEXT, sequence INTEGER,"
      "side TEXT, price REAL, delta REAL, raw_json TEXT)");
  store_.exec("CREATE INDEX IF NOT EXISTS idx_commodity_book ON commodity_book_events(ticker, sequence)");
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
  // A connect records the old set and clears it, and subscribes nothing itself (a ticker subscribe
  // naming only closed windows is the whole exchange): the next watch() subscribes to the engine's
  // open windows. See connect_frames.
  o.on_connect = [this] { return connect_frames(now_ms()); };
  ws_ = std::make_unique<WsClient>(
      o, [this](const std::string& text, long long ms) { on_text(text, ms); },
      [this](const std::string& event, const std::string&) {
        // 'stale', 'disconnected' and 'error' all come before the next connection is made.
        if (event == "connected") return;
        std::lock_guard<std::mutex> lk(mu_);
        conn_ready_ = false;
      });
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

namespace {
// One subscribe per channel, so each channel's own sid comes back on its own acknowledgement. The
// venue numbers a connection's subscriptions from 1 in the order they are asked for: ticker 1,
// orderbook_delta 2 on every connection recorded.
std::vector<std::string> subscribe_frames(const std::vector<std::string>& tickers) {
  // The ticker channel sends a market's current quote on subscribe only when asked; without it a
  // quiet window has no row until its quote next changes. The book always starts with a snapshot.
  return {json{{"id", 1}, {"cmd", "subscribe"},
               {"params", json{{"channels", json::array({"ticker"})}, {"market_tickers", tickers},
                               {"send_initial_snapshot", true}}}}.dump(),
          json{{"id", 2}, {"cmd", "subscribe"},
               {"params", json{{"channels", json::array({"orderbook_delta"})}, {"market_tickers", tickers}}}}.dump()};
}
}  // namespace

void MarketQuoteFeed::watch(const std::vector<std::string>& tickers) {
  // Deciding and queueing under one lock, and only on a connection whose connect handler has run,
  // so a new connection can never get a subscribe from here and another from its handler. The
  // lock order is this feed's, then the socket's: the socket calls back without holding its own.
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& f : watch_frames(tickers, ws_ != nullptr)) ws_->send(f);
}

void MarketQuoteFeed::force_reconnect(long long now_ms) {
  reconnect_due_ = false;
  last_forced_reconnect_ms_ = now_ms;
  ++forced_reconnects_;
  conn_ready_ = false;
  LOG_WARN("%s: a refusal left the subscription broken; reconnecting to subscribe afresh", s_.name.c_str());
  if (ws_) ws_->reconnect();
}

std::vector<std::string> MarketQuoteFeed::watch_frames(const std::vector<std::string>& tickers, bool has_socket) {
  // A refusal that came inside the minute after the last forced reconnect is acted on here.
  if (reconnect_due_ && now_ms() - last_forced_reconnect_ms_ >= 60000) force_reconnect(now_ms());
  // Never down to nothing. On the ticker channel a subscription with no markets left in it is a
  // subscription to every market on the exchange (the venue documents market_tickers as optional,
  // "omit to receive all markets"), and the cycle asks for nothing at every roll: it drops a window
  // the moment it closes and only learns of the next one from the market refresh later in the
  // cycle. On 2026-09-24 that made 46,583 of the last 60,000 commodity_quotes rows (77.6%) quotes
  // for 15,863 unrelated markets, in bursts of 4 to 11 seconds, at five rolls and after one reconnect
  // (14:50:07Z) that re-subscribed closed windows. A closed window sends nothing, so keeping it
  // until its successor is known costs nothing, and the successor is added before the old one is
  // deleted, so the set never passes through empty.
  if (tickers.empty()) return {};
  std::vector<std::string> add, remove, frames;
  std::set<std::string> want(tickers.begin(), tickers.end());
  for (const auto& t : want) if (!watching_.count(t)) add.push_back(t);
  for (const auto& t : watching_) if (!want.count(t)) remove.push_back(t);
  if (add.empty() && remove.empty()) return {};
  watching_ = std::move(want);
  // No connection to send on, or one whose connect handler has not run yet, or one about to be
  // dropped: the first watch() after the next connect subscribes.
  if (!has_socket || !conn_ready_ || reconnect_due_) return {};
  // The first set on a connection is a subscribe, not an update. A fresh process connects before
  // its first cycle has chosen any market, subscribes to nothing (nothing is right: an empty list
  // is the whole exchange), and then had only update_subscription to offer, which the venue
  // refused twice at the 2026-09-24 17:49Z start ("Unknown subscription ID"): the feed then
  // believed it watched five markets and received nothing.
  if (!subscribed_) {
    subscribed_ = true;
    return subscribe_frames(std::vector<std::string>(watching_.begin(), watching_.end()));
  }
  // Each channel's own sid, from its acknowledgement; until that arrives, the number the venue
  // gives it (see subscribe_frames). Commands are handled in order on one socket, so an update sent
  // after the subscribe finds the subscription even before the acknowledgement is back.
  const long long sids[2] = {sids_.count("ticker") ? sids_.at("ticker") : 1,
                             sids_.count("orderbook_delta") ? sids_.at("orderbook_delta") : 2};
  auto cmd = [&](const char* action, const std::vector<std::string>& list) {
    if (list.empty()) return;
    for (long long sid : sids) {
      json params{{"sid", sid}, {"action", action}, {"market_tickers", list}};
      const long long id = ++command_id_;
      // A snapshot on every addition, because a delta stream joined mid-flight is not a book. The
      // ids are kept: a refused add followed by an accepted delete would empty the ticker
      // subscription, which is the whole exchange.
      if (std::string(action) == "add_markets") {
        params["send_initial_snapshot"] = true;
        add_ids_.insert(id);
      }
      frames.push_back(json{{"id", id}, {"cmd", "update_subscription"}, {"params", params}}.dump());
    }
  };
  cmd("add_markets", add);
  cmd("delete_markets", remove);
  return frames;
}

std::vector<std::string> MarketQuoteFeed::connect_frames(long long now) {
  std::lock_guard<std::mutex> lk(mu_);
  const std::vector<std::string> before(watching_.begin(), watching_.end());
  // A new connection is a new subscription. The venue starts `seq` again at 1 and sends every
  // book again as a snapshot, but it reuses the same sid (2 on all 17 connections in the sample
  // of 2026-09-23/24), so nothing on the wire marks the boundary. Continuity starts over here,
  // and the row tells the replay that no book survives this point without its next snapshot.
  // The old connection's subscriptions went with it: their ids mean nothing on this one.
  last_seq_by_sid_.clear();
  sids_.clear();
  add_ids_.clear();
  reconnect_due_ = false;
  ++connects_;
  queue_book(book_row(now, nullptr, nullptr, "connect", nullptr, nullptr, nullptr, nullptr,
                      json{{"watching", before}}.dump()));
  // Nothing is subscribed from here. What was watched may be only windows that have closed: from a
  // roll until the refresh finds the next ones, and after any sleep longer than a window. The venue
  // takes a ticker subscribe naming only closed markets as a subscribe to every market: the
  // reconnect at 14:50:06Z on 2026-09-24 re-subscribed the closed 10:45 windows and recorded 12,727
  // quotes for unrelated markets in 11 s. So the set starts empty and the next watch() subscribes
  // to the engine's list, which holds only open windows. It costs at most one cycle of book.
  watching_.clear();
  subscribed_ = false;
  conn_ready_ = true;
  return {};
}

// Called under mu_. If the cycle stalls, book rows are dropped and counted rather than held without
// limit, and the hole is written down as soon as there is room again, so that a replay can tell
// rows this desk lost from messages the venue never sent. Both leave the same hole in `seq`.
void MarketQuoteFeed::queue_book(json row) {
  const json& sq = row["sequence"];
  const long long seq = sq.is_number() ? sq.get<long long>() : 0;
  if (pending_book_.size() >= s_.max_pending_rows) {
    ++book_dropped_rows_;
    if (drop_rows_++ == 0) {
      drop_first_ms_ = row.value("ts_received_ms", 0LL);
      drop_first_seq_ = seq;
    }
    if (seq > 0) drop_last_seq_ = seq;
    return;
  }
  if (drop_rows_ > 0) {
    pending_book_.push_back(book_row(row.value("ts_received_ms", 0LL), nullptr, nullptr, "dropped",
                                     nullptr, nullptr, nullptr, nullptr,
                                     json{{"rows", drop_rows_},
                                          {"first_seq", drop_first_seq_},
                                          {"last_seq", drop_last_seq_},
                                          {"first_received_ms", drop_first_ms_}}.dump()));
    drop_rows_ = drop_first_seq_ = drop_last_seq_ = drop_first_ms_ = 0;
  }
  pending_book_.push_back(std::move(row));
}

// Called under mu_. Continuity per subscription, because that is how the venue numbers `seq`: once
// per sid, across every market on it, and its replies to update_subscription take a number too.
// Counted per market, as it was until 2026-09-24, each switch from one market to the next looked
// like a gap: 30.3% and 31.3% of book messages in the two stretches checked, where the per-sid
// count finds 0 and 5 numbers missing, and all 5 are replies to our own commands at a roll.
void MarketQuoteFeed::note_sequence(long long sid, long long seq, bool book_frame) {
  auto it = last_seq_by_sid_.find(sid);
  if (it == last_seq_by_sid_.end()) {
    // Only a book frame starts the count. The ticker channel's own messages carry no seq, so a
    // reply on that sid would otherwise start a sequence that nothing continues.
    if (book_frame) last_seq_by_sid_[sid] = seq;
    return;
  }
  const long long expected = it->second + 1;
  if (seq > expected) {
    ++sid_gaps_;
    seqs_missing_ += seq - expected;
    if (sid_gaps_ <= 20 || sid_gaps_ % 100 == 0)
      LOG_WARN("%s: sid %lld went from seq %lld to %lld, so every book on it is unreliable until "
               "its next snapshot (%lld gaps so far)",
               s_.name.c_str(), sid, it->second, seq, sid_gaps_);
  } else if (seq < expected) {
    // Backwards is a new subscription on the same sid, not a late message: the venue does not
    // reorder within a subscription.
    ++seq_restarts_;
  }
  it->second = seq;
}

// Called under mu_.
void MarketQuoteFeed::note_missing(const std::string& kind, const char* key) {
  if (schema_misses_[kind + "." + key]++ == 0)
    LOG_WARN("%s: %s frame without a usable %s, so that column is not filled from it; the frame is "
             "kept whole in raw_json and counted in status (logged once per field)",
             s_.name.c_str(), kind.c_str(), key);
}

void MarketQuoteFeed::on_text(const std::string& text, long long received_ms) {
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    return;
  }
  const std::string type = j.value("type", std::string());
  const json& m = j.contains("msg") && j["msg"].is_object() ? j["msg"] : j;
  const std::optional<long long> sid = field_int(j, "sid"), seq = field_int(j, "seq");
  if (type == "orderbook_snapshot" || type == "orderbook_delta") {
    const std::string bt = m.value("market_ticker", std::string());
    if (bt.empty()) { std::lock_guard<std::mutex> lk(mu_); ++unparsed_; return; }
    const bool snapshot = type == "orderbook_snapshot";
    // Read from the keys the venue sends. A delta has price_dollars (a dollar string, not a whole
    // cent on 38% of deltas: "0.9980", "0.0210"), delta_fp (contracts with two decimals, negative
    // removes), side "yes" or "no", and ts_ms. A snapshot has its levels and none of those, so its columns stay
    // NULL rather than 0, which would read as a level at a price of zero. `price` and `delta` are
    // deliberately not read as a fallback: they are the margin socket's names, and on this venue's
    // older schema `price` was in cents, so a frame carrying them is counted as a miss rather than
    // recorded in what may be the wrong unit.
    std::optional<double> price, delta;
    const std::optional<long long> ex_ms = field_int(m, "ts_ms");
    const std::string side = m.value("side", std::string());
    size_t levels = 0;
    std::vector<const char*> missing;
    if (snapshot) {
      for (const char* k : {"yes_dollars_fp", "no_dollars_fp"})
        if (m.contains(k) && m[k].is_array()) levels += m[k].size();
    } else {
      price = field_num(m, "price_dollars");
      delta = field_num(m, "delta_fp");
      if (!price) missing.push_back("price_dollars");
      if (!delta) missing.push_back("delta_fp");
      if (side != "yes" && side != "no") missing.push_back("side");
      if (!ex_ms) missing.push_back("ts_ms");
    }
    if (!seq) missing.push_back("seq");
    // A delta whose columns hold all of it goes without its frame, which is most of the row. A
    // snapshot never does (its levels are only in the frame), nor does a delta with a field missing
    // or one the columns do not hold.
    const bool trim = !snapshot && !s_.keep_delta_raw && missing.empty() && delta_fully_held(j, m);
    std::lock_guard<std::mutex> lk(mu_);
    ++book_messages_;
    if (snapshot) {
      ++snapshots_;
      if (levels == 0) ++empty_snapshots_;
    }
    for (const char* k : missing) note_missing(type, k);
    if (sid && seq) note_sequence(*sid, *seq, true);
    if (disk_low_) {
      // Under the disk floor: followed, counted, not kept. set_disk_low writes the stretch down.
      ++paused_total_;
      if (pause_rows_++ == 0) {
        pause_first_ms_ = received_ms;
        pause_first_seq_ = seq.value_or(0);
      }
      pause_last_ms_ = received_ms;
      if (seq) pause_last_seq_ = *seq;
      return;
    }
    if (trim) ++raw_trimmed_;
    // Otherwise the frame is kept whole. A snapshot's levels exist nowhere else, and one cut short
    // is not JSON, which would silently cost every later delta for that market; the largest seen
    // was 3,558 bytes for 170 levels, and a 0.001 tick allows nearly 2,000.
    json row = book_row(received_ms, or_null(ex_ms), bt, type, or_null(seq),
                        side == "yes" || side == "no" ? json(side) : json(nullptr), or_null(price),
                        or_null(delta), trim ? std::string() : text);
    if (trim) row["raw_json"] = nullptr;
    queue_book(std::move(row));
    return;
  }
  if (type != "ticker") {
    if (type.empty()) { std::lock_guard<std::mutex> lk(mu_); ++unparsed_; return; }
    // Everything else on this socket is the venue answering a command: subscribed, ok, error,
    // unsubscribed. Kept verbatim with its seq, because a reply to update_subscription takes a
    // number in the book's sequence (the 5 numbers missing from the 400,000 book messages recorded
    // on 2026-09-24, one for each delete and add at the 15:30 roll), and without these rows a
    // replay cannot tell a change of subscription from a lost delta.
    std::lock_guard<std::mutex> lk(mu_);
    ++control_frames_;
    if (type == "subscribed" && m.contains("sid") && m["sid"].is_number()) {
      // The venue's own subscription ids, taken from its acknowledgements. `update_subscription`
      // needs them, and guessing works right up until the day they are numbered differently. A
      // new subscription numbers from 1 again.
      const long long new_sid = m["sid"].get<long long>();
      const std::string channel = m.value("channel", std::string());
      if (!channel.empty()) sids_[channel] = new_sid;
      last_seq_by_sid_.erase(new_sid);
    }
    if (type == "error") {
      // A refused add_markets leaves watching_ naming a market the socket does not carry, and
      // nothing else would ever say so.
      ++errors_;
      LOG_WARN("%s: the venue refused a command: %s", s_.name.c_str(), text.substr(0, 300).c_str());
      // Some refusals leave a subscription that cannot be repaired on this connection: a subscribe
      // of ours refused (ids 1 and 2), an add refused (the delete after it would still empty the
      // ticker subscription, which is the whole exchange), an unknown subscription id (7), and the
      // two the venue calls terminal, a channel error (10) and a subscription buffer overflow (25).
      // The keep-alive holds the socket up regardless, so without this the feed would believe it
      // watched markets and receive nothing until something else broke the connection, as it did
      // at 17:49Z on 2026-09-24. Drop the connection instead; the next one subscribes afresh. At
      // most once a minute, so a venue that keeps refusing is not hammered; one that comes inside
      // the minute is kept, and acted on by the first watch() after it.
      const long long code =
          j.contains("msg") && j["msg"].is_object() ? field_int(j["msg"], "code").value_or(0) : 0;
      const long long id = field_int(j, "id").value_or(0);
      if (id == 1 || id == 2 || add_ids_.count(id) || code == 7 || code == 10 || code == 25) {
        if (received_ms - last_forced_reconnect_ms_ >= 60000) force_reconnect(received_ms);
        else reconnect_due_ = true;
      }
    }
    if (sid && seq) note_sequence(*sid, *seq, false);
    queue_book(book_row(received_ms, nullptr, nullptr, type, or_null(seq), nullptr, nullptr, nullptr, text));
    return;
  }
  const std::string ticker = m.value("market_ticker", std::string());
  if (ticker.empty()) {
    std::lock_guard<std::mutex> lk(mu_);
    ++unparsed_;
    return;
  }
  // Prices arrive as dollar strings ("0.8400"). Sizes, volume and open interest arrive as *_fp
  // strings, a count of contracts with two decimals: "360.00" is 360 contracts, as fixed.h and the
  // REST parser (types.cpp) read the same fields. Until 2026-09-24 these were divided by 100, so
  // every non-zero size, volume and open interest in commodity_quotes before then is 100 times too
  // small; the book rebuilt from the raw deltas has the unscaled size at the best bid on 854 of
  // 854 ticker frames checked. A field that is missing or does not parse is NULL and counted.
  static const char* const kFields[] = {"yes_bid_dollars", "yes_ask_dollars", "yes_bid_size_fp",
                                        "yes_ask_size_fp", "price_dollars",   "volume_fp",
                                        "open_interest_fp"};
  std::optional<double> v[7];
  for (int i = 0; i < 7; ++i) v[i] = field_num(m, kFields[i]);
  std::optional<long long> exchange_ms = field_int(m, "ts_ms");
  if (!exchange_ms) {
    const std::optional<long long> secs = field_int(m, "ts");
    if (secs) exchange_ms = *secs * 1000;
  }
  std::lock_guard<std::mutex> lk(mu_);
  ++messages_;
  // Recorded either way (a market just removed can still send a last update), but counted, so
  // the status shows at once if the subscription ever widens to the whole exchange again.
  if (!watching_.count(ticker)) ++unwatched_;
  for (int i = 0; i < 7; ++i)
    if (!v[i]) note_missing(type, kFields[i]);
  if (!exchange_ms) note_missing(type, "ts_ms");
  if (pending_.size() >= s_.max_pending_rows) { ++dropped_rows_; return; }
  pending_.push_back(json{{"ts_received_ms", received_ms},
                          {"ts_exchange_ms", or_null(exchange_ms)},
                          {"ticker", ticker},
                          {"yes_bid", or_null(v[0])},
                          {"yes_ask", or_null(v[1])},
                          {"yes_bid_size", or_null(v[2])},
                          {"yes_ask_size", or_null(v[3])},
                          {"last", or_null(v[4])},
                          {"volume", or_null(v[5])},
                          {"open_interest", or_null(v[6])},
                          {"raw_json", text}});
}

int MarketQuoteFeed::flush(long long now) {
  (void)now;
  std::vector<json> rows, book;
  {
    std::lock_guard<std::mutex> lk(mu_);
    rows.swap(pending_);
    book.swap(pending_book_);
  }
  if (!book.empty()) {
    try {
      store_.transaction([&] {
      for (const auto& r : book)
        store_.exec("INSERT INTO commodity_book_events(ts_received_ms,ts_exchange_ms,ticker,kind,"
                    "sequence,side,price,delta,raw_json) VALUES(?,?,?,?,?,?,?,?,?)",
                    {sql_value(r, "ts_received_ms"), sql_value(r, "ts_exchange_ms"), sql_value(r, "ticker"),
                     sql_value(r, "kind"), sql_value(r, "sequence"), sql_value(r, "side"),
                     sql_value(r, "price"), sql_value(r, "delta"), sql_value(r, "raw_json")});
      });
      std::lock_guard<std::mutex> lk(mu_);
      book_recorded_ += static_cast<long long>(book.size());
    } catch (const std::exception& e) {
      LOG_WARN("%s: could not record %zu book events: %s (they stay queued)", s_.name.c_str(),
               book.size(), e.what());
      std::lock_guard<std::mutex> lk(mu_);
      book.insert(book.end(), std::make_move_iterator(pending_book_.begin()),
                  std::make_move_iterator(pending_book_.end()));
      pending_book_.swap(book);
    }
  }
  if (rows.empty()) return 0;
  try {
    store_.transaction([&] {
    for (const auto& r : rows)
      store_.exec(
          "INSERT INTO commodity_quotes(ts_received_ms,ts_exchange_ms,ticker,yes_bid,yes_ask,"
          "yes_bid_size,yes_ask_size,last,volume,open_interest,raw_json) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
          {sql_value(r, "ts_received_ms"), sql_value(r, "ts_exchange_ms"), sql_value(r, "ticker"),
           sql_value(r, "yes_bid"), sql_value(r, "yes_ask"), sql_value(r, "yes_bid_size"),
           sql_value(r, "yes_ask_size"), sql_value(r, "last"), sql_value(r, "volume"),
           sql_value(r, "open_interest"), sql_value(r, "raw_json")});
    });
  } catch (const std::exception& e) {
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

void MarketQuoteFeed::set_disk_low(bool low, long long now_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  if (low == disk_low_) return;
  disk_low_ = low;
  if (low || pause_rows_ == 0) return;
  queue_book(book_row(now_ms, nullptr, nullptr, "paused_low_disk", nullptr, nullptr, nullptr, nullptr,
                      json{{"rows", pause_rows_},
                           {"first_seq", pause_first_seq_},
                           {"last_seq", pause_last_seq_},
                           {"first_received_ms", pause_first_ms_},
                           {"last_received_ms", pause_last_ms_}}.dump()));
  pause_rows_ = pause_first_ms_ = pause_last_ms_ = pause_first_seq_ = pause_last_seq_ = 0;
}

json MarketQuoteFeed::status() const {
  json out;
  {
    std::lock_guard<std::mutex> lk(mu_);
    out = json{{"enabled", s_.enabled},
               {"messages", messages_},
               {"recorded", recorded_},
               {"unparsed", unparsed_},
               {"dropped_rows", dropped_rows_},      // ticker rows; book rows are counted apart
               {"pending_rows", static_cast<long long>(pending_.size())},
               {"unwatched_ticker_messages", unwatched_},
               {"schema_misses", schema_misses_},
               {"connects", connects_},
               {"control_frames", control_frames_},
               {"errors", errors_},
               // Kept apart from the ticker counts on purpose: one answers "did the quoted price
               // move", the other "could this quantity have been executed", and a study that
               // confuses them is describing a price nobody could have taken.
               {"book_messages", book_messages_},
               {"book_recorded", book_recorded_},
               {"book_snapshots", snapshots_},
               {"book_empty_snapshots", empty_snapshots_},
               // Per subscription id. Named apart from the per-market count it replaces
               // (book_sequence_gaps, about 30% of messages, nearly all of them market switches)
               // so that the two are never compared.
               {"book_sid_gaps", sid_gaps_},
               {"book_seqs_missing", seqs_missing_},
               {"book_seq_restarts", seq_restarts_},
               {"book_dropped_rows", book_dropped_rows_},
               {"book_raw_trimmed", raw_trimmed_},
               {"forced_reconnects", forced_reconnects_},
               {"book_paused_low_disk", disk_low_},
               {"book_not_kept_low_disk", paused_total_},
               {"min_free_gb", s_.min_free_gb},
               {"pending_book_rows", static_cast<long long>(pending_book_.size())},
               {"subscription_ids", sids_},
               {"watching", std::vector<std::string>(watching_.begin(), watching_.end())}};
  }
  out["socket"] = ws_ ? ws_->status() : json{{"running", false}};
  return out;
}

}  // namespace desk
