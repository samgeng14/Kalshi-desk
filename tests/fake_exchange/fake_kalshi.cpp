#include "fake_kalshi.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>

#include "rest.h"

namespace fakex {

namespace {
constexpr const char* kRestPrefix = "/trade-api/v2";
constexpr const char* kWsPath = "/trade-api/ws/v2";
constexpr std::size_t kWireLogMax = 50000;

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Market data channels and account channels the venue documents (websocket-connection.md). The
// fake sends nothing on the ones it does not model, but subscribing to them works.
const std::set<std::string>& known_channels() {
  static const std::set<std::string> s = {"orderbook_delta", "ticker", "trade", "fill", "market_positions",
                                          "market_lifecycle_v2", "multivariate_market_lifecycle", "communications",
                                          "order_group_updates", "user_orders", "cfbenchmarks_value",
                                          "cfbenchmarks_value_5hz", "pyth_value"};
  return s;
}
bool account_channel(const std::string& c) {
  return c == "fill" || c == "user_orders" || c == "market_positions" || c == "order_group_updates" ||
         c == "communications";
}

// {"k":v,...} in the order given: the venue writes type, sid, seq, msg, in that order, and a
// reader that looks at the first bytes should see what it would see from Kalshi.
std::string ordered(std::initializer_list<std::pair<const char*, json>> kv) {
  std::string s = "{";
  bool first = true;
  for (const auto& [k, v] : kv) {
    if (v.is_discarded()) continue;  // a field left out
    if (!first) s += ",";
    first = false;
    s += "\"";
    s += k;
    s += "\":";
    s += v.dump();
  }
  return s + "}";
}
json omit() { return json(json::value_t::discarded); }

std::string base64_decode(const std::string& in) {
  std::string out(in.size() / 4 * 3 + 3, '\0');
  const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                reinterpret_cast<const unsigned char*>(in.data()), static_cast<int>(in.size()));
  if (n < 0) return {};
  std::size_t len = static_cast<std::size_t>(n);
  if (!in.empty() && in.back() == '=') --len;
  if (in.size() > 1 && in[in.size() - 2] == '=') --len;
  out.resize(len);
  return out;
}
}  // namespace

void FakeKalshi::Bucket::refill(std::int64_t now) {
  if (now > at_ms) tokens = std::min(cap, tokens + rate * static_cast<double>(now - at_ms) / 1000.0);
  at_ms = std::max(at_ms, now);
}

FakeKalshi::FakeKalshi(FakeConfig cfg)
    : cfg_(std::move(cfg)), engine_(cfg_.engine), private_order_(cfg_.private_order) {
  manual_now_ = cfg_.start_ms ? cfg_.start_ms : wall_ms();
  const std::int64_t now = now_locked();
  read_ = Bucket{cfg_.tier.read_per_s * cfg_.tier.read_burst_s, cfg_.tier.read_per_s,
                 cfg_.tier.read_per_s * cfg_.tier.read_burst_s, now};
  write_ = Bucket{cfg_.tier.write_per_s * cfg_.tier.write_burst_s, cfg_.tier.write_per_s,
                  cfg_.tier.write_per_s * cfg_.tier.write_burst_s, now};
  if (cfg_.auth == AuthMode::VERIFY && !cfg_.public_key_pem.empty()) {
    BIO* bio = BIO_new_mem_buf(cfg_.public_key_pem.data(), static_cast<int>(cfg_.public_key_pem.size()));
    pubkey_ = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
  }
  if (!cfg_.tls_cert_file.empty() || !cfg_.tls_key_file.empty()) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx && SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) == 1 &&
        SSL_CTX_use_certificate_chain_file(ctx, cfg_.tls_cert_file.c_str()) == 1 &&
        SSL_CTX_use_PrivateKey_file(ctx, cfg_.tls_key_file.c_str(), SSL_FILETYPE_PEM) == 1 &&
        SSL_CTX_check_private_key(ctx) == 1) {
      SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
      tls_ctx_ = ctx;
    } else {
      char buf[256];
      ERR_error_string_n(ERR_get_error(), buf, sizeof buf);
      tls_err_ = "TLS listener: cannot load " + cfg_.tls_cert_file + " / " + cfg_.tls_key_file + ": " + buf;
      ERR_clear_error();
      if (ctx) SSL_CTX_free(ctx);
    }
  }
}

FakeKalshi::~FakeKalshi() {
  stop();
  if (pubkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pubkey_));
  if (tls_ctx_) SSL_CTX_free(static_cast<SSL_CTX*>(tls_ctx_));
}

std::string FakeKalshi::rest_base() const {
  return std::string(tls_ctx_ ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_) + kRestPrefix;
}
std::string FakeKalshi::ws_url() const {
  return std::string(tls_ctx_ ? "wss" : "ws") + "://127.0.0.1:" + std::to_string(port_) + kWsPath;
}

std::int64_t FakeKalshi::now_locked() const {
  clock_ms_ = std::max(clock_ms_, cfg_.manual_clock ? manual_now_ : wall_ms());
  return clock_ms_;
}
std::int64_t FakeKalshi::now_ms() const {
  std::lock_guard<std::mutex> lk(mu_);
  return now_locked();
}

bool FakeKalshi::start(std::string* err) {
  if (running_) return true;
  if (cfg_.auth == AuthMode::VERIFY && !pubkey_) {
    if (err) *err = "AuthMode::VERIFY needs a readable public_key_pem";
    return false;
  }
  if (!tls_err_.empty()) {
    if (err) *err = tls_err_;
    return false;
  }
  listen_fd_ = net::listen_loopback(cfg_.port, &port_, err);
  if (listen_fd_ < 0) return false;
  stopping_ = false;
  running_ = true;
  acceptor_ = std::thread([this] { accept_loop(); });
  ticker_ = std::thread([this] { tick_loop(); });
  return true;
}

void FakeKalshi::stop() {
  if (!running_) return;
  stopping_ = true;
  if (acceptor_.joinable()) acceptor_.join();
  if (ticker_.joinable()) ticker_.join();
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    for (const auto& [fd, owned] : conn_fds_) {
      (void)owned;
      ::shutdown(fd, SHUT_RDWR);
    }
  }
  std::vector<Conn> conns;
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    conns.swap(conns_);
  }
  for (auto& c : conns)
    if (c.th.joinable()) c.th.join();
  ::close(listen_fd_);
  listen_fd_ = -1;
  running_ = false;
}

