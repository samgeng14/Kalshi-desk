// kalshi-demo-probe: deskd's own net/ and signer against Kalshi's DEMO exchange (plan section 9,
// week 1; stage 1 item 6). It proves, against the real demo host, what the tests prove against
// the fake: the TLS factory (chain and host name verified, SNI, the guard on every connect), the
// spare pool, the strict HTTP/1.1 parser, the Ed25519 or RSA-PSS signer, and the wire builders
// and parsers; and it measures what the click budget rests on (plan section 2: a handshake of
// 35-95 ms that a click must never pay, 25-35 ms p50 from write to reply).
//
//   kalshi-demo-probe [--key FILE] [--base URL] [--handshakes N] [--loopback-ca FILE]
//                     [--order-check [--order-ticker T]] [--dump DIR]
//
//   --key FILE        the key file (default ~/.kalshi/demo.json): {env, key_id, private_key_path,
//                     key_type}, or a deskd config's {env, key_id, pem_path, key_type}. The key is
//                     read from the path it names straight into OpenSSL; nothing of it, of the
//                     key id or of a signature is ever printed. Each request's method and path are.
//   --base URL        default https://demo-api.kalshi.co/trade-api/v2. The guard refuses a
//                     production host whatever is asked (this program is never live), and a key
//                     file whose env is not demo is refused before anything connects.
//   --handshakes N    connections opened for the handshake figures, N with session resumption
//                     and N without (default 10)
//   --loopback-ca F   a CA trusted for loopback endpoints only: the tests point the probe at the
//                     fake exchange over TLS (tests/deskd/test_wire.cpp)
//   --order-check     one post-only bid for 1 contract at $0.01 with a 15 s exchange-side expiry
//                     (far below any touch: it rests, costs $0.01 of collateral, and dies on its
//                     own if the cancel is lost), read back, cancelled, read back again, and the
//                     resting list checked empty of it. Play money on demo; production is refused.
//   --order-ticker T  the market for it (default: the most liquid 1c-grid market on the page read)
//   --dump DIR        the raw reply bodies, for tests/deskd/fixtures (look them over for account
//                     ids before committing one)
//
// Exit 0 when every request got a whole 2xx reply (a GET order's 404 right after its 201 is not
// counted: demo answered 404 for about 0.7 s on 2026-10-07) and the order check, if asked for,
// left nothing resting; 1 otherwise; 2 on bad arguments, a key that does not load, or a base the
// guard refuses.
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "deskd/app/config.h"
#include "deskd/app/log.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/conn.h"
#include "deskd/net/guard.h"
#include "deskd/net/http.h"
#include "deskd/net/pool.h"

using namespace deskd;
using namespace deskd::kalshi;
using json = nlohmann::json;

namespace {

struct Args {
  std::string key = "~/.kalshi/demo.json";
  std::string base = "https://demo-api.kalshi.co/trade-api/v2";
  int handshakes = 10;
  std::string loopback_ca;
  bool order_check = false;
  std::string order_ticker;
  std::string dump;
};

int usage() {
  std::fprintf(stderr,
               "usage: kalshi-demo-probe [--key FILE] [--base URL] [--handshakes N] [--loopback-ca FILE]\n"
               "                         [--order-check [--order-ticker T]] [--dump DIR]\n");
  return 2;
}

double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * (v.size() - 1) + 0.5));
  return v[i];
}

void print_stats(const char* indent, const char* label, const std::vector<double>& v) {
  if (v.empty()) {
    std::printf("%s%-18s n 0\n", indent, label);
    return;
  }
  std::printf("%s%-18s n %zu  p50 %.1f  p90 %.1f  min %.1f  max %.1f ms\n", indent, label, v.size(), pct(v, 0.5),
              pct(v, 0.9), *std::min_element(v.begin(), v.end()), *std::max_element(v.begin(), v.end()));
}

std::string micros_str(Micros m) {
  char b[40];
  const long long a = m < 0 ? -static_cast<long long>(m) : static_cast<long long>(m);
  std::snprintf(b, sizeof b, "%s%lld.%06lld", m < 0 ? "-" : "", a / 1000000, a % 1000000);
  return b;
}

std::string hex16() {
  std::random_device rd;
  char b[20];
  std::snprintf(b, sizeof b, "%08x%08x", rd(), rd());
  return b;
}

