#include "deskd/ref/pyth.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/net/ws.h"

namespace deskd {
using json = nlohmann::json;
namespace {
std::optional<double> number(const json& v) {
  if (v.is_number()) return v.get<double>();
  if (!v.is_string()) return std::nullopt;
  const auto& s = v.get_ref<const std::string&>();
  if (s.empty() || s.size() > 40) return std::nullopt;
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return std::nullopt;
  return d;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
}  // namespace

std::int64_t RefSeries::median_gap_ms() const {
  if (gaps.empty()) return 0;
  std::vector<std::int64_t> v(gaps.begin(), gaps.end());
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

bool PythValues::value(const json& msg, std::int64_t recv_ms, std::string* why) {
  auto bad = [&](const char* w) { ++malformed_; if (why) *why = w; return false; };
  if (!msg.is_object()) return bad("pyth_value msg is not an object");
  if (!msg.contains("underlying_ticker") || !msg["underlying_ticker"].is_string()) return bad("no underlying_ticker");
  const auto symbol = msg["underlying_ticker"].get<std::string>();
  if (symbol.empty() || symbol.size() > 80) return bad("bad underlying_ticker");
  const auto v = msg.contains("value_usd") ? number(msg["value_usd"]) : std::nullopt;
  if (!v || !std::isfinite(*v) || *v <= 0) return bad("no usable value_usd");
  std::int64_t source = 0;
  if (msg.contains("source_ts_ms")) {
    const auto t = number(msg["source_ts_ms"]);
    if (!t || *t < 1e12 || *t > 1e14) return bad("bad source_ts_ms");
    source = static_cast<std::int64_t>(*t);
  }
  auto& s = series_[symbol];
  s.symbol = symbol;
  if (const auto* last = s.last()) {
    if (source && last->source_ms) {
      if (source == last->source_ms) { ++s.duplicates; return true; }
      if (source < last->source_ms) { ++s.out_of_order; return true; }
    }
    if (recv_ms >= last->recv_ms) {
      s.gaps.push_back(recv_ms - last->recv_ms);
      if (s.gaps.size() > 64) s.gaps.pop_front();
    }
    if (*v == last->value) ++s.unchanged;   // proves the channel sends unchanged values
  }
  ++s.messages;
  s.ticks.push_back(RefTick{source, recv_ms, *v});
  while (!s.ticks.empty() && recv_ms - s.ticks.front().recv_ms > o_.history_ms) s.ticks.pop_front();
  return true;
}

std::int64_t PythValues::gap_limit_ms(const RefSeries& s) const {
  return std::max<std::int64_t>(o_.gap_floor_ms, static_cast<std::int64_t>(o_.gap_multiple * s.median_gap_ms()));
}

bool PythValues::fresh(const RefSeries& s, std::int64_t now) const {
  const auto* last = s.last();
  if (!last) return false;
  const auto age = now - last->recv_ms;
  return age >= 0 && age <= gap_limit_ms(s) && age <= o_.stale_ms;
}

KalshiPyth::KalshiPyth(net::Endpoint ep, net::ConnFactory& factory, const KalshiSigner& signer, FeedState& feed)
    : KalshiPyth(std::move(ep), factory, signer, feed, Options{}) {}
KalshiPyth::KalshiPyth(net::Endpoint ep, net::ConnFactory& factory, const KalshiSigner& signer, FeedState& feed,
                       Options options)
    : endpoint_(std::move(ep)), factory_(factory), signer_(signer), feed_(feed), options_(options),
      values_(options.values) {}

std::string pyth_symbol_for(const PythValues& values, const std::vector<std::string>& patterns) {
  for (const auto& p : patterns)
    if (values.series().count(p)) return p;
  for (const auto& p : patterns) {
    const auto want = lower(p);
    for (const auto& [symbol, s] : values.series())
      if (lower(symbol).find(want) != std::string::npos) return symbol;
  }
  return "";
}

std::string KalshiPyth::symbol_for(const std::vector<std::string>& patterns) const {
  std::lock_guard<std::mutex> lk(m_);
  return pyth_symbol_for(values_, patterns);
}

std::optional<RefView> KalshiPyth::view(const std::string& symbol, std::int64_t since_ms, std::int64_t now) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = values_.series().find(symbol);
  if (it == values_.series().end() || !it->second.last()) return std::nullopt;
  const auto& s = it->second;
  RefView v;
  v.symbol = symbol;
  v.value = s.last()->value;
  v.source_ms = s.last()->source_ms;
  v.recv_ms = s.last()->recv_ms;
  v.fresh = values_.fresh(s, now);
  v.gap_limit_ms = values_.gap_limit_ms(s);
  if (since_ms >= 0)
    for (const auto& t : s.ticks) {
      const auto at = t.source_ms ? t.source_ms : t.recv_ms;
      if (at >= since_ms) v.ticks.emplace_back(at, t.value);
    }
  return v;
}

json KalshiPyth::status(std::int64_t now) const {
  std::lock_guard<std::mutex> lk(m_);
  json rows = json::object();
  for (const auto& [symbol, s] : values_.series()) {
    const auto* last = s.last();
    rows[symbol] = {{"value", last ? json(last->value) : json(nullptr)},
                    {"source_ms", last ? json(last->source_ms) : json(nullptr)},
                    {"age_ms", last ? json(now - last->recv_ms) : json(nullptr)},
                    {"median_gap_ms", s.median_gap_ms()}, {"fresh", values_.fresh(s, now)},
                    {"messages", s.messages}, {"duplicates", s.duplicates},
                    {"out_of_order", s.out_of_order}, {"unchanged", s.unchanged}};
  }
  return {{"up", feed_.view().up}, {"connects", connects_}, {"malformed", values_.malformed()},
          {"error", last_error_}, {"underlyings", rows}};
}

void KalshiPyth::run(Worker& worker) {
  int backoff = std::max(20, options_.backoff_min_ms);
  std::string logged;
  std::int64_t logged_ms = 0;
  while (!worker.stopping()) {
    feed_.set(false, true);
    worker.beat();
    net::WsClient ws;
    std::string why;
    const auto auth = signer_.headers(now_ms(), "GET", endpoint_.base_path);
    bool connected = !auth.empty() &&
                     ws.connect(factory_, endpoint_, auth, options_.connect_ms, &why, [&] { worker.beat(); });
    if (connected)
      connected = ws.send(net::WsOp::TEXT,
                          json{{"id", 1}, {"cmd", "subscribe"},
                               {"params", {{"channels", {"pyth_value"}}, {"underlying_tickers", {"all"}}}}}.dump(),
                          500, &why, [&] { worker.beat(); });
    if (connected) {
      std::lock_guard<std::mutex> lk(m_);
      ++connects_;
    }
    std::int64_t sid = 0;
    const auto started = mono_ms();
    auto last_ping = started;
    std::int64_t ping_sent = 0;
    std::string ping;
    int lag = -1;
    while (connected && !worker.stopping()) {
      worker.beat();
      const auto now = mono_ms();
      if (!sid && now - started > options_.ack_ms) { why = "no subscribe ack"; break; }
      if (ping_sent && now - ping_sent > options_.pong_ms) { why = "no pong"; break; }
      if (!ping_sent && now - last_ping >= options_.ping_ms) {
        ping = "pyth-" + std::to_string(now);
        ping_sent = now;
        last_ping = now;
        if (!ws.send(net::WsOp::PING, ping, 250, &why)) break;
      }
      feed_.set(sid != 0, false, lag);
      net::WsEvent frame;
      const auto status = ws.read(&frame, 100, &why);
      if (status == net::WsClient::Read::TIMEOUT) continue;
      if (status != net::WsClient::Read::EVENT) break;
      if (frame.op == net::WsOp::PING) continue;
      if (frame.op == net::WsOp::PONG) { if (frame.payload == ping) ping_sent = 0; continue; }
      if (frame.op != net::WsOp::TEXT) { why = "unexpected frame"; break; }
      const auto j = json::parse(frame.payload, nullptr, false);
      if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) { why = "unreadable frame"; break; }
      const auto type = j["type"].get<std::string>();
      if (type == "subscribed") {
        if (j.contains("msg") && j["msg"].is_object() && j["msg"].value("channel", "") == "pyth_value" &&
            j["msg"].contains("sid") && j["msg"]["sid"].is_number_integer())
          sid = j["msg"]["sid"].get<std::int64_t>();
        backoff = std::max(20, options_.backoff_min_ms);
        continue;
      }
      if (type == "error") {
        why = "venue error: " + (j.contains("msg") ? j["msg"].dump().substr(0, 160) : std::string("?"));
        break;
      }
      if (type != "pyth_value") continue;   // other actions' replies are not values
      std::string bad;
      const auto recv = now_ms();
      {
        std::lock_guard<std::mutex> lk(m_);
        if (!values_.value(j.contains("msg") ? j["msg"] : json(), recv, &bad)) last_error_ = bad;
      }
      if (j.contains("msg") && j["msg"].is_object() && j["msg"].contains("source_ts_ms") &&
          j["msg"]["source_ts_ms"].is_number_integer()) {
        const auto d = recv - j["msg"]["source_ts_ms"].get<std::int64_t>();
        if (d >= 0 && d < 600000) lag = static_cast<int>(d);
      }
    }
    feed_.set(false, true);
    ws.close();
    if (!worker.stopping()) {
      {
        std::lock_guard<std::mutex> lk(m_);
        last_error_ = why;
      }
      // once a minute per reason, so a venue that refuses the channel cannot flood the log
      if (why != logged || mono_ms() - logged_ms > 60000) {
        DLOG_WARN("pyth_value feed down: %s", why.empty() ? "connection closed" : why.c_str());
        logged = why;
        logged_ms = mono_ms();
      }
    }
    const auto end = mono_ms() + backoff;
    while (!worker.stopping() && mono_ms() < end) {
      worker.beat();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    backoff = std::min(std::max(20, options_.backoff_max_ms), backoff * 2);
  }
  feed_.set(false, true);
}

}  // namespace deskd