void FakeKalshi::accept_loop() {
  while (!stopping_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) continue;
    ++accepted_;
    net::no_sigpipe(fd);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lk(conn_mu_);
    // reap the connections that have finished: a sender that writes once per connection opens
    // thousands an hour
    for (auto it = conns_.begin(); it != conns_.end();) {
      if (it->done->load()) {
        it->th.join();
        it = conns_.erase(it);
      } else {
        ++it;
      }
    }
    if (tls_ctx_) {
      // the TLS socket belongs to its pump; the pump's socketpair end is what serve() reads. Until
      // the server side of the handshake ends the TCP socket is listed (kind 3), so a drop or a
      // stop landing between the client's Finished and the server's still reaches it.
      conn_fds_[fd] = 3;
      conns_.push_back(Conn{std::thread([this, fd, done] {
                              serve_tls(fd, done);
                              ::close(fd);
                              done->store(true);
                            }),
                            done});
      continue;
    }
    conn_fds_[fd] = 1;
    conns_.push_back(Conn{std::thread([this, fd, done] {
                            serve(fd);
                            {
                              std::lock_guard<std::mutex> g(conn_mu_);
                              conn_fds_.erase(fd);  // before the close, so stop() never shuts a reused number
                            }
                            ::close(fd);
                            done->store(true);
                          }),
                          done});
  }
}

void FakeKalshi::tick_loop() {
  while (!stopping_) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!cfg_.manual_clock) {
        const std::int64_t now = now_locked();
        pump_locked(now);
        engine_.tick(now);
        publish_locked(engine_.drain_events());
      }
      if (cfg_.ws_ping_ms > 0 && steady_ms() - last_ping_ms_ >= cfg_.ws_ping_ms) {
        last_ping_ms_ = steady_ms();
        for (auto& [id, s] : sessions_) enqueue(s, net::ws_frame(net::PING, ""), "ws " + std::to_string(id) + " ping");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.tick_ms));
  }
}

void FakeKalshi::advance_ms(std::int64_t d) {
  std::lock_guard<std::mutex> lk(mu_);
  manual_now_ += d;
  pump_locked(manual_now_);  // each tape event at its own time, so expiries interleave with them
  engine_.tick(now_locked());
  publish_locked(engine_.drain_events());
}

// ---- the tape ---------------------------------------------------------------------------------

void FakeKalshi::play(Tape tape, double speed) {
  std::lock_guard<std::mutex> lk(mu_);
  tape_ = std::move(tape);
  tape_pos_ = 0;
  tape_speed_ = speed > 0 ? speed : 1.0;
  tape_base_ms_ = now_locked();
  engine_.set_tape_debts(true);
  pump_locked(tape_base_ms_);
  publish_locked(engine_.drain_events());
}

bool FakeKalshi::tape_done() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tape_pos_ >= tape_.events.size();
}
std::size_t FakeKalshi::tape_played() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tape_pos_;
}

void FakeKalshi::pump_locked(std::int64_t now) {
  const std::int64_t t0 = tape_.t0();
  auto rebase = [&](std::int64_t tape_ms) {
    return tape_base_ms_ + static_cast<std::int64_t>(static_cast<double>(tape_ms - t0) / tape_speed_);
  };
  while (tape_pos_ < tape_.events.size()) {
    const TapeEvent& e = tape_.events[tape_pos_];
    const std::int64_t due = rebase(e.t_ms);
    if (due > now) break;
    const std::int64_t at = std::max(due, clock_ms_);
    clock_ms_ = at;
    engine_.tick(at);  // expiries and closes due before this event happen first
    auto ensure = [&](const std::string& ticker) {
      if (engine_.market(ticker)) return;
      MarketSpec s = default_spec(ticker);
      if (s.close_ms) s.close_ms = rebase(s.close_ms);
      engine_.add_market(std::move(s), at);
    };
    switch (e.kind) {
      case TapeEvent::Kind::MARKET: {
        MarketSpec s = *e.spec;
        if (s.close_ms) s.close_ms = rebase(s.close_ms);
        engine_.add_market(std::move(s), at);
        break;
      }
      case TapeEvent::Kind::SNAPSHOT: {
        const Market* m = engine_.market(e.ticker);
        // a re-subscription after a reconnect sends closed windows again (recorded 05:20:39Z
        // 2026-09-24): a closed market stays closed
        if (m && m->state == Market::State::CLOSED) break;
        ensure(e.ticker);
        engine_.street_snapshot(e.ticker, e.yes, e.no, at);
        break;
      }
      case TapeEvent::Kind::DELTA:
        ensure(e.ticker);
        if (e.qty > 0)
          engine_.street_add(e.ticker, e.no_side, e.px, e.qty, at);
        else
          engine_.street_remove(e.ticker, e.no_side, e.px, -e.qty, at);
        break;
      case TapeEvent::Kind::TRADE:
        ensure(e.ticker);
        engine_.street_take(e.ticker, e.side, e.px, e.qty, at);
        break;
      case TapeEvent::Kind::PAUSE:
        engine_.pause(e.ticker, at);
        break;
      case TapeEvent::Kind::RESUME:
        engine_.resume(e.ticker, at);
        break;
      case TapeEvent::Kind::CLOSE:
        engine_.close(e.ticker, at);
        break;
    }
    ++tape_pos_;
  }
}

// ---- faults and settings ----------------------------------------------------------------------

void FakeKalshi::add_fault(HttpFault f) {
  std::lock_guard<std::mutex> lk(mu_);
  faults_.push_back(std::move(f));
}
void FakeKalshi::clear_faults() {
  std::lock_guard<std::mutex> lk(mu_);
  faults_.clear();
}
std::size_t FakeKalshi::faults_pending() const {
  std::lock_guard<std::mutex> lk(mu_);
  return faults_.size();
}
void FakeKalshi::set_private_order(PrivateOrder p) {
  std::lock_guard<std::mutex> lk(mu_);
  private_order_ = p;
}
void FakeKalshi::set_rate_limits(bool on, RateTier tier) {
  std::lock_guard<std::mutex> lk(mu_);
  cfg_.rate_limits = on;
  cfg_.tier = tier;
  const std::int64_t now = now_locked();
  read_ = Bucket{tier.read_per_s * tier.read_burst_s, tier.read_per_s, tier.read_per_s * tier.read_burst_s, now};
  write_ = Bucket{tier.write_per_s * tier.write_burst_s, tier.write_per_s, tier.write_per_s * tier.write_burst_s, now};
  write_shard_.clear();
}
void FakeKalshi::drop_ws_connections() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [id, s] : sessions_) {
    (void)id;
    ::shutdown(s->fd, SHUT_RDWR);
  }
}
void FakeKalshi::drop_http_connections() {
  // Under conn_mu_: a connection's thread takes its fd out of conn_fds_ before closing it, so a
  // number listed here is still that connection's.
  std::lock_guard<std::mutex> lk(conn_mu_);
  for (auto& [fd, kind] : conn_fds_) {
    if (kind == 1) ::shutdown(fd, SHUT_RDWR);
    if (kind == 3) kind = 4;   // a TLS handshake still ending: dropped as soon as it is served
  }
}
void FakeKalshi::drop_ws_messages(const std::string& channel, int n) {
  std::lock_guard<std::mutex> lk(mu_);
  drop_left_[channel] += n;
}
void FakeKalshi::duplicate_ws_messages(const std::string& channel, int n) {
  std::lock_guard<std::mutex> lk(mu_);
  dup_left_[channel] += n;
}
void FakeKalshi::fragment_ws(std::size_t frame_bytes) { fragment_bytes_ = frame_bytes; }
void FakeKalshi::stall_ws(int ms) { stall_until_ms_ = steady_ms() + ms; }

