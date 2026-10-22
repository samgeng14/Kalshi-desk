#pragma once
// A fake Kalshi for deskd's tests: the Trade API v2 and its WebSocket on one loopback port, with
// the same paths, message shapes and sequence rules as the venue, books driven by a recorded tape
// or a scripted scenario, and faults on demand (plan section 8).
//
//   REST  http://127.0.0.1:<port>/trade-api/v2/...       (the venue: https://external-api.kalshi.com)
//   WS    ws://127.0.0.1:<port>/trade-api/ws/v2           (wss://external-api-ws.kalshi.com)
//
// REST (docs.kalshi.com/api-reference, fetched 2026-10-06):
//   POST   /portfolio/events/orders                    create (V2): 201 {order_id, client_order_id,
//                                                      fill_count, remaining_count,
//                                                      average_fill_price, average_fee_paid, ts_ms}
//   DELETE /portfolio/events/orders/{id}?market_ticker&exchange_index&subaccount
//                                                      200 {order_id, client_order_id, reduced_by, ts_ms}
//   DELETE /portfolio/events/orders/batched            {orders:[{order_id, market_ticker?,
//                                                      exchange_index?, subaccount?}]} -> 200 {orders:[
//                                                      {order_id, reduced_by, ts_ms, client_order_id, error}]}
//   DELETE /portfolio/events/orders?subaccount         cancel-all: 204, no body
//   GET    /portfolio/orders, /portfolio/orders/{id}, /portfolio/fills, /portfolio/positions
//                                                      newest first (positions by ticker), limit 1-1000
//                                                      (default 100), cursor paging; "" ends it
//   GET    /portfolio/balance?subaccount&exchange_index {balance (cents), balance_dollars,
//                                                      portfolio_value, updated_ts, balance_breakdown}
//   POST   /portfolio/order_groups/create              201 {order_group_id, subaccount, exchange_index}
//   GET    /portfolio/order_groups, /portfolio/order_groups/{id}
//   PUT    /portfolio/order_groups/{id}/trigger|reset|limit, DELETE /portfolio/order_groups/{id}
//                                                      ?subaccount&exchange_index (default 0): 200 {}
//   POST   /portfolio/intra_exchange_instance_transfer moves cash between shards (src/client.cpp's body)
//   GET    /markets (paged by ticker), /markets/{ticker}, /markets/{ticker}/orderbook,
//          /events/{event_ticker}?with_nested_markets, /exchange/status        (no auth)
// Errors are {"error":{"code","message","details"?}} (what the venue sent the old desk:
// src/executor.cpp kalshi_error_body; the docs' ErrorResponse is the inner object), except a 429,
// whose body is {"error": "too many requests"} (getting_started/rate_limits.md).
//
// WebSocket (websockets/websocket-connection.md and the frames recorded 2026-09-23/24):
//   subscribe {id, cmd, params{channels, market_ticker | market_tickers, send_initial_snapshot}}
//     -> {"id","type":"subscribed","msg":{"channel","sid"}} per channel; no seq. sids count from 1
//     on each connection, in subscription order (ticker 1, orderbook_delta 2 on all 17 recorded
//     connections). No markets means every market (the old recorder's 77.6% flood).
//   update_subscription {sid | sids:[one], action add_markets|delete_markets|get_snapshot, markets}
//     -> {"id","sid","seq","type":"ok","msg":{"market_tickers":[the whole list]}}: the reply
//     takes a number in the sid's sequence (the 5 "missing" numbers of 2026-09-24 were replies),
//     then a snapshot per added market.
//   unsubscribe {sids} -> {"id","sid","seq","type":"unsubscribed"}; list_subscriptions -> ok + list.
//   errors {"id","type":"error","msg":{"code","msg"}}: 1 unparseable, 2 params, 3 channels,
//     4 sids, 5 command, 7 unknown sid, 8 channel, 9 auth, 11 parameter, 12 exactly one sid,
//     13 action, 14 market ticker, 15 action required, and the limits (FakeConfig ws_*):
//     25 "Subscription buffer overflow" (no id, the sid; the subscription is gone), 26
//     "Subscription market limit exceeded" and 27 "Too many requests" (the command refused).
//     The venue documents the codes, not its numbers: the limits here are the fake's own, and
//     deskd must survive each code whatever the number (a slow md-k, a TOP list of ~3,300).
//   Channels and types: orderbook_delta (orderbook_snapshot, orderbook_delta), ticker, trade, fill,
//     user_orders ("user_order"), market_positions ("market_position"), order_group_updates.
//     seq, per sid and across every market on it, on orderbook_* , trade, order_group_updates and
//     the ok/unsubscribed replies; none on ticker, fill, user_order, market_position. Every
//     message carries sending_ts_ms (changelog, 2026-10-01).
//
// Auth: KALSHI-ACCESS-KEY, -TIMESTAMP (ms) and -SIGNATURE on /portfolio and the socket's
// handshake; AuthMode::VERIFY checks the RSA-PSS SHA-256 signature (salt = digest length) of
// timestamp + method + path, the path without its query (src/auth.cpp, src/client.cpp).
//
// Rate limits (getting_started/rate_limits.md, public /account/endpoint_costs 2026-10-05): token
// buckets; Basic reads 200/s holding 3 s, writes 100/s holding 1 s; Advanced 300/300 holding 3 s.
// Create 10 tokens, cancel 2, batch cancel 2 an order, cancel-all 2, GET one order 2, else 10. A
// single order write naming a nonzero shard draws on that shard's bucket only; auto-routed ones on
// the unscoped bucket and every nonzero shard's; batches and shard 0 on the unscoped one
// (exchange_sharding.md: "Auto-routed single REST order writes are billed to the unscoped Write
// bucket and every nonzero shard's Write bucket"). So an auto-routed shard-0 order spends shard 2's
// budget too: deskd names the shard on every write.
//
// TLS (week-0 review issue 10(a), added 2026-10-07): with tls_cert_file and tls_key_file set the
// listener speaks TLS 1.2+ (https:// and wss://), so a client's real TLS path runs against the
// fake: the handshake and its verification (tests/fake_exchange/tls_pki.h makes a CA and a leaf at
// run time), TLS 1.3's session tickets arriving after the handshake, resumption, close_notify, a
// bare FIN without one (tls_close_notify false: a load balancer that just drops), and the idle
// close of a kept-open connection. Each TLS connection runs a pump thread between the TLS socket
// and a local socketpair whose other end is served exactly like a plain connection, so every
// fault, the wire log and the idle close behave the same in both modes. One difference: the order
// between two connections' bytes (EVENTS_FIRST, REPLY_FIRST) holds for what the fake hands its
// pumps; the two pumps then forward in parallel, microseconds apart.
//
// Threads: one per connection (two with TLS), one writer per socket, a timer. The engine sits
// behind one mutex. Nothing here talks to anything but 127.0.0.1.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine.h"
#include "net.h"
#include "tape.h"

