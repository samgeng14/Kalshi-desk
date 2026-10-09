// Shapes verified against docs.kalshi.com/websockets/orderbook-updates and
// websockets/websocket-connection, 2026-10-08. Replay tests also cover the recorded reply seq.
#include "deskd/md/kalshi_md.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <deque>
#include <limits>
#include <thread>

#include <nlohmann/json.hpp>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/ws.h"

namespace deskd {
using json = nlohmann::json;
namespace {
std::optional<std::int64_t> integer(const json& j, const char* field) {
  const auto it = j.find(field);
  if (it == j.end() || !it->is_number_integer()) return {};
  if (it->is_number_unsigned() && it->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) return {};
  return it->get<std::int64_t>();
}
bool valid_ticker(const std::string& ticker) {
  return !ticker.empty() && ticker.size() <= 96 && std::all_of(ticker.begin(), ticker.end(), [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
  });
}
bool levels(const json& msg, const char* key, bool no, std::vector<MdLevel>* out) {
  const auto it = msg.find(key);
  // Kalshi omits an empty side. No prices are fabricated: the vector remains empty and the
  // consumer publishes a null best bid/ask, not a zero-price quote.
  if (it == msg.end() || it->is_null()) return true;
  if (!it->is_array()) return false;
  std::map<Px, Qty> unique;
  for (const auto& level : *it) {
    if (!level.is_array() || level.size() != 2 || !level[0].is_string() || !level[1].is_string()) return false;
    const auto px = kalshi::parse_px(level[0].get<std::string>());
    const auto qty = kalshi::parse_count(level[1].get<std::string>());
    if (!px || !qty || *qty <= 0 || !unique.emplace(no ? kOneDollar - *px : *px, *qty).second) return false;
  }
  if (no) for (const auto& v : unique) out->push_back({v.first, v.second});
  else for (auto p = unique.rbegin(); p != unique.rend(); ++p) out->push_back({p->first, p->second});
  return true;
}
}  // namespace

void MdBooks::watch(const std::vector<std::string>& tickers, std::int64_t now) {
  const std::set<std::string> wanted(tickers.begin(), tickers.end());
  for (auto it = books_.begin(); it != books_.end();) {
    if (!wanted.count(it->first)) {
      it->second.stale = true; it->second.reason = "unwatched"; it->second.recv_ms = now;
      if (callback_) callback_(it->second);
      it = books_.erase(it);
    } else ++it;
  }
  for (const auto& ticker : wanted) {
    if (!books_.count(ticker)) {
      MdBook book; book.ticker = ticker; book.recv_ms = now;
      books_.emplace(ticker, book);
      if (callback_) callback_(book);
    }
  }
}
void MdBooks::disconnected(const std::string& reason, std::int64_t now) {
  subs_.clear();
  for (auto& [ticker, book] : books_) {
    (void)ticker;
    book.stale = true; book.reason = reason; book.recv_ms = now; book.sid = 0; book.seq = 0;
    if (callback_) callback_(book);
  }
}
bool MdBooks::subscribed(std::int64_t sid, const std::vector<std::string>& tickers, std::string* why) {
  if (sid <= 0 || tickers.empty() || subs_.count(sid)) { if (why) *why = "invalid or duplicate subscription"; return false; }
  for (const auto& ticker : tickers) {
    if (!books_.count(ticker) || books_.at(ticker).sid) { if (why) *why = "subscription contains unwatched or duplicate market"; return false; }
  }
  Sub sub; sub.tickers = std::set<std::string>(tickers.begin(), tickers.end());
  subs_.emplace(sid, std::move(sub));
  for (const auto& ticker : tickers) books_.at(ticker).sid = sid;
  return true;
}
bool MdBooks::fail(std::int64_t sid, const std::string& reason, std::int64_t now, std::string* why) {
  if (why) *why = reason;
  auto sub = subs_.find(sid);
  if (sub != subs_.end()) sub->second.broken = true;
  for (auto& [ticker, book] : books_) {
    (void)ticker;
    if (sid && book.sid != sid) continue;
    book.stale = true; book.reason = reason; book.recv_ms = now;
    if (callback_) callback_(book);
  }
  return false;
}
bool MdBooks::message(const json& frame, std::int64_t received, std::string* why) {
  if (!frame.is_object() || !frame.contains("type") || !frame["type"].is_string())
    return fail(0, "malformed market-data envelope", received, why);
  const auto type = frame["type"].get<std::string>();
  const auto sid = integer(frame, "sid");
  if (type == "error") return fail(sid.value_or(0), "venue rejected market-data subscription", received, why);
  const bool is_book = type == "orderbook_snapshot" || type == "orderbook_delta";
  const bool numbered = frame.contains("seq") || is_book;
  if (!numbered) return true;
  const auto seq = integer(frame, "seq");
  if (!sid || !seq || *sid <= 0 || *seq <= 0 || !subs_.count(*sid))
    return fail(0, "missing or unknown market-data sequence", received, why);
  auto& sub = subs_.at(*sid);
  if (sub.broken) return false;
  // The first subscribed frame may start at 2 (documented snapshot example). Thereafter every
  // numbered message, including acknowledgments, must be exactly next. Replays are unsafe too.
  if (sub.last_seq && (*seq <= sub.last_seq || *seq - sub.last_seq != 1))
    return fail(*sid, "market-data sequence gap", received, why);
  sub.last_seq = *seq;
  if (type == "unsubscribed") return fail(*sid, "market-data subscription ended", received, why);
  if (!is_book) return true;
  if (!frame.contains("msg") || !frame["msg"].is_object()) return fail(*sid, "missing book payload", received, why);
  const auto& msg = frame["msg"];
  if (!msg.contains("market_ticker") || !msg["market_ticker"].is_string()) return fail(*sid, "missing book ticker", received, why);
  const auto ticker = msg["market_ticker"].get<std::string>();
  if (!sub.tickers.count(ticker)) return fail(*sid, "book is outside its explicit subscription", received, why);
  auto& previous = books_.at(ticker);
  MdBook next = previous;
  if (type == "orderbook_snapshot") {
    next.bids.clear(); next.asks.clear();
    if (!levels(msg, "yes_dollars_fp", false, &next.bids) || !levels(msg, "no_dollars_fp", true, &next.asks))
      return fail(*sid, "malformed book snapshot", received, why);
  } else {
    if (previous.stale) return fail(*sid, "delta arrived before the market snapshot", received, why);
    if (!msg.contains("side") || !msg["side"].is_string() || !msg.contains("price_dollars") ||
        !msg["price_dollars"].is_string() || !msg.contains("delta_fp") || !msg["delta_fp"].is_string())
      return fail(*sid, "malformed book delta", received, why);
    const auto side = msg["side"].get<std::string>();
    const auto px = kalshi::parse_px(msg["price_dollars"].get<std::string>());
    const auto delta = kalshi::parse_count(msg["delta_fp"].get<std::string>(), true);
    if (!px || !delta || (side != "yes" && side != "no")) return fail(*sid, "invalid book delta units", received, why);
    const Px yes_px = side == "no" ? kOneDollar - *px : *px;
    auto& rows = side == "yes" ? next.bids : next.asks;
    auto level = std::find_if(rows.begin(), rows.end(), [&](const MdLevel& r) { return r.px == yes_px; });
    const Qty before = level == rows.end() ? 0 : level->qty;
    if ((*delta < 0 && *delta < -before) || (*delta > 0 && before > INT64_MAX - *delta))
      return fail(*sid, "book delta creates a negative or overflowing quantity", received, why);
    const Qty after = before + *delta;
    if (level != rows.end()) { if (!after) rows.erase(level); else level->qty = after; }
    else if (after) rows.push_back({yes_px, after});
    std::sort(rows.begin(), rows.end(), [&](const MdLevel& a, const MdLevel& b) { return side == "yes" ? a.px > b.px : a.px < b.px; });
  }
  if (!next.bids.empty() && !next.asks.empty() && next.bids.front().px > next.asks.front().px)
    return fail(*sid, "crossed market-data book", received, why);
  if (!previous.last_change_ms || next.bids != previous.bids || next.asks != previous.asks) next.last_change_ms = received;
  next.seq = *seq; next.recv_ms = received; next.venue_ms = integer(frame, "sending_ts_ms");
  next.stale = false; next.reason.clear(); previous = std::move(next);
  if (callback_) callback_(previous);
  return true;
}
bool MdBooks::ready() const {
  return std::all_of(books_.begin(), books_.end(), [](const auto& p) { return !p.second.stale; });
}

KalshiMd::KalshiMd(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
                   FeedState& feed, Callback callback)
    : KalshiMd(std::move(endpoint), factory, signer, feed, std::move(callback), Options{}) {}
KalshiMd::KalshiMd(net::Endpoint endpoint, net::ConnFactory& factory, const KalshiSigner& signer,
                   FeedState& feed, Callback callback, Options options)
    : endpoint_(std::move(endpoint)), factory_(factory), signer_(signer), feed_(feed),
      callback_(std::move(callback)), options_(options) {
  options_.markets_per_subscription = std::max<std::size_t>(1, options_.markets_per_subscription);
  options_.backoff_min_ms = std::max(20, options_.backoff_min_ms);
  options_.backoff_max_ms = std::max(options_.backoff_min_ms, options_.backoff_max_ms);
}
bool KalshiMd::set_watch(std::vector<std::string> tickers, std::string* why) {
  if (tickers.empty()) { if (why) *why = "empty market subscription refused; use clear_watch"; return false; }
  if (tickers.size() > options_.max_watch || !std::all_of(tickers.begin(), tickers.end(), valid_ticker)) {
    if (why) *why = "invalid or excessive market subscription";
    return false;
  }
  std::sort(tickers.begin(), tickers.end()); tickers.erase(std::unique(tickers.begin(), tickers.end()), tickers.end());
  std::lock_guard<std::mutex> lock(mutex_);
  if (desired_ != tickers) { desired_ = std::move(tickers); ++version_; }
  return true;
}
void KalshiMd::clear_watch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!desired_.empty()) { desired_.clear(); ++version_; }
}
void KalshiMd::run(Worker& self) {
  MdBooks books(callback_);
  int backoff = options_.backoff_min_ms;
  while (!self.stopping()) {
    self.beat();
    std::vector<std::string> watch; std::uint64_t version = 0;
    { std::lock_guard<std::mutex> lock(mutex_); watch = desired_; version = version_; }
    books.watch(watch, now_ms()); books.disconnected("connecting", now_ms());
    feed_.set(false, !watch.empty());
    net::WsClient ws; std::string reason;
    const auto headers = signer_.headers(now_ms(), "GET", endpoint_.base_path);
    bool connected = !headers.empty() && ws.connect(factory_, endpoint_, headers, options_.connect_timeout_ms, &reason, [&] { self.beat(); });
    std::map<std::int64_t, std::vector<std::string>> pending;
    std::int64_t id = 0;
    if (connected) {
      for (std::size_t first = 0; first < watch.size(); first += options_.markets_per_subscription) {
        const auto last = std::min(first + options_.markets_per_subscription, watch.size());
        std::vector<std::string> batch(watch.begin() + first, watch.begin() + last);
        pending[++id] = batch;
        json command{{"id", id}, {"cmd", "subscribe"}, {"params", {{"channels", {"orderbook_delta"}}, {"market_tickers", batch}}}};
        if (!ws.send(net::WsOp::TEXT, command.dump(), 500, &reason, [&] { self.beat(); })) { connected = false; break; }
      }
    }
    const auto started = mono_ms(); auto last_ping = started;
    std::int64_t ping_sent = 0; std::string ping_payload;
    std::deque<int> lags; int lag = -1;
    bool changed = false;
    while (connected && !self.stopping()) {
      self.beat();
      { std::lock_guard<std::mutex> lock(mutex_); changed = version_ != version; }
      if (changed) { reason = "watch list changed"; break; }
      const auto mono = mono_ms();
      if (!books.ready() && mono - started > options_.snapshot_timeout_ms) { reason = "market snapshot timeout"; break; }
      if (ping_sent && mono - ping_sent > options_.pong_timeout_ms) { reason = "market-data pong timeout"; break; }
      if (!ping_sent && mono - last_ping >= options_.ping_ms) {
        ping_payload = "md-" + std::to_string(mono); ping_sent = mono; last_ping = mono;
        if (!ws.send(net::WsOp::PING, ping_payload, 250, &reason)) break;
      }
      feed_.set(true, !books.ready(), lag);
      net::WsEvent event;
      const auto read = ws.read(&event, 100, &reason);
      if (read == net::WsClient::Read::TIMEOUT) continue;
      if (read != net::WsClient::Read::EVENT) break;
      if (event.op == net::WsOp::PONG) { if (ping_sent && event.payload == ping_payload) ping_sent = 0; continue; }
      if (event.op == net::WsOp::PING) continue;
      if (event.op != net::WsOp::TEXT) { reason = "non-text market-data message"; break; }
      const auto frame = json::parse(event.payload, nullptr, false);
      if (!frame.is_object()) { reason = "invalid market-data JSON"; break; }
      if (const auto ts = integer(frame, "sending_ts_ms")) {
        const auto elapsed = now_ms() - *ts;
        if (elapsed >= 0 && elapsed <= INT_MAX) {
          lags.push_back(static_cast<int>(elapsed)); if (lags.size() > 31) lags.pop_front();
          std::vector<int> sorted(lags.begin(), lags.end()); std::sort(sorted.begin(), sorted.end()); lag = sorted[sorted.size() / 2];
        }
      }
      if (!frame.contains("type") || !frame["type"].is_string()) { reason = "missing market-data message type"; break; }
      if (frame["type"].get<std::string>() == "subscribed") {
        const auto command_id = integer(frame, "id");
        if (!command_id || !pending.count(*command_id) || !frame.contains("msg") || !frame["msg"].is_object()) {
          reason = "unexpected subscription acknowledgment"; break;
        }
        const auto& msg = frame["msg"]; const auto sid = integer(msg, "sid");
        if (!sid || !msg.contains("channel") || !msg["channel"].is_string() || msg["channel"].get<std::string>() != "orderbook_delta" ||
            !books.subscribed(*sid, pending.at(*command_id), &reason)) break;
        pending.erase(*command_id);
      } else if (!books.message(frame, now_ms(), &reason)) break;
      if (books.ready() && pending.empty()) backoff = options_.backoff_min_ms;
    }
    ws.close(); feed_.set(false, !watch.empty());
    books.disconnected(reason.empty() ? "market-data disconnected" : reason, now_ms());
    if (changed || self.stopping()) continue;
    const auto until = mono_ms() + backoff;
    while (!self.stopping() && mono_ms() < until) {
      self.beat();
      { std::lock_guard<std::mutex> lock(mutex_); changed = version_ != version; }
      if (changed) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    backoff = std::min(options_.backoff_max_ms, backoff > options_.backoff_max_ms / 2 ? options_.backoff_max_ms : backoff * 2);
  }
  feed_.set(false, false);
}
}  // namespace deskd