bool FakeKalshi::take_fault_locked(const net::Request& req, HttpFault* f) {
  const std::string path = req.path.compare(0, std::char_traits<char>::length(kRestPrefix), kRestPrefix) == 0
                               ? req.path.substr(std::char_traits<char>::length(kRestPrefix))
                               : req.path;
  for (auto it = faults_.begin(); it != faults_.end(); ++it) {
    if (it->method != req.method || path.compare(0, it->path.size(), it->path) != 0) continue;
    *f = *it;
    if (--it->times <= 0) faults_.erase(it);
    return true;
  }
  return false;
}

// ---- the wire log -----------------------------------------------------------------------------

void FakeKalshi::log_wire(const std::string& line) {
  std::lock_guard<std::mutex> lk(log_mu_);
  wire_.push_back(line);
  if (wire_.size() > kWireLogMax) wire_.pop_front();
}
std::vector<std::string> FakeKalshi::wire_log() const {
  std::lock_guard<std::mutex> lk(log_mu_);
  return std::vector<std::string>(wire_.begin(), wire_.end());
}
void FakeKalshi::clear_wire_log() {
  std::lock_guard<std::mutex> lk(log_mu_);
  wire_.clear();
}
std::size_t FakeKalshi::ws_connections() const {
  std::lock_guard<std::mutex> lk(mu_);
  return sessions_.size();
}

bool FakeKalshi::wait_ws_flushed(int ms) {
  std::vector<std::shared_ptr<Session>> ss;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, s] : sessions_) {
      (void)id;
      ss.push_back(s);
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (auto& s : ss) {
    std::unique_lock<std::mutex> lk(s->m);
    if (!s->cv.wait_until(lk, deadline, [&] { return s->written >= s->queued || s->dead; })) return false;
  }
  return true;
}

// ---- the socket's writer ----------------------------------------------------------------------

std::string FakeKalshi::frame_text(const std::string& payload) const { return net::ws_text(payload, fragment_bytes_); }

void FakeKalshi::enqueue(const std::shared_ptr<Session>& s, std::string bytes, std::string label, int sid) {
  {
    std::lock_guard<std::mutex> lk(s->m);
    if (s->dead || s->closing) return;
    s->out.push_back(OutItem{std::move(bytes), std::move(label), sid});
    if (sid) ++s->pending[sid];
    ++s->queued;
  }
  s->cv.notify_all();
}