namespace fakex {

// When an order's private messages (fill, user_orders, market_positions, order_group_updates)
// leave relative to its REST reply. The venue sends them from a different host, so either can
// arrive first (plan section 3: "private events that beat the 201 wait up to 5 s"). Book, ticker
// and trade messages always go at once: holding them back would let a snapshot taken meanwhile
// count the same change twice.
//   NATURAL       queued for the socket at once; the socket's writer and the reply race
//   REPLY_FIRST   queued only after the reply's last byte is written
//   EVENTS_FIRST  the reply waits until they are written
// Whatever the mode, one order's (and one market's position) private messages leave in the
// engine's order: a later request's messages for an order wait behind that order's messages
// still held for an earlier reply. Until 2026-10-07 a cancel sent on another connection once the
// create's 201 arrived could publish its "canceled" before the create's held "resting" (review):
// an ordering the venue is not known to produce, and a flaky one. The fake's own choice: the
// venue documents no ordering for private messages, so deskd still tolerates any (it keeps the
// largest fill count and never revives a terminal order).
enum class PrivateOrder { NATURAL, REPLY_FIRST, EVENTS_FIRST };

struct HttpFault {
  enum class Kind {
    RATE_LIMIT,           // 429 {"error": "too many requests"}; nothing processed
    DROP_BEFORE_REPLY,    // the request read, nothing processed, the connection closed
    DROP_AFTER_ACCEPT,    // processed (an order exists), then closed with no reply at all
    STATUS_AFTER_ACCEPT,  // processed, then `status` (5xx) with an error body
    CUT_BODY,             // processed; the head promises the whole body, cut_bytes arrive, the close
    CHUNKED,              // processed; the reply in chunks of chunk_bytes
    STALL_BEFORE,         // stall_ms, then processed and answered
    STALL_AFTER,          // processed, stall_ms, then answered
    CLOSE_AFTER_REPLY,    // answered with Connection: close, then closed
    FILL_BEFORE_REPLY,    // processed; its socket messages are written before the reply's first byte
    HIT_AFTER_REST,       // create: once it rests the street takes `qty` of it, and that fill and
                          // its user_order are on the socket before the 201 (whose fill_count
                          // says 0)
    FILL_DURING_CANCEL,   // cancel: the street takes `qty` first; the reply's reduced_by is the
                          // rest, and the fill and its user_order follow the reply
                          // (private_order EVENTS_FIRST puts them first instead)
  };
  Kind kind = Kind::STATUS_AFTER_ACCEPT;
  std::string method = "POST";
  std::string path = "/portfolio/events/orders";  // a prefix of the path after /trade-api/v2
  int status = 500;
  int stall_ms = 2000;
  std::size_t chunk_bytes = 16;
  std::size_t cut_bytes = 0;  // 0: half the body
  Qty qty = 0;
  int times = 1;
  // This request's private messages against its reply. NATURAL keeps the server's setting, except
  // that FILL_BEFORE_REPLY and HIT_AFTER_REST mean EVENTS_FIRST and FILL_DURING_CANCEL REPLY_FIRST.
  PrivateOrder private_order = PrivateOrder::NATURAL;
};

enum class AuthMode { OFF, PRESENCE, VERIFY };

struct RateTier {
  double read_per_s, write_per_s, read_burst_s, write_burst_s;
};
inline RateTier basic_tier() { return {200, 100, 3, 1}; }
inline RateTier advanced_tier() { return {300, 300, 3, 3}; }

struct FakeConfig {
  EngineConfig engine;
  int port = 0;                      // 0: any free port
  AuthMode auth = AuthMode::PRESENCE;
  std::string public_key_pem;        // AuthMode::VERIFY
  std::int64_t auth_max_skew_ms = 0;  // 0: any timestamp
  bool rate_limits = true;
  RateTier tier = basic_tier();
  // Kalshi's load balancer closed a kept-open connection idle 65 s and kept one idle 30 s
  // (v3_research fills F10, 2026-10-06). An idle close is logged "http idle-close".
  int idle_close_ms = 60000;
  // TLS: both set, the listener speaks TLS with this chain (leaf first) and key, PEM files.
  std::string tls_cert_file, tls_key_file;
  bool tls_close_notify = true;      // false: every close is a bare TCP FIN, no close_notify
  int tls_handshake_ms = 5000;       // a client that does not finish its handshake is dropped
  int ws_ping_ms = 0;                // the server pings every so often; 0: never
  // WebSocket limits (errors 25-27 above); 0 turns one off. 5,000 unwritten messages on one
  // subscription is far past what a reader that keeps up leaves queued (a 2,600-market snapshot
  // burst is 2,600), so only a reader that stopped reading meets it.
  std::size_t ws_sub_buffer = 5000;
  std::size_t ws_sub_market_limit = 0;   // markets on one subscription
  int ws_cmds_per_s = 0;                 // commands a second on one connection
  PrivateOrder private_order = PrivateOrder::NATURAL;
  bool manual_clock = false;         // time moves only by advance_ms
  std::int64_t start_ms = 0;         // the manual clock's first reading; 0: the wall clock now
  int tick_ms = 5;                   // the venue's timers (expiry, close) on the wall clock
};

class FakeKalshi {
 public:
  explicit FakeKalshi(FakeConfig cfg = {});
  ~FakeKalshi();
  FakeKalshi(const FakeKalshi&) = delete;
  FakeKalshi& operator=(const FakeKalshi&) = delete;

