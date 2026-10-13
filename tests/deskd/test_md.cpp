#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>
#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/md/kalshi_md.h"
#include "deskd/net/guard.h"
#include "deskd/net/ws.h"
#include "fake_kalshi.h"

using namespace deskd;
using json = nlohmann::json;
namespace {
std::string frame(net::WsOp op, const std::string& payload, bool fin = true) {
  std::string out(1, static_cast<char>((fin ? 128 : 0) | static_cast<unsigned>(op)));
  if (payload.size() < 126) out.push_back(static_cast<char>(payload.size()));
  else if (payload.size() <= 65535) {
    out.push_back(126); out.push_back(static_cast<char>(payload.size() >> 8)); out.push_back(static_cast<char>(payload.size()));
  } else {
    out.push_back(127);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> (i * 8)));
  }
  return out + payload;
}
void framing() {
  CHECK(net::ws_accept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  for (const std::size_t n : {0u, 125u, 126u, 4097u, 8193u, 65535u, 65536u, 200000u}) {
    const std::string text(n, 'a'); const auto wire = frame(net::WsOp::TEXT, text);
    for (const auto chunk : {1u, 4096u, 8192u, 300000u}) {
      net::WsParser parser;
      bool ok = true;
      for (std::size_t i = 0; i < wire.size(); i += chunk) ok = ok && parser.feed(wire.substr(i, chunk));
      net::WsEvent e;
      CHECK(ok && parser.pop(&e) && e.op == net::WsOp::TEXT && e.payload == text && !parser.pop(&e));
    }
    const auto client = net::ws_client_frame(net::WsOp::TEXT, text);
    CHECK(!client.empty() && (static_cast<unsigned char>(client[1]) & 0x80));
    const auto head = n < 126 ? 2u : n <= 65535 ? 4u : 10u;
    std::string decoded;
    for (std::size_t i = 0; i < n; ++i) decoded.push_back(client[head + 4 + i] ^ client[head + i % 4]);
    CHECK(decoded == text);
  }
  net::WsParser parser;
  CHECK(parser.feed(frame(net::WsOp::TEXT, "a\xe2", false) + frame(net::WsOp::PING, "ping") +
                    frame(net::WsOp::CONT, "\x82\xac", false) + frame(net::WsOp::CONT, "b")));
  net::WsEvent event;
  CHECK(parser.pop(&event) && event.op == net::WsOp::PING && event.payload == "ping");
  CHECK(parser.pop(&event) && event.op == net::WsOp::TEXT && event.payload == "a\xe2\x82\xac" "b");
  for (const auto& bad : {net::ws_client_frame(net::WsOp::TEXT, "masked"), frame(net::WsOp::CONT, "orphan"),
                         frame(net::WsOp::PING, "fragment", false), frame(net::WsOp::PING, std::string(126, 'x')),
                         frame(net::WsOp::TEXT, "\xc0\x80"), frame(net::WsOp::CLOSE, "x"),
                         frame(net::WsOp::TEXT, "a", false) + frame(net::WsOp::TEXT, "b")}) {
    net::WsParser rejected; CHECK(!rejected.feed(bad));
  }
  net::WsParser limited(1024);
  CHECK(!limited.feed(frame(net::WsOp::TEXT, std::string(1025, 'x'))));
  net::WsParser fragment_limit(10);
  CHECK(!fragment_limit.feed(frame(net::WsOp::TEXT, "12345678", false) + frame(net::WsOp::CONT, "901")));
  std::string oversized(10, '\0'); oversized[0] = static_cast<char>(0x81); oversized[1] = 127; oversized[2] = 1;
  net::WsParser huge; CHECK(!huge.feed(oversized));
}
json snapshot(const std::string& ticker, int sid, int seq) {
  return {{"type", "orderbook_snapshot"}, {"sid", sid}, {"seq", seq}, {"sending_ts_ms", 1700000000000LL},
          {"msg", {{"market_ticker", ticker}, {"yes_dollars_fp", json::array({json::array({"0.4000", "5.50"}), json::array({"0.3900", "7.00"})})},
                   {"no_dollars_fp", json::array({json::array({"0.5800", "3.00"}), json::array({"0.5700", "2.00"})})}}}};
}
json delta(const std::string& ticker, int sid, int seq, const std::string& side, const std::string& px, const std::string& count) {
  return {{"type", "orderbook_delta"}, {"sid", sid}, {"seq", seq},
          {"msg", {{"market_ticker", ticker}, {"side", side}, {"price_dollars", px}, {"delta_fp", count}}}};
}
void reducer() {
  std::vector<MdBook> published;
  MdBooks b([&](const MdBook& book) { published.push_back(book); });
  std::string why;
  b.watch({"A", "B", "C"}, 1);
  CHECK(!b.ready());
  CHECK(!b.subscribed(1, {}, &why));
  CHECK(b.subscribed(1, {"A", "B"}, &why));
  CHECK(b.subscribed(2, {"C"}, &why));
  CHECK(!b.subscribed(3, {"A"}, &why));
  CHECK(b.message(snapshot("A", 1, 2), 100, &why));
  const auto a = b.books().at("A");
  CHECK(a.bids.size() == 2 && a.bids[0].px == 4000 && a.bids[0].qty == 550);
  CHECK(a.asks.size() == 2 && a.asks[0].px == 4200 && a.asks[1].px == 4300);
  CHECK(a.venue_ms && *a.venue_ms == 1700000000000LL && a.last_change_ms == 100);
  CHECK(b.message(snapshot("C", 2, 1), 101, &why));  // another sid has an independent sequence
  CHECK(b.message({{"type", "ok"}, {"sid", 1}, {"seq", 3}}, 102, &why));
  CHECK(b.message(snapshot("B", 1, 4), 103, &why));
  CHECK(b.ready());
  CHECK(b.message(delta("A", 1, 5, "yes", "0.4000", "-0.50"), 104, &why));
  CHECK(!b.books().at("A").bids.empty() && b.books().at("A").bids[0].qty == 500 && !b.books().at("A").venue_ms);
  CHECK(b.message(delta("A", 1, 6, "no", "0.5800", "-3.00"), 105, &why));
  CHECK(b.books().at("A").asks.size() == 1 && b.books().at("A").asks[0].px == 4300);
  CHECK(!b.message(delta("A", 1, 8, "yes", "0.3900", "1.00"), 106, &why));
  CHECK(b.books().at("A").stale && b.books().at("B").stale && !b.books().at("C").stale);
  CHECK(!b.message(snapshot("A", 1, 9), 107, &why));  // an old sid cannot repair a gap
  b.disconnected("reconnect", 110);
  CHECK(b.subscribed(3, {"A", "B"}, &why) && b.subscribed(4, {"C"}, &why));
  CHECK(b.message(snapshot("A", 3, 1), 111, &why));
  CHECK(!b.ready());  // one refreshed ticker cannot clear the other markets' stale state
  CHECK(b.message(snapshot("B", 3, 2), 112, &why));
  CHECK(b.message(snapshot("C", 4, 1), 113, &why));
  CHECK(b.ready());
  auto empty = snapshot("A", 3, 3); empty["msg"].erase("yes_dollars_fp"); empty["msg"].erase("no_dollars_fp");
  CHECK(b.message(empty, 114, &why));
  CHECK(b.books().at("A").bids.empty() && b.books().at("A").asks.empty());
  CHECK(!b.message(delta("A", 3, 4, "yes", "0.5000", "-0.01"), 115, &why));
  CHECK(b.books().at("A").stale);
  b.disconnected("reconnect", 120); CHECK(b.subscribed(5, {"A", "B", "C"}, &why));
  auto malformed = snapshot("A", 5, 1); malformed["msg"]["yes_dollars_fp"][0][0] = "0.40001";
  CHECK(!b.message(malformed, 121, &why));
  CHECK(b.books().at("A").bids.empty());  // malformed updates never partly apply
  b.watch({}, 122); CHECK(b.books().empty() && published.back().reason == "unwatched");
}
std::unique_ptr<KalshiSigner> signer() {
  EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
  if (!key) return {};
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
  char* bytes = nullptr; const auto n = BIO_get_mem_data(bio, &bytes);
  std::string pem(bytes, static_cast<std::size_t>(n)); BIO_free(bio); EVP_PKEY_free(key);
  std::string why; return KalshiSigner::from_pem("md-test", pem, "ed25519", &why);
}
bool wait_for(const std::function<bool()>& predicate, int ms = 4000) {
  const auto until = mono_ms() + ms;
  while (mono_ms() < until) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  return predicate();
}
void socket_integration() {
  fakex::FakeConfig config; config.rate_limits = false; config.auth = fakex::AuthMode::PRESENCE; config.ws_ping_ms = 40;
  fakex::FakeKalshi exchange(config);
  exchange.with_engine([](fakex::Engine& e, std::int64_t now) {
    for (const auto& ticker : {"TEST-A", "TEST-B", "TEST-C"}) {
      fakex::MarketSpec spec; spec.ticker = ticker; spec.exchange_index = 0; e.add_market(spec, now);
      e.street_add(ticker, false, 4000, 500, now); e.street_add(ticker, true, 5800, 300, now);
    }
  });
  std::string why; CHECK(exchange.start(&why));
  auto key = signer(); CHECK(key != nullptr); if (!key) return;
  SendGuard guard(false, nullptr); net::ConnFactory factory(guard);
  auto endpoint = net::Endpoint::parse("http://127.0.0.1:" + std::to_string(exchange.port()) + "/trade-api/ws/v2", &why);
  CHECK(endpoint.has_value()); if (!endpoint) return;
  FeedState feed;
  std::mutex mutex; std::map<std::string, MdBook> latest; int stale = 0; std::map<std::string, int> stale_of;
  KalshiMd::Options options; options.markets_per_subscription = 2; options.ping_ms = 80;
  options.pong_timeout_ms = 500; options.backoff_min_ms = 20; options.backoff_max_ms = 100;
  KalshiMd md(*endpoint, factory, *key, feed, [&](const MdBook& book) {
    std::lock_guard<std::mutex> lock(mutex); latest[book.ticker] = book; if (book.stale) { ++stale; ++stale_of[book.ticker]; }
  }, options);
  CHECK(!md.set_watch({}, &why)); CHECK(!md.set_watch({"BAD\r\n"}, &why));
  CHECK(md.set_watch({"TEST-C", "TEST-B", "TEST-A"}, &why));
  Worker worker("test-md"); worker.start([&](Worker& w) { md.run(w); });
  CHECK(wait_for([&] { return feed.view().up && !feed.view().gap; }));
  {
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(latest.size() == 3 && !latest.at("TEST-A").stale);
    CHECK(latest.at("TEST-A").bids[0].px == 4000 && latest.at("TEST-A").asks[0].px == 4200);
    CHECK(latest.at("TEST-C").sid != latest.at("TEST-A").sid);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  CHECK(feed.view().up && !feed.view().gap);  // quiet books survive several verified ping/pongs
  CHECK(worker.progress() > 5);
  int before = 0; { std::lock_guard<std::mutex> lock(mutex); before = stale; }
  exchange.drop_ws_messages("orderbook_delta", 1);
  exchange.with_engine([](fakex::Engine& e, std::int64_t now) {
    e.street_add("TEST-A", false, 3900, 100, now);
    e.street_add("TEST-A", false, 3800, 100, now);
  });
  CHECK(wait_for([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return stale > before && !latest.at("TEST-A").stale && latest.at("TEST-A").bids.size() == 3;
  }));
  CHECK(wait_for([&] { return feed.view().up && !feed.view().gap; }));
  exchange.drop_ws_connections();
  CHECK(wait_for([&] { return factory.stats().connects >= 3 && feed.view().up && !feed.view().gap; }));
  {   // a watch-list change on a live connection: in place, no reconnect; the other books never
      // go stale and the feed never gaps
    const auto connects = factory.stats().connects;
    std::map<std::string, int> was; { std::lock_guard<std::mutex> lock(mutex); was = stale_of; }
    CHECK(md.set_watch({"TEST-B", "TEST-C"}, &why));
    CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(mutex); return latest.at("TEST-A").reason == "unwatched"; }));
    // TEST-A's frames still come on its old subscription and are ignored
    exchange.with_engine([](fakex::Engine& e, std::int64_t now) { e.street_add("TEST-A", false, 3700, 100, now); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(feed.view().up && !feed.view().gap);
    // watched again: a new subscription and its own snapshot
    CHECK(md.set_watch({"TEST-A", "TEST-B", "TEST-C"}, &why));
    CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(mutex); return latest.at("TEST-A").reason.empty() && !latest.at("TEST-A").stale; }));
    std::size_t levels = 0; { std::lock_guard<std::mutex> lock(mutex); levels = latest.at("TEST-A").bids.size(); }
    // a delta on it applies once, though both its subscriptions carry it
    exchange.with_engine([](fakex::Engine& e, std::int64_t now) { e.street_add("TEST-A", false, 3600, 100, now); });
    CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(mutex); return latest.at("TEST-A").bids.size() == levels + 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
      std::lock_guard<std::mutex> lock(mutex);
      CHECK(latest.at("TEST-A").bids.size() == levels + 1 && latest.at("TEST-A").bids.back().qty == 100);
      CHECK(stale_of["TEST-B"] == was["TEST-B"] && stale_of["TEST-C"] == was["TEST-C"]);
    }
    CHECK(factory.stats().connects == connects && feed.view().up && !feed.view().gap);
  }
  md.clear_watch();
  CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(mutex); return latest.at("TEST-A").reason == "unwatched"; }));
  CHECK(md.set_watch({"TEST-B"}, &why));
  CHECK(wait_for([&] { std::lock_guard<std::mutex> lock(mutex); return latest.at("TEST-B").reason.empty(); }));
  worker.request_stop(); worker.join();
  CHECK(!feed.view().up);
  const auto logs = exchange.wire_log();
  int subscriptions = 0;
  for (const auto& line : logs) {
    if (line.find("subscribed orderbook_delta") != std::string::npos) ++subscriptions;
    CHECK(line.find("subscribed ticker") == std::string::npos);
  }
  CHECK(subscriptions >= 5);
  const auto sockets = factory.stats().sockets;
  auto production = net::Endpoint::parse("https://api.elections.kalshi.com/trade-api/ws/v2", &why);
  net::WsClient refused;
  CHECK(production && !refused.connect(factory, *production, {}, 200, &why));
  CHECK(factory.stats().sockets == sockets);  // every WS/reconnect uses the existing guard
}
}  // namespace