void FakeKalshi::writer_loop(const std::shared_ptr<Session>& s) {
  auto unpend = [&s](int sid) {   // under s->m
    if (!sid) return;
    auto it = s->pending.find(sid);
    if (it != s->pending.end() && --it->second == 0) s->pending.erase(it);
  };
  for (;;) {
    OutItem item;
    {
      std::unique_lock<std::mutex> lk(s->m);
      s->cv.wait(lk, [&] { return s->closing || !s->out.empty(); });
      if (s->out.empty()) return;  // closing, and everything queued is out
      item = std::move(s->out.front());
      s->out.pop_front();
      if (s->dead) {
        ++s->written;
        unpend(item.sid);
        lk.unlock();
        s->cv.notify_all();
        continue;
      }
    }
    while (steady_ms() < stall_until_ms_.load() && !stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool ok = net::write_all(s->fd, item.bytes);
    if (ok && !item.label.empty()) log_wire(item.label);  // before `written` moves: see EVENTS_FIRST
    {
      std::lock_guard<std::mutex> lk(s->m);
      ++s->written;
      unpend(item.sid);
      if (!ok) s->dead = true;
    }
    s->cv.notify_all();
  }
}

// ---- fan-out ----------------------------------------------------------------------------------

void FakeKalshi::deliver_locked(const std::shared_ptr<Session>& s, int sid, Sub& sub, const std::string& type,
                                const json& msg, bool seq, std::int64_t now, Targets* targets) {
  if (sub.overflowed) return;
  if (cfg_.ws_sub_buffer > 0) {
    // A reader that stopped reading: the venue drops the subscription with error 25 rather than
    // queue without end (the fake's outbound queue had no bound until 2026-10-07).
    std::size_t unwritten = 0;
    {
      std::lock_guard<std::mutex> lk(s->m);
      auto it = s->pending.find(sid);
      unwritten = it == s->pending.end() ? 0 : it->second;
    }
    if (unwritten >= cfg_.ws_sub_buffer) {
      sub.overflowed = true;
      enqueue(s,
              frame_text(ordered({{"type", "error"},
                                  {"sid", sid},
                                  {"msg", json{{"code", 25}, {"msg", "Subscription buffer overflow"}}},
                                  {"sending_ts_ms", now}})),
              "ws " + std::to_string(s->id) + " error 25 " + std::to_string(sid));
      return;
    }
  }
  auto drop = drop_left_.find(sub.channel);
  if (drop != drop_left_.end() && drop->second > 0) {
    // lost on the way: the number is spent, so the reader sees a gap
    --drop->second;
    if (seq) ++sub.seq;
    log_wire("ws " + std::to_string(s->id) + " lost " + type);
    return;
  }
  const std::int64_t n = seq ? ++sub.seq : 0;
  const std::string payload = ordered({{"type", type},
                                       {"sid", sid},
                                       {"seq", seq ? json(n) : omit()},
                                       {"msg", msg},
                                       {"sending_ts_ms", now}});
  std::string key;
  for (const char* k : {"trade_id", "order_id", "order_group_id", "market_ticker"}) {
    if (msg.is_object() && msg.contains(k) && msg[k].is_string()) {
      key = msg[k].get<std::string>();
      break;
    }
  }
  std::string label = "ws " + std::to_string(s->id) + " " + type + " " + key;
  if (seq) label += " " + std::to_string(n);
  const std::string frame = frame_text(payload);
  enqueue(s, frame, label, sid);
  auto dup = dup_left_.find(sub.channel);
  if (dup != dup_left_.end() && dup->second > 0) {
    --dup->second;
    enqueue(s, frame, label + " again", sid);
  }
  if (targets) {
    std::lock_guard<std::mutex> lk(s->m);
    targets->emplace_back(s, s->queued);
  }
}

void FakeKalshi::send_snapshot_locked(const std::shared_ptr<Session>& s, int sid, Sub& sub, const std::string& ticker,
                                      std::int64_t now, Targets* targets) {
  const Market* m = engine_.market(ticker);
  json msg{{"market_ticker", ticker}, {"market_id", m ? m->spec.market_id : std::string()}};
  // levels as [price_dollars, count_fp], ascending; a side with no levels is absent (recorded
  // closes carry neither)
  for (const bool no_side : {false, true}) {
    if (!m || m->state == Market::State::CLOSED) break;
    const auto lv = engine_.levels(ticker, no_side);
    if (lv.empty()) continue;
    json a = json::array();
    for (const auto& [px, q] : lv) a.push_back(json::array({px_str(px), qty_str(q)}));
    msg[no_side ? "no_dollars_fp" : "yes_dollars_fp"] = a;
  }
  deliver_locked(s, sid, sub, "orderbook_snapshot", msg, true, now, targets);
}

namespace {
// what orders a private message behind earlier ones: its order, its group, its position
std::set<std::string> held_keys(const Event& e) {
  std::set<std::string> k;
  if (!account_channel(e.channel) || !e.msg.is_object()) return k;
  if (e.msg.contains("order_id") && e.msg["order_id"].is_string()) k.insert("o:" + e.msg["order_id"].get<std::string>());
  if (e.msg.contains("order_group_id") && e.msg["order_group_id"].is_string())
    k.insert("g:" + e.msg["order_group_id"].get<std::string>());
  if (e.channel == "market_positions" && e.msg.contains("market_ticker") && e.msg["market_ticker"].is_string())
    k.insert("p:" + e.msg["market_ticker"].get<std::string>());
  return k;
}
bool meets(const std::set<std::string>& a, const std::set<std::string>& b) {
  for (const auto& x : a)
    if (b.count(x)) return true;
  return false;
}
}  // namespace

FakeKalshi::Targets FakeKalshi::publish_locked(const std::vector<Event>& events) {
  if (held_.empty()) return publish_now_locked(events);
  std::set<std::string> blocked;
  for (const Held& h : held_) blocked.insert(h.keys.begin(), h.keys.end());
  std::vector<Event> now, later;
  for (const Event& e : events) {
    const std::set<std::string> k = held_keys(e);
    if (!k.empty() && meets(k, blocked)) {
      blocked.insert(k.begin(), k.end());   // the rest of this order's messages follow it
      later.push_back(e);
    } else {
      now.push_back(e);
    }
  }
  if (!later.empty()) hold_locked(std::move(later), true);
  return publish_now_locked(now);
}

std::uint64_t FakeKalshi::hold_locked(std::vector<Event> events, bool ready) {
  Held h;
  h.id = next_held_++;
  for (const Event& e : events) {
    const std::set<std::string> k = held_keys(e);
    h.keys.insert(k.begin(), k.end());
  }
  h.events = std::move(events);
  h.ready = ready;
  held_.push_back(std::move(h));
  return held_.back().id;
}

void FakeKalshi::release_locked(std::uint64_t id) {
  for (Held& h : held_)
    if (h.id == id) h.ready = true;
  std::set<std::string> blocked;
  std::deque<Held> keep;
  for (Held& h : held_) {
    if (h.ready && !meets(h.keys, blocked)) {
      publish_now_locked(h.events);
    } else {
      blocked.insert(h.keys.begin(), h.keys.end());
      keep.push_back(std::move(h));
    }
  }
  held_.swap(keep);
}

void FakeKalshi::sweep_locked(const std::shared_ptr<Session>& s) {
  for (auto it = s->subs.begin(); it != s->subs.end();) {
    if (it->second.overflowed) it = s->subs.erase(it);
    else ++it;
  }
}

FakeKalshi::Targets FakeKalshi::publish_now_locked(const std::vector<Event>& events) {
  Targets t;
  if (events.empty()) return t;
  const std::int64_t now = now_locked();
  for (const Event& e : events) {
    for (auto& [id, s] : sessions_) {
      (void)id;
      for (auto& [sid, sub] : s->subs) {
        if (sub.channel != e.channel) continue;
        if (!e.ticker.empty() && !sub.all &&
            std::find(sub.markets.begin(), sub.markets.end(), e.ticker) == sub.markets.end())
          continue;
        if (e.reset_book)
          send_snapshot_locked(s, sid, sub, e.ticker, now, &t);
        else
          deliver_locked(s, sid, sub, e.type, e.msg, e.seq, now, &t);
      }
    }
  }
  for (auto& [id, s] : sessions_) {
    (void)id;
    sweep_locked(s);
  }
  return t;
}

// ---- WebSocket commands -----------------------------------------------------------------------

void FakeKalshi::ws_command_locked(const std::shared_ptr<Session>& s, const std::string& text) {
  const std::int64_t now = now_locked();
  if (cfg_.ws_cmds_per_s > 0) {
    while (!s->cmd_ms.empty() && now - s->cmd_ms.front() >= 1000) s->cmd_ms.pop_front();
    if (s->cmd_ms.size() >= static_cast<std::size_t>(cfg_.ws_cmds_per_s)) {
      const json j = json::parse(text, nullptr, false);
      const json id = j.is_object() && j.contains("id") && j["id"].is_number_integer() && j["id"].get<long long>() != 0
                          ? j["id"]
                          : omit();
      enqueue(s,
              frame_text(ordered({{"id", id},
                                  {"type", "error"},
                                  {"msg", json{{"code", 27}, {"msg", "Too many requests"}}},
                                  {"sending_ts_ms", now}})),
              "ws " + std::to_string(s->id) + " error 27");
      return;   // refused: it does not count toward the next second
    }
    s->cmd_ms.push_back(now);
  }
  ws_command_inner_locked(s, text);
  sweep_locked(s);
}

void FakeKalshi::ws_command_inner_locked(const std::shared_ptr<Session>& s, const std::string& text) {
  const std::int64_t now = now_locked();
  const std::string tag = "ws " + std::to_string(s->id) + " ";
  const json j = json::parse(text, nullptr, false);
  const json id = j.is_object() && j.contains("id") && j["id"].is_number_integer() && j["id"].get<long long>() != 0
                      ? j["id"]
                      : omit();  // "omitted if command had id=0"
  auto error = [&](int code, const std::string& m, const json& sid = omit()) {
    enqueue(s,
            frame_text(ordered({{"id", id},
                                {"type", "error"},
                                {"sid", sid},
                                {"msg", json{{"code", code}, {"msg", m}}},
                                {"sending_ts_ms", now}})),
            tag + "error " + std::to_string(code));
  };
  if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) return error(1, "Unable to process message");
  const std::string cmd = j["cmd"].get<std::string>();
  const json params = j.contains("params") ? j["params"] : json();
  // market_ticker or market_tickers, never both (websocket-connection.md); empty when neither
  auto markets_of = [&](std::vector<std::string>* out) -> bool {
    out->clear();
    const bool one = params.contains("market_ticker"), many = params.contains("market_tickers");
    if (one && many) return false;
    if (one) {
      if (!params["market_ticker"].is_string()) return false;
      out->push_back(params["market_ticker"].get<std::string>());
    }
    if (many) {
      if (!params["market_tickers"].is_array()) return false;
      for (const auto& t : params["market_tickers"]) {
        if (!t.is_string()) return false;
        out->push_back(t.get<std::string>());
      }
    }
    return true;
  };
  auto initial = [&](int sid, Sub& sub, const std::vector<std::string>& tickers, bool ticker_snapshot) {
    if (sub.channel == "orderbook_delta") {
      for (const auto& t : tickers) send_snapshot_locked(s, sid, sub, t, now, nullptr);
    } else if (sub.channel == "ticker" && ticker_snapshot) {
      for (const auto& t : tickers) {
        const Market* m = engine_.market(t);
        if (m) deliver_locked(s, sid, sub, "ticker", engine_.ticker_msg(*m, now), false, now, nullptr);
      }
    }
  };
  auto all_tickers = [&] {
    std::vector<std::string> v;
    for (const auto& [t, m] : engine_.markets())
      if (m.state != Market::State::CLOSED) v.push_back(t);
    return v;
  };

  if (cmd == "subscribe") {
    if (!params.is_object()) return error(2, "Params required");
    if (!params.contains("channels") || !params["channels"].is_array() || params["channels"].empty())
      return error(3, "Channels required");
    std::vector<std::string> channels;
    for (const auto& c : params["channels"]) {
      if (!c.is_string() || !known_channels().count(c.get<std::string>())) return error(8, "Unknown channel name");
      channels.push_back(c.get<std::string>());
    }
    for (const auto& c : channels)
      if (account_channel(c) && !s->authed) return error(9, "Authentication required");
    std::vector<std::string> markets;
    if (!markets_of(&markets)) return error(11, "Invalid parameter");
    if (cfg_.ws_sub_market_limit > 0 && markets.size() > cfg_.ws_sub_market_limit)
      return error(26, "Subscription market limit exceeded");
    const bool snap = params.value("send_initial_snapshot", false);
    for (const auto& c : channels) {
      const int sid = s->next_sid++;
      Sub& sub = s->subs[sid];
      sub.channel = c;
      sub.all = markets.empty();  // no list: every market (the old recorder's flood)
      sub.markets = markets;
      enqueue(s,
              frame_text(ordered({{"id", id},
                                  {"type", "subscribed"},
                                  {"msg", json{{"channel", c}, {"sid", sid}}},
                                  {"sending_ts_ms", now}})),
              tag + "subscribed " + c + " " + std::to_string(sid));
      initial(sid, sub, sub.all ? all_tickers() : markets, snap);
    }
    return;
  }
  if (cmd == "update_subscription") {
    if (!params.is_object()) return error(2, "Params required");
    if (!params.contains("action") || !params["action"].is_string()) return error(15, "Action required");
    const std::string action = params["action"].get<std::string>();
    if (action != "add_markets" && action != "delete_markets" && action != "get_snapshot")
      return error(13, "Unsupported action");
    int sid = 0;
    if (params.contains("sids")) {
      if (!params["sids"].is_array() || params["sids"].size() != 1 || !params["sids"][0].is_number_integer())
        return error(12, "Exactly one subscription ID required");
      sid = params["sids"][0].get<int>();
    } else if (params.contains("sid") && params["sid"].is_number_integer()) {
      sid = params["sid"].get<int>();
    } else {
      return error(12, "Exactly one subscription ID required");
    }
    auto it = s->subs.find(sid);
    if (it == s->subs.end()) return error(7, "Unknown subscription ID", sid);
    Sub& sub = it->second;
    std::vector<std::string> markets;
    if (!markets_of(&markets)) return error(11, "Invalid parameter", sid);
    if (markets.empty() && action != "get_snapshot") return error(14, "Market Ticker required", sid);
    std::vector<std::string> added;
    if (action == "add_markets" && cfg_.ws_sub_market_limit > 0 && !sub.all) {
      std::set<std::string> after(sub.markets.begin(), sub.markets.end());
      after.insert(markets.begin(), markets.end());
      if (after.size() > cfg_.ws_sub_market_limit) return error(26, "Subscription market limit exceeded", sid);
    }
    if (action == "add_markets") {
      for (const auto& t : markets)
        if (!sub.all && std::find(sub.markets.begin(), sub.markets.end(), t) == sub.markets.end()) {
          sub.markets.push_back(t);
          added.push_back(t);
        }
    } else if (action == "delete_markets") {
      for (const auto& t : markets) sub.markets.erase(std::remove(sub.markets.begin(), sub.markets.end(), t), sub.markets.end());
    } else {
      added = markets.empty() ? (sub.all ? all_tickers() : sub.markets) : markets;
    }
    // the reply takes a number in the sid's sequence
    const json listed = params.value("skip_ticker_ack", false) ? omit() : json{{"market_tickers", sub.markets}};
    const std::int64_t n = ++sub.seq;
    enqueue(s,
            frame_text(ordered({{"id", id},
                                {"sid", sid},
                                {"seq", n},
                                {"type", "ok"},
                                {"msg", listed.is_discarded() ? json::object() : listed},
                                {"sending_ts_ms", now}})),
            tag + "ok " + action + " " + std::to_string(sid) + " " + std::to_string(n));
    // a delta stream joined mid-flight is not a book: added markets get their snapshot
    initial(sid, sub, added, action == "get_snapshot" || params.value("send_initial_snapshot", false));
    return;
  }
  if (cmd == "unsubscribe") {
    if (!params.is_object()) return error(2, "Params required");
    if (!params.contains("sids") || !params["sids"].is_array() || params["sids"].empty())
      return error(4, "Subscription IDs required");
    for (const auto& v : params["sids"]) {
      const int sid = v.is_number_integer() ? v.get<int>() : 0;
      auto it = s->subs.find(sid);
      if (it == s->subs.end()) {
        error(7, "Unknown subscription ID", sid);
        continue;
      }
      const std::int64_t n = ++it->second.seq;
      s->subs.erase(it);
      enqueue(s,
              frame_text(ordered({{"id", id}, {"sid", sid}, {"seq", n}, {"type", "unsubscribed"}, {"sending_ts_ms", now}})),
              tag + "unsubscribed " + std::to_string(sid));
    }
    return;
  }
  if (cmd == "list_subscriptions") {
    json list = json::array();
    for (const auto& [sid, sub] : s->subs) list.push_back(json{{"channel", sub.channel}, {"sid", sid}});
    enqueue(s, frame_text(ordered({{"id", id}, {"type", "ok"}, {"msg", list}, {"sending_ts_ms", now}})),
            tag + "ok list");
    return;
  }
  error(5, "Unknown command");
}