  bool start(std::string* err);
  void stop();
  int port() const { return port_; }
  std::string rest_base() const;  // http(s)://127.0.0.1:<port>/trade-api/v2
  std::string ws_url() const;     // ws(s)://127.0.0.1:<port>/trade-api/ws/v2
  bool tls() const { return tls_ctx_ != nullptr; }

  // ---- time
  std::int64_t now_ms() const;
  // Manual clock: time moves by d, the tape plays what is due, expiries and closes run.
  void advance_ms(std::int64_t d);

  // ---- the venue, under its lock: f(engine, now). Whatever the engine pushes goes out as the
  // venue would send it. Return values, not references into the engine.
  template <class F>
  auto with_engine(F&& f) -> decltype(f(std::declval<Engine&>(), std::int64_t{})) {
    std::unique_lock<std::mutex> lk(mu_);
    const std::int64_t now = now_locked();
    if constexpr (std::is_void_v<decltype(f(engine_, now))>) {
      f(engine_, now);
      publish_locked(engine_.drain_events());
    } else {
      auto r = f(engine_, now);
      publish_locked(engine_.drain_events());
      return r;
    }
  }

  // ---- Kalshi's pyth_value channel: one reference value, to every pyth_value subscription.
  // The shape is the old desk's reading of the documented channel (src/reference_feed.cpp):
  // {type: pyth_value, sid, msg: {underlying_ticker, value_usd, source_ts_ms}}, no seq.
  void pyth(const std::string& underlying, double value_usd, std::int64_t source_ts_ms);