// The trade channel: prints in YES terms for subscribed markets only; a gap or a malformed print
// is counted or skipped, never a reason to drop the books.
void trades() {
  std::vector<MdTrade> prints;
  MdBooks b([](const MdBook&) {}, [&](const MdTrade& t) { prints.push_back(t); });
  std::string why;
  b.watch({"A", "B"}, 1);
  CHECK(b.subscribed(1, {"A", "B"}, &why));
  CHECK(b.subscribed_trades(2, {"A"}, &why));
  CHECK(!b.subscribed_trades(2, {"A"}, &why));      // duplicate sid
  CHECK(!b.subscribed_trades(3, {"Z"}, &why));      // not watched
  CHECK(b.message(snapshot("A", 1, 1), 10, &why) && b.message(snapshot("B", 1, 2), 11, &why) && b.ready());
  auto trade = [](const std::string& tk, std::int64_t seq, const char* px, const char* qty, const char* taker) {
    return json{{"type", "trade"}, {"sid", 2}, {"seq", seq},
                {"msg", {{"trade_id", "t" + std::to_string(seq)}, {"market_ticker", tk}, {"yes_price_dollars", px},
                         {"count_fp", qty}, {"taker_side", taker}, {"ts", 1700000001}}}};
  };
  CHECK(b.message(trade("A", 1, "0.4200", "2.00", "yes"), 20, &why));
  CHECK(prints.size() == 1 && prints[0].px == 4200 && prints[0].qty == 200 && prints[0].taker_bought_yes &&
        prints[0].ts_ms == 1700000001000LL && prints[0].trade_id == "t1");
  CHECK(b.message(trade("A", 2, "0.4100", "0.50", "no"), 21, &why));
  CHECK(prints.size() == 2 && !prints[1].taker_bought_yes && prints[1].qty == 50);
  CHECK(b.message(trade("A", 5, "0.4100", "1.00", "no"), 22, &why) && b.trade_gaps() == 1 && b.ready());   // gap: counted
  CHECK(b.message(trade("B", 6, "0.4100", "1.00", "no"), 23, &why) && prints.size() == 3);    // not this sid's market
  CHECK(b.message(trade("A", 7, "1.5", "1.00", "no"), 24, &why) && prints.size() == 3);      // malformed: skipped
  CHECK(b.message({{"type", "ok"}, {"sid", 2}, {"seq", 8}}, 25, &why) && b.ready());          // other frames on the sid
  json unnumbered = trade("A", 9, "0.4300", "1.00", "yes"); unnumbered.erase("seq");
  CHECK(b.message(unnumbered, 26, &why) && prints.size() == 4 && prints[3].px == 4300);       // a print without seq
  CHECK(b.message(delta("A", 1, 3, "yes", "0.4000", "-0.50"), 27, &why) && b.ready());        // books unaffected
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  framing(); reducer(); trades(); socket_integration();
  return finish("deskd-md");
}