// ---- auth and limits --------------------------------------------------------------------------

std::string FakeKalshi::auth_refusal(const net::Request& req, std::int64_t now) const {
  if (cfg_.auth == AuthMode::OFF) return "";
  const std::string key = req.header("kalshi-access-key"), ts = req.header("kalshi-access-timestamp"),
                    sig = req.header("kalshi-access-signature");
  if (key.empty() || ts.empty() || sig.empty()) return "KALSHI-ACCESS-KEY, -TIMESTAMP and -SIGNATURE are required";
  if (ts.size() > 15 || ts.find_first_not_of("0123456789") != std::string::npos)
    return "KALSHI-ACCESS-TIMESTAMP must be milliseconds";
  if (cfg_.auth_max_skew_ms > 0 && std::llabs(std::stoll(ts) - now) > cfg_.auth_max_skew_ms)
    return "KALSHI-ACCESS-TIMESTAMP is too far from the exchange clock";
  if (cfg_.auth == AuthMode::VERIFY) {
    const std::string text = ts + req.method + req.path;  // the path without its query
    const std::string raw = base64_decode(sig);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    EVP_PKEY* pk = static_cast<EVP_PKEY*>(pubkey_);
    // an Ed25519 key signs the message itself (api_keys.md, keys allowed since the 2026-10-01
    // changelog); an RSA key signs it with PSS over SHA-256, salt the digest's length
    const bool ed = EVP_PKEY_get_base_id(pk) == EVP_PKEY_ED25519;
    const bool init = ed ? EVP_DigestVerifyInit(ctx, &pctx, nullptr, nullptr, pk) == 1
                         : EVP_DigestVerifyInit(ctx, &pctx, EVP_sha256(), nullptr, pk) == 1 &&
                               EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) == 1 &&
                               EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) == 1;
    const bool ok = !raw.empty() && init &&
                    EVP_DigestVerify(ctx, reinterpret_cast<const unsigned char*>(raw.data()), raw.size(),
                                     reinterpret_cast<const unsigned char*>(text.data()), text.size()) == 1;
    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
    if (!ok) return "signature does not verify";
  }
  return "";
}