  // ---- a tape or scenario, from now at `speed` (2.0 plays twice as fast)
  void play(Tape tape, double speed = 1.0);
  bool tape_done() const;
  std::size_t tape_played() const;

  // ---- faults
  void add_fault(HttpFault f);
  void clear_faults();
  std::size_t faults_pending() const;
  void set_private_order(PrivateOrder p);
  void set_rate_limits(bool on, RateTier tier = basic_tier());
  void drop_ws_connections();                                  // TCP closed, no close frame
  // Every kept-open REST connection closed under its client (a load balancer's idle close, a
  // network drop): the next request on it gets nothing back.
  void drop_http_connections();
  void drop_ws_messages(const std::string& channel, int n);    // lost; their seq numbers spent
  void duplicate_ws_messages(const std::string& channel, int n);  // each sent twice
  void fragment_ws(std::size_t frame_bytes);                   // text messages in frames this size
  void stall_ws(int ms);                                       // writers wait, then send it all

  // ---- what happened on the wire, in the order it was written: "http 201 POST
  // /portfolio/events/orders", "http cut ...", "http drop ...", "ws <conn> fill <trade_id>",
  // "ws <conn> user_order <order_id>", "ws <conn> orderbook_delta <ticker> <seq>" ...
  std::vector<std::string> wire_log() const;
  void clear_wire_log();
  std::size_t ws_connections() const;
  std::uint64_t http_requests() const { return requests_.load(); }
  std::uint64_t connections_accepted() const { return accepted_.load(); }
  std::uint64_t idle_closes() const { return idle_closes_.load(); }
  std::uint64_t tls_handshakes() const { return tls_ok_.load(); }        // completed
  std::uint64_t tls_handshake_failures() const { return tls_failed_.load(); }
  std::uint64_t tls_resumed() const { return tls_resumed_.load(); }
  // Waits until every queued socket message is written (or `ms` passes).
  bool wait_ws_flushed(int ms = 2000);

 private:
  struct Sub {
    std::string channel;
    bool all = false;
    std::vector<std::string> markets;  // in the order added
    std::int64_t seq = 0;
    bool overflowed = false;           // error 25 sent; removed at the end of the fan-out
  };
  struct OutItem {
    std::string bytes, label;  // the frame, its wire-log label
    int sid = 0;               // the subscription it belongs to, 0: none
  };
  struct Session {
    int id = 0, fd = -1;
    bool authed = false;
    std::mutex m;
    std::condition_variable cv;
    std::deque<OutItem> out;
    std::map<int, std::size_t> pending;  // unwritten messages by subscription (error 25)
    bool closing = false, dead = false;
    std::uint64_t queued = 0, written = 0;
    std::thread writer;
    // under FakeKalshi::mu_
    std::map<int, Sub> subs;
    int next_sid = 1;
    std::deque<std::int64_t> cmd_ms;  // command times in the last second (error 27)
  };
  // Private messages held for a reply (REPLY_FIRST), or waiting behind such a batch for the same
  // order: published in this order, each once it is ready and no earlier one shares a key.
  struct Held {
    std::uint64_t id = 0;
    std::set<std::string> keys;  // "o:<order_id>", "g:<group_id>", "p:<ticker>"
    std::vector<Event> events;
    bool ready = false;
  };
  struct Bucket {
    double tokens = 0, rate = 0, cap = 0;
    std::int64_t at_ms = 0;
    void refill(std::int64_t now);
  };
  struct Reply {
    int status = 200;
    std::string body;
  };
  using Targets = std::vector<std::pair<std::shared_ptr<Session>, std::uint64_t>>;