std::int64_t wall_s() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The key file: ~/.kalshi/demo.json's shape or a deskd config's. Nothing of the key or its id
// is kept anywhere but in the signer; the description says the type and the id's length.
bool load_key(const Args& a, std::unique_ptr<KalshiSigner>* out, std::string* desc, std::string* err) {
  const std::string path = expand_home(a.key);
  std::ifstream in(path);
  if (!in) {
    *err = "cannot open the key file " + path;
    return false;
  }
  json j;
  try {
    j = json::parse(in, nullptr, true, true);
  } catch (const json::exception&) {
    *err = path + ": not JSON";
    return false;
  }
  if (!j.is_object()) {
    *err = path + ": not a JSON object";
    return false;
  }
  auto s = [&](const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  const std::string env = s("env"), key_id = s("key_id");
  const std::string key_type = s("key_type").empty() ? std::string("rsa") : s("key_type");
  std::string pem = s("private_key_path");
  if (pem.empty()) pem = s("pem_path");
  if (env != "demo") {
    *err = path + ": env is \"" + env + "\"; this probe only ever touches the demo exchange";
    return false;
  }
  if (key_id.empty() || pem.empty()) {
    *err = path + ": needs key_id and private_key_path (or pem_path)";
    return false;
  }
  *out = KalshiSigner::load(key_id, expand_home(pem), key_type, err);
  if (!*out) return false;
  *desc = std::string(key_type_name((*out)->type())) +
          ((*out)->type() == KeyType::RSA ? " " + std::to_string((*out)->bits()) + " bits" : "") + ", id of " +
          std::to_string(key_id.size()) + " characters, from " + path;
  return true;
}

struct DemoProbe {   // not Probe: bus/msg.h has a message of that name
  Args a;
  SendGuard guard{false, nullptr};   // never live: a production host gets no socket
  net::ConnFactory factory;
  net::Endpoint base;
  std::unique_ptr<KalshiSigner> signer;
  std::unique_ptr<net::SparePool> pool;
  std::vector<double> ttfb, done, take;
  bool all_ok = true;

  DemoProbe(Args args, net::Endpoint ep, std::unique_ptr<KalshiSigner> s)
      : a(std::move(args)), factory(guard, options(a, true)), base(std::move(ep)), signer(std::move(s)) {}

  static net::FactoryOptions options(const Args& a, bool resume) {
    net::FactoryOptions o;
    o.loopback_ca_file = a.loopback_ca;
    o.resume_sessions = resume;
    return o;
  }

  void dump(const char* name, const std::string& body) const {
    if (a.dump.empty()) return;
    std::ofstream(a.dump + "/kalshi_demo_" + name + ".json") << body;
  }

  // One request on one pre-opened connection, closed afterwards (one write per connection). The
  // line shows the status and the write-to-reply time, or why there was no reply. Only the
  // method and the path are ever shown: no query, no header.
  net::Exchange send(const WireRequest& r, const std::string& label, bool show = true) {
    const std::int64_t t0 = net::mono_us();
    bool miss = false;
    std::string why;
    auto c = pool->take(5000, &miss, &why);
    const double take_ms = (net::mono_us() - t0) / 1000.0;
    if (!c) {
      std::printf("  %-34s no connection: %s\n", label.c_str(), why.c_str());
      all_ok = false;
      return net::Exchange{};
    }
    std::vector<std::pair<std::string, std::string>> h;
    if (r.auth) h = signer->headers(now_ms(), r.method, r.path);
    net::Exchange x = net::exchange(*c, request_bytes(r, base.host_header(), h, "kalshi-demo-probe/1"),
                                    net::mono_us() + 5LL * 1000 * 1000);
    c->close(true);
    if (x.fate == net::Exchange::Fate::REPLIED) {
      ttfb.push_back((x.t_first_byte_us - x.t_written_us) / 1000.0);
      done.push_back(x.write_to_done_ms());
      take.push_back(take_ms);
      if (show)
        std::printf("  %-34s %3d %7.1f ms write->reply  %s %s%s\n", label.c_str(), x.res.status, x.write_to_done_ms(),
                    r.method.c_str(), r.path.c_str(), miss ? "  (pool miss)" : "");
    } else {
      std::printf("  %-34s %s: %s  (%s %s)\n", label.c_str(), net::fate_name(x.fate), x.why.c_str(), r.method.c_str(),
                  r.path.c_str());
      all_ok = false;
    }
    return x;
  }
  bool ok2xx(const net::Exchange& x) const {
    return x.fate == net::Exchange::Fate::REPLIED && x.res.status >= 200 && x.res.status < 300;
  }
  void expect2xx(const net::Exchange& x) {
    if (!ok2xx(x)) all_ok = false;
  }

  void handshakes() {
    const int n = a.handshakes;
    std::printf("\nhandshakes (TCP connect + TLS, %d with session resumption, %d without):\n", n, n);
    for (bool resume : {true, false}) {
      net::ConnFactory f(guard, options(a, resume));
      std::vector<double> tcp, tls, total;
      std::string ver, cipher, peer, why;
      int resumed = 0, failed = 0;
      for (int i = 0; i < n; ++i) {
        auto c = f.open(base, 5000, &why);
        if (!c) {
          ++failed;
          std::printf("  open failed: %s\n", why.c_str());
          continue;
        }
        tcp.push_back(c->connect_ms());
        tls.push_back(c->handshake_ms());
        total.push_back(c->connect_ms() + c->handshake_ms());
        ver = c->tls_version();
        cipher = c->cipher();
        peer = c->peer();
        if (c->resumed()) ++resumed;
        // TLS 1.3's session ticket arrives after the handshake: take it in before the next open
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        c->usable();
        c->close(true);
      }
      if (failed) all_ok = false;
      std::printf("  resumption %s: %s %s, peer %s, %d resumed%s\n", resume ? "on " : "off", ver.c_str(), cipher.c_str(),
                  peer.c_str(), resumed, failed ? (", " + std::to_string(failed) + " failed").c_str() : "");
      print_stats("    ", "TCP connect", tcp);
      print_stats("    ", "TLS handshake", tls);
      print_stats("    ", "total", total);
    }
  }

  // The guard in the real binary: a production host gets no socket, not even a DNS lookup.
  void production_check() {
    std::string why;
    const std::uint64_t before = factory.stats().sockets;
    auto prod = net::Endpoint::parse("https://api.elections.kalshi.com/trade-api/v2", &why);
    std::unique_ptr<net::Conn> c;
    if (prod) c = factory.open(*prod, 1000, &why);
    const std::uint64_t after = factory.stats().sockets;
    if (c) {
      std::printf("production check: a connection to api.elections.kalshi.com OPENED: the guard failed\n");
      c->close(true);
      all_ok = false;
    } else {
      std::printf("production check: %s (sockets opened: %llu)\n", why.c_str(),
                  static_cast<unsigned long long>(after - before));
      if (after != before) all_ok = false;
    }
  }

  std::vector<MarketRow> reads() {
    std::printf("\nrequests, one per pre-opened connection:\n");
    std::string why;
    net::Exchange x = send(build_get_exchange_status(), "exchange status");
    expect2xx(x);
    if (ok2xx(x)) {
      dump("exchange_status", x.res.body);
      auto st = parse_exchange_status(x.res.body, &why);
      if (st) {
        std::printf("    exchange_active %s, trading_active %s, %zu shards listed\n", st->exchange_active ? "true" : "false",
                    st->trading_active ? "true" : "false", st->shards.size());
      } else {
        std::printf("    cannot read it: %s\n", why.c_str());
        all_ok = false;
      }
    }

    x = send(build_get_balance(), "balance (signed)");
    expect2xx(x);
    if (ok2xx(x)) {
      dump("balance", x.res.body);
      auto b = parse_balance(x.res.body, &why);
      if (b) {
        std::printf("    BALANCE available $%s, portfolio value $%s", money_str(b->available).c_str(),
                    b->portfolio_value ? money_str(*b->portfolio_value).c_str() : "-");
        for (const auto& [shard, m] : b->by_shard) std::printf(", shard %d $%s", shard, money_str(m).c_str());
        std::printf(" (updated_ts %lld)\n", static_cast<long long>(b->updated_ts));
      } else {
        std::printf("    cannot read it: %s\n", why.c_str());
        all_ok = false;
      }
    } else if (x.fate == net::Exchange::Fate::REPLIED) {
      const ApiError e = parse_error(x.res.status, x.res.body);
      std::printf("    %d %s %s %s\n", e.http, e.code.c_str(), e.message.c_str(), e.details.c_str());
    }

    ListQuery q;
    q.status = "resting";
    q.limit = 200;
    x = send(build_get_orders(q), "resting orders (signed)");
    expect2xx(x);
    if (ok2xx(x)) {
      auto page = parse_orders(x.res.body, &why);
      if (page) {
        std::printf("    resting orders: %zu%s\n", page->rows.size(), page->cursor.empty() ? "" : " (and more pages)");
        for (std::size_t i = 0; i < page->rows.size() && i < 5; ++i)
          std::printf("      %s %s %s @ %s, %s left, cid %s\n", page->rows[i].ticker.c_str(),
                      book_side_name(page->rows[i].side), count_wire(page->rows[i].initial).c_str(),
                      price_wire(page->rows[i].yes_price).c_str(), count_wire(page->rows[i].remaining).c_str(),
                      page->rows[i].client_order_id.empty() ? "(none: placed on the website)" : page->rows[i].client_order_id.c_str());
      } else {
        std::printf("    cannot read it: %s\n", why.c_str());
        all_ok = false;
      }
    }

    std::vector<MarketRow> rows;
    MarketsQuery mq;
    mq.limit = 100;
    mq.status = "open";
    mq.mve_filter = "exclude";
    x = send(build_get_markets(mq), "markets page");
    expect2xx(x);
    if (ok2xx(x)) {
      dump("markets", x.res.body);
      auto page = parse_markets(x.res.body, &why);
      if (page) {
        rows = page->rows;
        int with_bid = 0;
        for (const auto& m : rows)
          if (m.yes_bid && *m.yes_bid > 0) ++with_bid;
        std::printf("    %zu markets (%d with a YES bid), %s, %zu bytes\n", rows.size(), with_bid,
                    page->cursor.empty() ? "no cursor" : "a cursor", x.res.body.size());
        for (std::size_t i = 0; i < rows.size() && i < 3; ++i)
          std::printf("      %s shard %d bid %s ask %s\n", rows[i].ticker.c_str(), rows[i].exchange_index.value_or(-1),
                      rows[i].yes_bid ? price_wire(*rows[i].yes_bid).c_str() : "-",
                      rows[i].yes_ask ? price_wire(*rows[i].yes_ask).c_str() : "-");
      } else {
        std::printf("    cannot read it: %s\n", why.c_str());
        all_ok = false;
      }
    } else if (x.fate == net::Exchange::Fate::REPLIED) {
      const ApiError e = parse_error(x.res.status, x.res.body);
      std::printf("    %d %s %s %s\n", e.http, e.code.c_str(), e.message.c_str(), e.details.c_str());
    }

    // a latency sample on the two cheapest routes, one unsigned and one signed
    for (int i = 0; i < 5; ++i) {
      expect2xx(send(build_get_exchange_status(), "exchange status"));
      expect2xx(send(build_get_balance(), "balance"));
    }
    return rows;
  }

  // Which market takes the test order: open, a 1c grid at $0.01, an ask above $0.01 (so a
  // post-only bid there never crosses), not closing within two minutes; the deepest bid first.
  std::optional<MarketRow> choose(const std::vector<MarketRow>& rows) {
    std::string why;
    if (!a.order_ticker.empty()) {
      net::Exchange x = send(build_get_market(a.order_ticker), "GET market");
      expect2xx(x);
      if (!ok2xx(x)) return std::nullopt;
      auto m = parse_market(x.res.body, &why);
      if (!m) std::printf("    cannot read it: %s\n", why.c_str());
      return m;
    }
    const std::int64_t now = now_ms();
    std::optional<MarketRow> best;
    for (const auto& m : rows) {
      if (m.status != "active" && m.status != "open") continue;
      if (!m.exchange_index || !m.yes_ask || *m.yes_ask <= 100) continue;
      if (m.close_ms && *m.close_ms < now + 120 * 1000) continue;
      bool cent_grid = false;
      for (const auto& r : m.price_ranges)
        if (r.step == 100 && r.start <= 100 && r.end >= 100) cent_grid = true;
      if (!cent_grid) continue;
      if (!best || m.yes_bid.value_or(0) > best->yes_bid.value_or(0)) best = m;
    }
    return best;
  }

  // Reads the order until pred(row) holds or `ms` pass; the first line and the last are shown.
  std::optional<OrderRow> poll_order(const std::string& order_id, int ms, const std::function<bool(const OrderRow&)>& pred,
                                     int* other_replies, std::string* last_body) {
    std::string why;
    const std::int64_t until = net::mono_us() + static_cast<std::int64_t>(ms) * 1000;
    *other_replies = 0;
    last_body->clear();
    std::optional<OrderRow> got;
    for (int i = 0;; ++i) {
      net::Exchange x = send(build_get_order(order_id), "GET order", i == 0);
      std::optional<OrderRow> row = ok2xx(x) ? parse_order(x.res.body, &why) : std::nullopt;
      if (row && pred(*row)) {
        if (i > 0)
          std::printf("  %-34s %3d %7.1f ms write->reply  GET %s\n", "GET order", x.res.status, x.write_to_done_ms(),
                      build_get_order(order_id).path.c_str());
        *last_body = x.res.body;
        return row;
      }
      ++*other_replies;
      if (x.fate != net::Exchange::Fate::REPLIED || net::mono_us() >= until) {
        if (row) got = row;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return got;
  }

  bool order_check(const std::vector<MarketRow>& rows) {
    std::printf("\norder check (%s):\n", base.host.c_str());
    std::string why;
    auto m = choose(rows);
    if (!m) {
      std::printf("  no market fits (open, a 1c grid at $0.01, an ask above it, two minutes to its close): skipped\n");
      return false;
    }
    CreateOrder o;
    o.ticker = m->ticker;
    o.exchange_index = m->exchange_index.value_or(0);
    o.side = BookSide::BID;
    o.count = kOneContract;
    o.price = 100;   // $0.0100
    o.tif = TimeInForce::GTC;
    o.post_only = true;
    o.cancel_on_pause = true;
    o.expiration_s = wall_s() + 15;
    o.stp = Stp::MAKER;   // Manual's setting (plan section 3)
    o.client_order_id = "P-" + hex16();
    std::printf("  %s shard %d: post-only bid 1 @ %s, expires in 15 s, cid %s (best bid %s, best ask %s)\n",
                o.ticker.c_str(), o.exchange_index, price_wire(o.price).c_str(), o.client_order_id.c_str(),
                m->yes_bid ? price_wire(*m->yes_bid).c_str() : "-", m->yes_ask ? price_wire(*m->yes_ask).c_str() : "-");
    WireRequest cr;
    if (!build_create(o, &cr, &why)) {
      std::printf("  the builder refused it: %s\n", why.c_str());
      return false;
    }
    net::Exchange x = send(cr, "create order (POST)");
    WriteOutcome w = classify_write(WireOp::CREATE, x);
    if (x.fate == net::Exchange::Fate::REPLIED) dump("create", x.res.body);
    std::string order_id;
    switch (w.kind) {
      case WriteOutcome::Kind::ACK: {
        auto ack = parse_create_ack(x.res.body, &why);
        order_id = ack->order_id;
        std::printf("    outcome ACK: order_id %s, filled %s, remaining %s%s\n", ack->order_id.c_str(),
                    count_wire(ack->fill_count).c_str(), count_wire(ack->remaining).c_str(),
                    ack->avg_fill_price ? (", average fill " + micros_str(*ack->avg_fill_price)).c_str() : "");
        break;
      }
      case WriteOutcome::Kind::REFUSED:
        std::printf("    outcome REFUSED %s (HTTP %d): %s\n", reject_class_name(w.cls), w.http, w.why.c_str());
        return false;
      case WriteOutcome::Kind::NOT_SENT:
        std::printf("    outcome NOT_SENT: %s\n", w.why.c_str());
        return false;
      case WriteOutcome::Kind::UNKNOWN: {
        // the UNKNOWN rule (plan section 3): the venue may hold it; look for the cid for 3 s
        std::printf("    outcome UNKNOWN: %s\n", w.why.c_str());
        ListQuery q;
        q.ticker = o.ticker;
        q.status = "resting";
        for (int i = 0; i < 30 && order_id.empty(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          net::Exchange lx = send(build_get_orders(q), "resting orders (the UNKNOWN rule)", i == 0);
          auto page = ok2xx(lx) ? parse_orders(lx.res.body, &why) : std::nullopt;
          if (page)
            for (const auto& r : page->rows)
              if (r.client_order_id == o.client_order_id) order_id = r.order_id;
        }
        std::printf("    %s\n", order_id.empty()
                                    ? "no order with this cid rests 3 s later (its 15 s expiry covers one that appears)"
                                    : ("found resting as " + order_id + ": cancelling it").c_str());
        if (order_id.empty()) return false;
        break;
      }
    }

    // read back: demo answered 404 for about 0.7 s after a 201 on 2026-10-07
    const std::int64_t t_ack = net::mono_us();
    int others = 0;
    std::string body;
    auto row = poll_order(order_id, 3000, [](const OrderRow&) { return true; }, &others, &body);
    if (row) {
      std::printf("    read back %s after %.0f ms (%d repl%s before it: 404 right after the 201)\n",
                  order_status_name(row->status), (net::mono_us() - t_ack) / 1000.0, others, others == 1 ? "y" : "ies");
      if (!body.empty()) dump("order", body);
      if (row->expiration_ms && row->created_ms)
        std::printf("    expiration_time - created_time = %.3f s (sent now + 15 s in whole seconds)\n",
                    (*row->expiration_ms - *row->created_ms) / 1000.0);
    } else {
      std::printf("    not readable 3 s after the acknowledgement (%d replies)\n", others);
      all_ok = false;
    }

    WireRequest cx;
    if (!build_cancel(order_id, o.exchange_index, o.ticker, -1, &cx, &why)) {
      std::printf("  the cancel builder refused it: %s\n", why.c_str());
      return false;
    }
    x = send(cx, "cancel order (DELETE)");
    w = classify_write(WireOp::CANCEL, x);
    if (x.fate == net::Exchange::Fate::REPLIED) dump("cancel", x.res.body);
    bool cancelled = false;
    if (w.kind == WriteOutcome::Kind::ACK) {
      auto ack = parse_cancel_ack(x.res.body, &why);
      std::printf("    outcome ACK: reduced_by %s\n", count_wire(ack->reduced_by).c_str());
      cancelled = true;
    } else if (w.kind == WriteOutcome::Kind::REFUSED && w.cls == RejectClass::GONE) {
      std::printf("    outcome GONE: nothing left to cancel (filled or expired first)\n");
      cancelled = true;
    } else {
      std::printf("    outcome %s%s: %s\n", write_kind_name(w.kind),
                  w.kind == WriteOutcome::Kind::REFUSED ? (std::string(" ") + reject_class_name(w.cls)).c_str() : "", w.why.c_str());
    }
    const std::int64_t t_cx = net::mono_us();
    auto after = poll_order(order_id, 3000, [](const OrderRow& r) { return r.status != OrderStatus::RESTING; }, &others, &body);
    if (after) {
      std::printf("    read back %s after %.0f ms (%d repl%s still said resting)\n", order_status_name(after->status),
                  (net::mono_us() - t_cx) / 1000.0, others, others == 1 ? "y" : "ies");
      if (!body.empty()) dump(after->status == OrderStatus::CANCELED ? "order_canceled" : "order_after", body);
    } else {
      std::printf("    still resting 3 s after the cancel's reply\n");
      cancelled = false;
    }

    ListQuery q;
    q.status = "resting";
    q.limit = 200;
    x = send(build_get_orders(q), "resting orders after");
    expect2xx(x);
    std::size_t ours = 0, all = 0;
    if (auto page = ok2xx(x) ? parse_orders(x.res.body, &why) : std::nullopt) {
      all = page->rows.size();
      for (const auto& r : page->rows)
        if (r.client_order_id == o.client_order_id) ++ours;
    }
    std::printf("    resting orders: %zu (this check's: %zu)\n", all, ours);
    if (ours > 0) {
      std::printf("    THIS CHECK LEFT AN ORDER RESTING: cancelling again\n");
      send(cx, "cancel order (DELETE), again");
      return false;
    }
    return cancelled && ok2xx(x);
  }

  void sign_time() {
    std::vector<double> us;
    for (int i = 0; i < 2000; ++i) {
      const std::int64_t t0 = net::mono_us();
      signer->sign(now_ms(), "GET", std::string(kBasePath) + "/portfolio/balance");
      us.push_back(static_cast<double>(net::mono_us() - t0));
    }
    std::printf("sign (%s, this key): p50 %.1f us, p90 %.1f us\n", key_type_name(signer->type()), pct(us, 0.5), pct(us, 0.9));
  }

  void summary() {
    std::printf("\nsummary:\n");
    print_stats("  ", "write->first byte", ttfb);
    print_stats("  ", "write->reply", done);
    print_stats("  ", "take", take);
    const net::PoolStats s = pool->stats();
    std::printf("pool: %llu handshakes (p50 %.1f ms), %llu takes, %llu hits, %llu misses, %llu stale on take, %llu closed by "
                "the venue, %llu idle retired\n",
                static_cast<unsigned long long>(s.handshakes), pct(s.handshake_ms, 0.5),
                static_cast<unsigned long long>(s.takes), static_cast<unsigned long long>(s.hits),
                static_cast<unsigned long long>(s.misses), static_cast<unsigned long long>(s.stale_on_take),
                static_cast<unsigned long long>(s.closed_by_venue), static_cast<unsigned long long>(s.idle_retired));
    const net::FactoryStats fs = factory.stats();
    std::printf("factory: %llu sockets, %llu handshakes (%llu resumed, %llu failed), %llu refused\n",
                static_cast<unsigned long long>(fs.sockets), static_cast<unsigned long long>(fs.handshakes),
                static_cast<unsigned long long>(fs.resumed), static_cast<unsigned long long>(fs.handshake_fail),
                static_cast<unsigned long long>(fs.refused));
  }
};

}  // namespace

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);   // a write into a reset connection is an error to report, not an exit
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string s = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (s == "--key" && (v = value())) a.key = v;
    else if (s == "--base" && (v = value())) a.base = v;
    else if (s == "--handshakes" && (v = value())) a.handshakes = std::max(0, std::atoi(v));
    else if (s == "--loopback-ca" && (v = value())) a.loopback_ca = v;
    else if (s == "--order-check") a.order_check = true;
    else if (s == "--order-ticker" && (v = value())) a.order_ticker = v;
    else if (s == "--dump" && (v = value())) a.dump = v;
    else return usage();
  }
  std::string err, desc;
  std::unique_ptr<KalshiSigner> signer;
  if (!load_key(a, &signer, &desc, &err)) {
    std::fprintf(stderr, "kalshi-demo-probe: %s\n", err.c_str());
    return 2;
  }
  auto ep = net::Endpoint::parse(a.base, &err);
  if (!ep) {
    std::fprintf(stderr, "kalshi-demo-probe: --base: %s\n", err.c_str());
    return 2;
  }
  if (!a.dump.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(a.dump, ec);
  }
  DemoProbe p(a, *ep, std::move(signer));
  std::printf("key: %s\n", desc.c_str());
  const std::string refused = p.guard.refuse(p.base.host);
  if (!p.factory.ok(&err)) {
    std::fprintf(stderr, "kalshi-demo-probe: %s\n", err.c_str());
    return 2;
  }
  std::printf("venue: %s (guard: %s), trust store: %zu roots\n", p.base.url().c_str(),
              refused.empty() ? (classify_host(p.base.host) == HostClass::DEMO ? "allowed, demo" : "allowed, loopback")
                              : refused.c_str(),
              p.factory.trust_roots());
  if (!refused.empty()) {
    std::fprintf(stderr, "kalshi-demo-probe: refusing to run against %s\n", p.base.host.c_str());
    return 2;
  }
  p.production_check();
  p.handshakes();

  net::PoolConfig pc;
  pc.name = "probe";
  pc.endpoint = p.base;
  pc.spares = 2;
  p.pool = std::make_unique<net::SparePool>(p.factory, pc);
  p.pool->start();
  if (!p.pool->wait_ready(2, 10000)) {
    std::printf("pool: no connection ready after 10 s: %s\n", p.pool->stats().last_error.c_str());
    p.all_ok = false;
  }
  const std::vector<MarketRow> rows = p.reads();
  bool order_ok = true;
  if (a.order_check) order_ok = p.order_check(rows);
  p.summary();
  p.sign_time();
  p.pool->stop();
  std::fflush(stdout);
  const bool ok = p.all_ok && order_ok;
  std::printf("%s\n", ok ? "ok" : "NOT OK (see above)");
  return ok ? 0 : 1;
}