bool FakeKalshi::rate_ok_locked(const net::Request& req, const std::string& path, const json& body, std::int64_t now) {
  if (!cfg_.rate_limits) return true;
  std::vector<Bucket*> buckets;
  double cost = 10;  // "Most requests cost the default of 10 tokens"
  auto shard_bucket = [&](int shard) -> Bucket* {
    auto it = write_shard_.find(shard);
    if (it == write_shard_.end())
      it = write_shard_.emplace(shard, Bucket{write_.cap, write_.rate, write_.cap, now}).first;
    return &it->second;
  };
  // a single order write: an explicit nonzero shard bills that shard; shard 0 the unscoped bucket;
  // auto-routed the unscoped bucket and every nonzero shard (exchange_sharding.md FAQ)
  auto single = [&](int shard) {
    if (shard > 0) {
      buckets.push_back(shard_bucket(shard));
    } else {
      buckets.push_back(&write_);
      if (shard < 0)
        for (const int s : engine_.shards())
          if (s > 0) buckets.push_back(shard_bucket(s));
    }
  };
  const bool cancel_path = path.compare(0, 24, "/portfolio/events/orders") == 0;
  if (req.method == "GET") {
    buckets.push_back(&read_);
    if (path.compare(0, 18, "/portfolio/orders/") == 0) cost = 2;  // GET one order
  } else if (req.method == "POST" && path == "/portfolio/events/orders") {
    const int shard = body.is_object() && body.contains("exchange_index") && body["exchange_index"].is_number_integer()
                          ? body["exchange_index"].get<int>()
                          : -1;
    single(shard);
  } else if (req.method == "DELETE" && path == "/portfolio/events/orders/batched") {
    // "Every item in the batch is billed separately ... The whole batch must fit in the bucket"
    const std::size_t n = body.is_object() && body.contains("orders") && body["orders"].is_array() ? body["orders"].size() : 1;
    cost = 2.0 * static_cast<double>(std::max<std::size_t>(n, 1));
    buckets.push_back(&write_);
  } else if (req.method == "DELETE" && cancel_path && path.size() > 25) {
    cost = 2;
    auto it = req.params.find("exchange_index");
    single(it == req.params.end() ? -1 : std::atoi(it->second.c_str()));
  } else if (req.method == "DELETE" && path == "/portfolio/events/orders") {
    cost = 2;  // cancel-all
    buckets.push_back(&write_);
  } else {
    buckets.push_back(&write_);
  }
  for (Bucket* b : buckets) b->refill(now);
  for (Bucket* b : buckets)
    if (b->tokens < cost) return false;
  for (Bucket* b : buckets) b->tokens -= cost;
  return true;
}

FakeKalshi::Reply FakeKalshi::route_locked(const net::Request& req, const std::string& path, const json& body,
                                           std::int64_t now, std::string* order_id_out) {
  const RestReply r = rest_route(engine_, req, path, body, now, order_id_out);
  return Reply{r.status, r.body};
}

// ---- one connection ---------------------------------------------------------------------------