  std::int64_t now_locked() const;
  void accept_loop();
  void tick_loop();
  void serve(int fd);
  void serve_tls(int tcp_fd, std::shared_ptr<std::atomic<bool>> done);   // the pump; serve() on a socketpair
  void serve_ws(int fd, const net::Request& req);
  void writer_loop(const std::shared_ptr<Session>& s);
  void log_wire(const std::string& line);
  void enqueue(const std::shared_ptr<Session>& s, std::string bytes, std::string label, int sid = 0);
  std::string frame_text(const std::string& payload) const;

  // Publishes events, except that a private message for an order (or position) with messages
  // still held goes behind them (Held); the returned targets cover what went out now.
  Targets publish_locked(const std::vector<Event>& events);
  Targets publish_now_locked(const std::vector<Event>& events);
  void pyth_locked(const std::string& underlying, double value_usd, std::int64_t source_ts_ms);
  std::uint64_t hold_locked(std::vector<Event> events, bool ready);  // a new Held batch; its id
  void release_locked(std::uint64_t id);   // that batch is ready: publish what can go
  void sweep_locked(const std::shared_ptr<Session>& s);   // drops overflowed subscriptions
  void deliver_locked(const std::shared_ptr<Session>& s, int sid, Sub& sub, const std::string& type,
                      const json& msg, bool seq, std::int64_t now, Targets* targets);
  void send_snapshot_locked(const std::shared_ptr<Session>& s, int sid, Sub& sub, const std::string& ticker,
                            std::int64_t now, Targets* targets);
  void ws_command_locked(const std::shared_ptr<Session>& s, const std::string& text);
  void ws_command_inner_locked(const std::shared_ptr<Session>& s, const std::string& text);
  void pump_locked(std::int64_t now);
  bool take_fault_locked(const net::Request& req, HttpFault* f);

  std::string auth_refusal(const net::Request& req, std::int64_t now) const;
  bool rate_ok_locked(const net::Request& req, const std::string& path, const json& body, std::int64_t now);
  Reply route_locked(const net::Request& req, const std::string& path, const json& body, std::int64_t now,
                     std::string* order_id_out);

  FakeConfig cfg_;
  mutable std::mutex mu_;
  Engine engine_;
  std::int64_t manual_now_ = 0;
  // The engine's time never goes back: on the wall clock the timer plays a tape event a few ms
  // after it was due, and a REST call may have stamped a later time meanwhile.
  mutable std::int64_t clock_ms_ = 0;
  void* pubkey_ = nullptr;  // EVP_PKEY*

  int listen_fd_ = -1, port_ = 0;
  void* tls_ctx_ = nullptr;  // SSL_CTX*, when TLS
  std::string tls_err_;
  std::atomic<bool> running_{false}, stopping_{false};
  std::thread acceptor_, ticker_;
  struct Conn {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::mutex conn_mu_;
  std::vector<Conn> conns_;
  std::map<int, int> conn_fds_;  // fd -> 1 (REST) or 2 (upgraded) while its thread owns it

  std::map<int, std::shared_ptr<Session>> sessions_;
  int next_session_ = 1;
  std::deque<Held> held_;
  std::uint64_t next_held_ = 1;
  std::deque<HttpFault> faults_;
  PrivateOrder private_order_;
  std::map<std::string, int> drop_left_, dup_left_;
  std::atomic<std::size_t> fragment_bytes_{0};
  std::atomic<std::int64_t> stall_until_ms_{0};  // steady clock
  Bucket read_, write_;
  std::map<int, Bucket> write_shard_;

  Tape tape_;
  std::size_t tape_pos_ = 0;
  double tape_speed_ = 1.0;
  std::int64_t tape_base_ms_ = 0;
  std::int64_t last_ping_ms_ = 0;

  mutable std::mutex log_mu_;
  std::deque<std::string> wire_;
  std::atomic<std::uint64_t> requests_{0}, accepted_{0}, idle_closes_{0};
  std::atomic<std::uint64_t> tls_ok_{0}, tls_failed_{0}, tls_resumed_{0};
};

}  // namespace fakex