void FakeKalshi::serve(int fd) {
  std::string buf;
  for (;;) {
    net::Request req;
    std::string why;
    const net::ReadResult rr = net::read_request(fd, buf, req, cfg_.idle_close_ms, &why);
    if (rr == net::ReadResult::TIMEOUT && !stopping_) {
      // the load balancer's idle close of a kept-open connection (with TLS: close_notify, unless
      // tls_close_notify is off)
      ++idle_closes_;
      log_wire("http idle-close");
      return;
    }
    if (rr == net::ReadResult::CLOSED || rr == net::ReadResult::TIMEOUT || stopping_) return;
    if (rr == net::ReadResult::BAD) {
      const RestReply e = rest_error(400, "bad_request", why.empty() ? "malformed request" : why);
      net::write_all(fd, net::response_head(400, {{"Content-Type", "application/json"},
                                                  {"Content-Length", std::to_string(e.body.size())},
                                                  {"Connection", "close"}}) +
                             e.body);
      return;
    }
    ++requests_;
    if (net::lower(req.header("upgrade")) == "websocket") {
      serve_ws(fd, req);
      return;
    }
    const std::size_t plen = std::char_traits<char>::length(kRestPrefix);
    const bool ours = req.path.compare(0, plen, kRestPrefix) == 0;
    const std::string path = ours ? req.path.substr(plen) : std::string();
    const std::string what = req.method + " " + (ours ? path : req.path);

    HttpFault f;
    bool faulted;
    {
      std::lock_guard<std::mutex> lk(mu_);
      faulted = take_fault_locked(req, &f);
    }
    using K = HttpFault::Kind;
    auto is = [&](K k) { return faulted && f.kind == k; };
    if (is(K::RATE_LIMIT)) {
      const std::string body = R"({"error":"too many requests"})";
      net::write_all(fd, net::response_head(429, {{"Content-Type", "application/json"},
                                                  {"Content-Length", std::to_string(body.size())}}) +
                             body);
      log_wire("http 429 " + what + " (fault)");
      if (!req.keep_alive) return;
      continue;
    }
    if (is(K::DROP_BEFORE_REPLY)) {
      log_wire("http drop-before-reply " + what);
      return;
    }
    if (is(K::STALL_BEFORE)) std::this_thread::sleep_for(std::chrono::milliseconds(f.stall_ms));

    const json body = req.body.empty() ? json() : json::parse(req.body, nullptr, false);
    Reply rep;
    Targets wait_for;
    std::uint64_t held = 0;  // private messages held until the reply is written (Held)
    {
      std::lock_guard<std::mutex> lk(mu_);
      const std::int64_t now = now_locked();
      const bool private_path = ours && path.compare(0, 10, "/portfolio") == 0;
      const std::string denied = private_path ? auth_refusal(req, now) : std::string();
      std::string created;
      if (!ours) {
        const RestReply e = rest_error(404, "not_found", "no such route");
        rep = Reply{e.status, e.body};
      } else if (!denied.empty()) {
        const RestReply e = rest_error(401, "authentication_error", denied);
        rep = Reply{e.status, e.body};
      } else if (!rate_ok_locked(req, path, body, now)) {
        rep = Reply{429, R"({"error":"too many requests"})"};
      } else {
        // The street's fill lands in the matching engine before the cancel does, so its messages
        // come first among this request's; when they reach the client is private_order's business.
        if (is(K::FILL_DURING_CANCEL) && req.method == "DELETE")
          engine_.street_hit(path.substr(path.rfind('/') + 1), f.qty, now);
        rep = route_locked(req, path, body, now, &created);
        if (is(K::HIT_AFTER_REST) && rep.status == 201 && !created.empty()) {
          const Order* o = engine_.order(created);
          if (o && o->in_book) engine_.street_hit(created, f.qty, now);
        }
      }
      PrivateOrder mode = private_order_;
      if (is(K::FILL_BEFORE_REPLY) || is(K::HIT_AFTER_REST)) mode = PrivateOrder::EVENTS_FIRST;
      if (is(K::FILL_DURING_CANCEL)) mode = PrivateOrder::REPLY_FIRST;
      if (faulted && f.private_order != PrivateOrder::NATURAL) mode = f.private_order;
      std::vector<Event> now_events, after;
      for (Event& e : engine_.drain_events())
        (mode == PrivateOrder::REPLY_FIRST && account_channel(e.channel) ? after : now_events).push_back(std::move(e));
      Targets t = publish_locked(now_events);
      if (!after.empty()) held = hold_locked(std::move(after), false);
      if (mode == PrivateOrder::EVENTS_FIRST) wait_for = std::move(t);
    }
    if (!wait_for.empty()) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      for (auto& target : wait_for) {
        Session& s = *target.first;
        const std::uint64_t n = target.second;
        std::unique_lock<std::mutex> lk(s.m);
        s.cv.wait_until(lk, deadline, [&] { return s.written >= n || s.dead; });
      }
    }
    if (is(K::STALL_AFTER)) std::this_thread::sleep_for(std::chrono::milliseconds(f.stall_ms));
    if (is(K::STATUS_AFTER_ACCEPT)) {
      const RestReply e = rest_error(f.status, "internal_server_error", "internal server error");
      rep = Reply{f.status, e.body};
    }

    const bool close_after = !req.keep_alive || is(K::CLOSE_AFTER_REPLY) || is(K::CUT_BODY) || is(K::DROP_AFTER_ACCEPT);
    if (is(K::DROP_AFTER_ACCEPT)) {
      log_wire("http drop-after-accept " + std::to_string(rep.status) + " " + what);
    } else {
      std::vector<std::pair<std::string, std::string>> h;
      if (rep.status != 204) h.emplace_back("Content-Type", "application/json");
      if (is(K::CHUNKED))
        h.emplace_back("Transfer-Encoding", "chunked");
      else if (rep.status != 204)
        h.emplace_back("Content-Length", std::to_string(rep.body.size()));
      h.emplace_back("Connection", close_after ? "close" : "keep-alive");
      std::string data = net::response_head(rep.status, h);
      std::string label = "http " + std::to_string(rep.status) + " " + what;
      if (is(K::CHUNKED)) {
        data += net::chunked_body(rep.body, f.chunk_bytes);
        label += " (chunked)";
      } else if (is(K::CUT_BODY)) {
        const std::size_t cut = f.cut_bytes ? std::min(f.cut_bytes, rep.body.size() ? rep.body.size() - 1 : 0)
                                            : rep.body.size() / 2;
        data += rep.body.substr(0, cut);
        label = "http cut " + std::to_string(rep.status) + " " + what;
      } else {
        data += rep.body;
      }
      net::write_all(fd, data);
      log_wire(label);
    }
    if (held) {
      std::lock_guard<std::mutex> lk(mu_);
      release_locked(held);
    }
    if (close_after) return;
  }
}

// One TLS connection: the handshake, then a pump between the TLS socket and one end of a
// socketpair while serve() reads the other end as it would a plain connection. Both sockets are
// non-blocking and the pump is the only thread that touches the SSL object.
void FakeKalshi::serve_tls(int tcp, std::shared_ptr<std::atomic<bool>> done) {
  (void)done;
  ::fcntl(tcp, F_SETFL, ::fcntl(tcp, F_GETFL, 0) | O_NONBLOCK);
  SSL* ssl = SSL_new(static_cast<SSL_CTX*>(tls_ctx_));
  if (!ssl) return;
  SSL_set_fd(ssl, tcp);
  const std::int64_t hs_deadline = steady_ms() + cfg_.tls_handshake_ms;
  for (;;) {
    ERR_clear_error();
    const int r = SSL_accept(ssl);
    if (r == 1) break;
    const int e = SSL_get_error(ssl, r);
    const short ev = e == SSL_ERROR_WANT_READ ? POLLIN : e == SSL_ERROR_WANT_WRITE ? POLLOUT : 0;
    const std::int64_t left = hs_deadline - steady_ms();
    pollfd p{tcp, ev, 0};
    if (ev == 0 || left <= 0 || stopping_ || ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(left, 50))) < 0) {
      ++tls_failed_;
      log_wire("tls handshake-failed");
      ERR_clear_error();
      SSL_free(ssl);
      std::lock_guard<std::mutex> lk(conn_mu_);
      conn_fds_.erase(tcp);   // before the caller's close, so stop() never shuts a reused number
      return;
    }
  }
  ++tls_ok_;
  if (SSL_session_reused(ssl)) ++tls_resumed_;
  int sp[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
    SSL_free(ssl);
    std::lock_guard<std::mutex> lk(conn_mu_);
    conn_fds_.erase(tcp);
    return;
  }
  const int inner = sp[0], srv_fd = sp[1];
  net::no_sigpipe(inner);
  net::no_sigpipe(srv_fd);
  ::fcntl(inner, F_SETFL, ::fcntl(inner, F_GETFL, 0) | O_NONBLOCK);
  bool dropped = false;   // a drop that reached the TCP socket during the handshake
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    dropped = conn_fds_.count(tcp) && conn_fds_[tcp] == 4;
    conn_fds_.erase(tcp);
    conn_fds_[srv_fd] = 1;
  }
  if (dropped) ::shutdown(srv_fd, SHUT_RDWR);
  std::thread srv([this, srv_fd] {
    serve(srv_fd);
    {
      std::lock_guard<std::mutex> g(conn_mu_);
      conn_fds_.erase(srv_fd);
    }
    ::close(srv_fd);
  });

  std::string to_inner, to_tls;
  bool client_done = false;   // close_notify or EOF from the client: nothing more will come
  bool tls_dead = false;      // a reset or a TLS error: nothing more can go out either
  bool inner_eof = false;     // serve() closed its end: the reply is all in to_tls
  bool inner_wr_shut = false;
  char buf[16384];
  while (!stopping_) {
    while (!client_done) {
      ERR_clear_error();
      const int r = SSL_read(ssl, buf, sizeof buf);
      if (r > 0) {
        to_inner.append(buf, static_cast<std::size_t>(r));
        continue;
      }
      const int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
      client_done = true;
      if (e != SSL_ERROR_ZERO_RETURN) tls_dead = true;
      ERR_clear_error();
    }
    while (!to_inner.empty()) {
      const ssize_t n = ::send(inner, to_inner.data(), to_inner.size(), 0);
      if (n > 0) {
        to_inner.erase(0, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      to_inner.clear();   // serve() is gone
      break;
    }
    if (client_done && to_inner.empty() && !inner_wr_shut) {
      ::shutdown(inner, SHUT_WR);   // serve() sees the client's close
      inner_wr_shut = true;
    }
    while (!inner_eof && to_tls.size() < (1u << 20)) {
      const ssize_t n = ::recv(inner, buf, sizeof buf, 0);
      if (n > 0) {
        to_tls.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      inner_eof = true;
    }
    while (!to_tls.empty() && !tls_dead) {
      ERR_clear_error();
      const int r = SSL_write(ssl, to_tls.data(), static_cast<int>(to_tls.size()));
      if (r > 0) {
        to_tls.erase(0, static_cast<std::size_t>(r));
        continue;
      }
      const int e = SSL_get_error(ssl, r);
      if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) break;
      tls_dead = true;
      ERR_clear_error();
    }
    if (tls_dead) to_tls.clear();
    if (inner_eof && to_tls.empty()) break;   // the reply is out: close the TLS side
    if (tls_dead && client_done) break;
    pollfd p[2] = {{tcp, 0, 0}, {inner, 0, 0}};
    if (!client_done) p[0].events |= POLLIN;
    if (!to_tls.empty()) p[0].events |= POLLOUT;
    if (!inner_eof && to_tls.size() < (1u << 20)) p[1].events |= POLLIN;
    if (!to_inner.empty()) p[1].events |= POLLOUT;
    ::poll(p, 2, 50);
  }
  if (!tls_dead && cfg_.tls_close_notify) {
    ERR_clear_error();
    SSL_shutdown(ssl);   // close_notify; the peer's is not waited for
  }
  ERR_clear_error();
  ::shutdown(tcp, SHUT_RDWR);
  ::shutdown(inner, SHUT_RDWR);   // serve() ends if it has not
  srv.join();
  ::close(inner);
  SSL_free(ssl);
}

void FakeKalshi::serve_ws(int fd, const net::Request& req) {
  auto refuse = [&](int status, const std::string& code, const std::string& m) {
    const RestReply e = rest_error(status, code, m);
    net::write_all(fd, net::response_head(status, {{"Content-Type", "application/json"},
                                                   {"Content-Length", std::to_string(e.body.size())},
                                                   {"Connection", "close"}}) +
                           e.body);
    log_wire("http " + std::to_string(status) + " GET " + req.path + " (upgrade)");
  };
  if (req.path != kWsPath) return refuse(404, "not_found", "the socket is at /trade-api/ws/v2");
  const std::string key = req.header("sec-websocket-key");
  if (req.method != "GET" || key.empty() || req.header("sec-websocket-version") != "13" ||
      net::lower(req.header("connection")).find("upgrade") == std::string::npos)
    return refuse(400, "bad_request", "not a WebSocket handshake");
  std::string denied;
  {
    std::lock_guard<std::mutex> lk(mu_);
    denied = auth_refusal(req, now_locked());
  }
  // "Authentication is required even for public channels" (redesign_all kalshi K3)
  if (!denied.empty()) return refuse(401, "authentication_error", denied);
  if (!net::write_all(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " +
                              net::ws_accept_key(key) + "\r\n\r\n"))
    return;
  {
    std::lock_guard<std::mutex> lk(conn_mu_);
    conn_fds_[fd] = 2;  // a socket now: drop_http_connections leaves it alone
  }
  auto s = std::make_shared<Session>();
  s->fd = fd;
  s->authed = cfg_.auth != AuthMode::OFF || !req.header("kalshi-access-key").empty();
  {
    std::lock_guard<std::mutex> lk(mu_);
    s->id = next_session_++;
    sessions_[s->id] = s;
  }
  const std::string tag = "ws " + std::to_string(s->id);
  log_wire(tag + " open");
  s->writer = std::thread([this, s] { writer_loop(s); });
  net::WsReader rd;
  net::WsMessage msg;
  while (!stopping_) {
    const net::ReadResult r = net::ws_read(fd, rd, msg, true, 250);
    if (r == net::ReadResult::TIMEOUT) continue;
    if (r != net::ReadResult::OK) break;
    if (msg.opcode == net::TEXT) {
      std::lock_guard<std::mutex> lk(mu_);
      ws_command_locked(s, msg.payload);
    } else if (msg.opcode == net::PING) {
      enqueue(s, net::ws_frame(net::PONG, msg.payload), tag + " pong");
    } else if (msg.opcode == net::CLOSE) {
      enqueue(s, net::ws_frame(net::CLOSE, msg.payload.substr(0, 2)), tag + " close");
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.erase(s->id);
  }
  {
    std::lock_guard<std::mutex> lk(s->m);
    s->closing = true;
  }
  s->cv.notify_all();
  s->writer.join();
  log_wire(tag + " closed");
}

}  // namespace fakex
