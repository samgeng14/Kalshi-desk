// deskd gw/: the Kalshi signer, the wire builders and parsers, and what a write's reply means.
//
//   - signing: RSA-PSS and Ed25519 keys made at run time, the key type checked against the PEM,
//     an Ed25519 known answer computed with Python's cryptography (RFC 8032 key 1, 2026-10-07),
//     RSA-PSS checked by Python's cryptography too when it is installed, the query never signed,
//     four threads signing at once, and sign times for both
//   - values: prices, counts, dollars to 6 decimals, RFC 3339 times, query encoding
//   - builders: create order V2's body field by field, every refusal in check_create, intents in
//     venue terms, cancels, batch cancels, cancel-all and the reads' queries
//   - parsers: the documented shapes (docs.kalshi.com, 2026-10-06/07), a missing or mistyped
//     required field failing the whole reply, and the shapes Kalshi's demo sent the probe
//   - classify_write: ACK, REFUSED with its class, UNKNOWN (5xx, 408, a cut, no venue body, an
//     unreadable 2xx), NOT_SENT
//   - round trips against the fake over TLS, signed and verified, through the spare pool: create,
//     get, fills, positions, balance per shard, cancel, a cancel of a gone order, batch cancel,
//     cancel-all, markets, orderbook, the soft refusals (post-only cross, collateral, duplicate
//     cid, triggered group), a bad signature, and the faults that make a write UNKNOWN
//   - the demo probe binary (tools/kalshi_demo_probe.cpp) run against the fake over TLS through
//     its real command line: handshakes, the reads, the order check, the dumps, no key material
//     in its output; a production base and a non-demo key file refused before any socket
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <signal.h>
#include <sys/wait.h>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#include "deskd/app/log.h"
#include "deskd/build_paths.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/net/pool.h"
#include "net_fixture.h"

using namespace deskd;
using namespace deskd::kalshi;
using namespace deskd::net;
using json = nlohmann::json;
using K = WriteOutcome::Kind;

namespace {

// RFC 8032 test 1's secret key, PKCS#8: a published test vector, not a credential
const char* kEdPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MC4CAQAwBQYDK2VwBCIEIJ1hsZ3v/VpguoRK9JLsLMREScVpezJpGXA7rAMcrn9g\n"
    "-----END PRIVATE KEY-----\n";

std::string pem_of(EVP_PKEY* k, bool pub) {
  BIO* b = BIO_new(BIO_s_mem());
  if (pub)
    PEM_write_bio_PUBKEY(b, k);
  else
    PEM_write_bio_PrivateKey(b, k, nullptr, nullptr, 0, nullptr, nullptr);
  char* p = nullptr;
  const long n = BIO_get_mem_data(b, &p);
  std::string s(p, static_cast<std::size_t>(n));
  BIO_free(b);
  return s;
}
std::string new_key_pem(const char* type, int bits = 0, bool pub = false) {
  EVP_PKEY* k = bits ? EVP_PKEY_Q_keygen(nullptr, nullptr, type, static_cast<std::size_t>(bits))
                     : EVP_PKEY_Q_keygen(nullptr, nullptr, type);
  std::string s = pem_of(k, pub);
  EVP_PKEY_free(k);
  return s;
}
void write_file(const std::string& path, const std::string& text) {
  std::ofstream(path) << text;
}

net::Exchange replied(int status, const std::string& body) {
  net::Exchange x;
  x.fate = net::Exchange::Fate::REPLIED;
  x.res.status = status;
  x.res.body = body;
  return x;
}

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ---- signing ----------------------------------------------------------------------------------

static void test_signer() {
  TempDir d;
  std::string err;
  const std::string rsa = new_key_pem("RSA", 2048), rsa_small = new_key_pem("RSA", 1024),
                    ed = new_key_pem("ED25519"), ed_pub = new_key_pem("ED25519", 0, true);
  write_file(d.file("rsa.pem"), rsa);
  write_file(d.file("ed.pem"), ed);
  write_file(d.file("small.pem"), rsa_small);
  write_file(d.file("pub.pem"), ed_pub);
  write_file(d.file("junk.pem"), "-----BEGIN PRIVATE KEY-----\nnot a key\n-----END PRIVATE KEY-----\n");

  auto r = KalshiSigner::load("key-r", d.file("rsa.pem"), "rsa", &err);
  auto e = KalshiSigner::load("key-e", d.file("ed.pem"), "ed25519", &err);
  CHECK(r && r->type() == KeyType::RSA && r->bits() == 2048);
  CHECK(e && e->type() == KeyType::ED25519);
  // the config's key type must match the file
  CHECK(!KalshiSigner::load("k", d.file("rsa.pem"), "ed25519", &err) &&
        err.find("holds an rsa key, but the config says ed25519") != std::string::npos);
  CHECK(!KalshiSigner::load("k", d.file("ed.pem"), "rsa", &err) && err.find("ed25519 key") != std::string::npos);
  CHECK(!KalshiSigner::load("k", d.file("ed.pem"), "ecdsa", &err) && err.find("key_type") != std::string::npos);
  CHECK(!KalshiSigner::load("k", d.file("small.pem"), "rsa", &err) && err.find("1024 bits") != std::string::npos);
  CHECK(!KalshiSigner::load("k", d.file("pub.pem"), "ed25519", &err));
  CHECK(!KalshiSigner::load("k", d.file("junk.pem"), "ed25519", &err) && err.find("junk.pem") != std::string::npos);
  CHECK(!KalshiSigner::load("k", d.file("missing.pem"), "ed25519", &err) &&
        err == "cannot open the private key file " + d.file("missing.pem"));
  CHECK(!KalshiSigner::load("", d.file("ed.pem"), "ed25519", &err));
  // an error never carries the key: none of the PEM's base64 shows up
  KalshiSigner::load("k", d.file("rsa.pem"), "ed25519", &err);
  CHECK(err.find(rsa.substr(40, 30)) == std::string::npos);

  // the message: ts + METHOD + path, the query cut off
  CHECK(KalshiSigner::message(1700000000000, "GET", "/trade-api/v2/portfolio/orders?limit=5") ==
        "1700000000000GET/trade-api/v2/portfolio/orders");
  // Ed25519 known answers (Python cryptography 46.0.2 with RFC 8032 key 1; the same key gives the
  // RFC's own signature for the empty message)
  auto kat = KalshiSigner::from_pem("kat", kEdPem, "ed25519", &err);
  CHECK(kat != nullptr);
  if (kat) {
    CHECK(kat->sign(1700000000000, "GET", "/trade-api/v2/portfolio/balance") ==
          "NZNbn42nWTCkd3HNR5AZoq/t/QrVPbmH3kEjYPYNKXYaxQNVfzDcKmioxh2AAht4ymXHC+6UxJgODwo32QYwBg==");
    CHECK(kat->sign(1700000000000, "GET", "/trade-api/v2/portfolio/balance?subaccount=0&exchange_index=2") ==
          "NZNbn42nWTCkd3HNR5AZoq/t/QrVPbmH3kEjYPYNKXYaxQNVfzDcKmioxh2AAht4ymXHC+6UxJgODwo32QYwBg==");
    CHECK(kat->sign(1791300600123, "POST", "/trade-api/v2/portfolio/events/orders") ==
          "R17w9YqWhKaon3NAF4BXJ7viZe0b62ZLaUkvM/l+iWVJvcgUkgmldca3tcYdBJfmJ9PW9vPKaejIiwwWstM1Bw==");
    const auto h = kat->headers(1700000000000, "GET", "/trade-api/v2/portfolio/balance");
    CHECK(h.size() == 3 && h[0].first == "KALSHI-ACCESS-KEY" && h[0].second == "kat" &&
          h[1].first == "KALSHI-ACCESS-TIMESTAMP" && h[1].second == "1700000000000" &&
          h[2].first == "KALSHI-ACCESS-SIGNATURE" && h[2].second.size() == 88);
  }
  // RSA-PSS: salted, so two signatures differ and both verify; 2048 bits = 256 bytes = 344 base64
  const std::string s1 = r->sign(1, "GET", "/trade-api/v2/x"), s2 = r->sign(1, "GET", "/trade-api/v2/x");
  CHECK(s1 != s2 && s1.size() == 344 && r->verify(1, "GET", "/trade-api/v2/x", s1) &&
        r->verify(1, "GET", "/trade-api/v2/x", s2));
  CHECK(!r->verify(2, "GET", "/trade-api/v2/x", s1) && !r->verify(1, "POST", "/trade-api/v2/x", s1));
  const std::string es = e->sign(5, "DELETE", "/trade-api/v2/portfolio/events/orders/abc");
  CHECK(es.size() == 88 && es == e->sign(5, "DELETE", "/trade-api/v2/portfolio/events/orders/abc"));   // deterministic
  CHECK(e->verify(5, "DELETE", "/trade-api/v2/portfolio/events/orders/abc", es));
  CHECK(!e->verify(5, "DELETE", "/trade-api/v2/portfolio/events/orders/abd", es));
  CHECK(!e->verify(5, "DELETE", "/trade-api/v2/portfolio/events/orders/abc", "not base64!"));

  // RSA-PSS checked by an implementation that is not OpenSSL's EVP path: Python's cryptography,
  // PSS(MGF1(SHA-256), salt_length=DIGEST_LENGTH), exactly as api_keys.md writes it
  if (std::string(DESKD_PYTHON).empty()) {
    std::printf("RSA-PSS cross-check skipped: no python3\n");
  } else {
    write_file(d.file("rsa_pub.pem"), r->public_pem());
    write_file(d.file("sig.txt"), s1);
    write_file(d.file("check.py"),
               "import base64,sys\n"
               "try:\n"
               "  from cryptography.hazmat.primitives import hashes, serialization\n"
               "  from cryptography.hazmat.primitives.asymmetric import padding\n"
               "except ImportError:\n"
               "  print('skip'); sys.exit(0)\n"
               "k=serialization.load_pem_public_key(open(sys.argv[1],'rb').read())\n"
               "sig=base64.b64decode(open(sys.argv[2]).read())\n"
               "k.verify(sig,b'1GET/trade-api/v2/x',padding.PSS(mgf=padding.MGF1(hashes.SHA256()),"
               "salt_length=padding.PSS.DIGEST_LENGTH),hashes.SHA256())\n"
               "print('ok')\n");
    const std::string cmd = std::string(DESKD_PYTHON) + " " + d.file("check.py") + " " + d.file("rsa_pub.pem") + " " +
                            d.file("sig.txt") + " 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    char buf[256] = {0};
    std::string out;
    while (p && std::fgets(buf, sizeof buf, p)) out += buf;
    if (p) ::pclose(p);
    CHECK(out == "ok\n" || out == "skip\n");
    if (out == "skip\n") std::printf("RSA-PSS cross-check skipped: no cryptography module\n");
    if (out != "ok\n" && out != "skip\n") std::fprintf(stderr, "  python said: %s\n", out.c_str());
  }

  // four threads signing at once over one key
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        const std::string p = "/trade-api/v2/t" + std::to_string(t);
        if (!e->verify(i, "GET", p, e->sign(i, "GET", p))) ++bad;
        if (i % 20 == 0 && !r->verify(i, "GET", p, r->sign(i, "GET", p))) ++bad;
      }
    });
  for (auto& t : ts) t.join();
  CHECK(bad == 0);

  // sign times (plan section 2 budgets 0.34 ms for RSA-PSS)
  auto time_sign = [](const KalshiSigner& s, int n) {
    std::vector<double> us;
    for (int i = 0; i < n; ++i) {
      const std::int64_t t0 = mono_us();
      const std::string sig = s.sign(1791300600000 + i, "POST", "/trade-api/v2/portfolio/events/orders");
      us.push_back(static_cast<double>(mono_us() - t0));
      if (sig.empty()) return std::vector<double>{};
    }
    return us;
  };
  const auto ed_us = time_sign(*e, 2000), rsa_us = time_sign(*r, 500);
  CHECK(!ed_us.empty() && !rsa_us.empty());
  std::printf("sign: Ed25519 p50 %.1f us p90 %.1f us; RSA-PSS-2048 p50 %.1f us p90 %.1f us\n", fx::pct(ed_us, 0.5),
              fx::pct(ed_us, 0.9), fx::pct(rsa_us, 0.5), fx::pct(rsa_us, 0.9));
}

// ---- values ------------------------------------------------------------------------------------

static void test_values() {
  CHECK(price_wire(5600) == "0.5600" && price_wire(1) == "0.0001" && price_wire(9999) == "0.9999");
  CHECK(count_wire(1000) == "10.00" && count_wire(1) == "0.01" && count_wire(-350) == "-3.50" && count_wire(0) == "0.00");
  CHECK(parse_dollars("0.5600") == 560000 && parse_dollars("0.56") == 560000 && parse_dollars("1") == 1000000);
  CHECK(parse_dollars("0.010000") == 10000 && parse_dollars("-3.2100") == -3210000 && parse_dollars("0.000001") == 1);
  CHECK(!parse_dollars("0.0000001") && !parse_dollars("") && !parse_dollars("1e3") && !parse_dollars(".5") &&
        !parse_dollars("+1") && !parse_dollars("1.") && !parse_dollars(" 1") && !parse_dollars("--1"));
  CHECK(parse_px("0.4200") == 4200 && parse_px("0.42") == 4200 && parse_px("1.0000") == 10000 && parse_px("0") == 0);
  CHECK(!parse_px("0.563333") && !parse_px("1.0001") && !parse_px("-0.5"));
  CHECK(parse_money_wire("123.4500") == 1234500 && !parse_money_wire("0.005612"));
  CHECK(parse_count("10.00") == 1000 && parse_count("10") == 1000 && parse_count("0.01") == 1 && parse_count("10.5") == 1050);
  CHECK(!parse_count("0.001") && !parse_count("-1.00") && parse_count("-1.00", true) == -100 && !parse_count("1,000"));
  CHECK(parse_time_ms("2026-10-06T15:30:00Z") == 1791300600000);
  CHECK(parse_time_ms("2026-10-06T15:30:00.123456Z") == 1791300600123);
  CHECK(parse_time_ms("2026-10-06T11:30:00.5-04:00") == 1791300600500);
  CHECK(parse_time_ms("1970-01-01T00:00:00Z") == 0);
  CHECK(parse_time_ms("2024-02-29T00:00:00Z") == 1709164800000);
  CHECK(!parse_time_ms("2026-10-06 15:30:00") && !parse_time_ms("2026-13-06T15:30:00Z") &&
        !parse_time_ms("2026-10-06T15:30:00.Z") && !parse_time_ms("2026-10-06T15:30:00+0400") && !parse_time_ms(""));
  CHECK(url_encode("KXNATGAS15M-26OCT061145-45") == "KXNATGAS15M-26OCT061145-45");
  CHECK(url_encode("a+b/c=d e&f") == "a%2Bb%2Fc%3Dd%20e%26f");
}

// ---- builders ----------------------------------------------------------------------------------

static CreateOrder post_order() {
  CreateOrder o;
  o.ticker = fx::T;
  o.side = BookSide::BID;
  o.count = 100;
  o.price = 3000;
  o.tif = TimeInForce::GTC;
  o.post_only = true;
  o.cancel_on_pause = true;
  o.expiration_s = 1791300615;
  o.stp = Stp::MAKER;
  o.client_order_id = "M-0001";
  o.exchange_index = 0;
  return o;
}

static void test_builders() {
  WireRequest r;
  std::string why;
  CHECK(build_create(post_order(), &r, &why));
  CHECK(r.method == "POST" && r.path == "/trade-api/v2/portfolio/events/orders" && r.query.empty() && r.write &&
        r.auth && r.op == WireOp::CREATE);
  const json b = json::parse(r.body);
  CHECK(b == json::parse(R"({"ticker":"KXTEST-26OCT06-A","side":"bid","count":"1.00","price":"0.3000",
        "time_in_force":"good_till_canceled","self_trade_prevention_type":"maker","client_order_id":"M-0001",
        "exchange_index":0,"post_only":true,"cancel_order_on_pause":true,"expiration_time":1791300615})"));
  CreateOrder ioc = post_order();
  ioc.tif = TimeInForce::IOC;
  ioc.post_only = ioc.cancel_on_pause = false;
  ioc.expiration_s = 0;
  ioc.reduce_only = true;
  ioc.side = BookSide::ASK;
  ioc.stp = Stp::TAKER_AT_CROSS;
  ioc.order_group_id = "g-1";
  ioc.subaccount = 0;
  ioc.exchange_index = 2;
  CHECK(build_create(ioc, &r, &why));
  CHECK(json::parse(r.body) == json::parse(R"({"ticker":"KXTEST-26OCT06-A","side":"ask","count":"1.00",
        "price":"0.3000","time_in_force":"immediate_or_cancel","self_trade_prevention_type":"taker_at_cross",
        "client_order_id":"M-0001","exchange_index":2,"reduce_only":true,"order_group_id":"g-1","subaccount":0})"));

  // every refusal: each a desk bug caught before it costs a write
  auto refuse = [&](const std::function<void(CreateOrder&)>& f, const char* frag) {
    CreateOrder o = post_order();
    f(o);
    WireRequest out;
    std::string w;
    const bool built = build_create(o, &out, &w);
    if (built || w.find(frag) == std::string::npos) std::fprintf(stderr, "  expected \"%s\", got \"%s\"\n", frag, w.c_str());
    return !built && w.find(frag) != std::string::npos;
  };
  CHECK(refuse([](CreateOrder& o) { o.ticker = ""; }, "bad ticker"));
  CHECK(refuse([](CreateOrder& o) { o.ticker = "KX TEST"; }, "bad ticker"));
  CHECK(refuse([](CreateOrder& o) { o.count = 0; }, "count"));
  CHECK(refuse([](CreateOrder& o) { o.price = 0; }, "strictly between"));
  CHECK(refuse([](CreateOrder& o) { o.price = 10000; }, "strictly between"));
  CHECK(refuse([](CreateOrder& o) { o.client_order_id = ""; }, "client_order_id"));
  CHECK(refuse([](CreateOrder& o) { o.client_order_id = "M 1"; }, "client_order_id"));
  CHECK(refuse([](CreateOrder& o) { o.exchange_index = -1; }, "names its shard"));
  CHECK(refuse([](CreateOrder& o) { o.subaccount = 64; }, "subaccount"));
  CHECK(refuse([](CreateOrder& o) { o.reduce_only = true; }, "reduce_only only with immediate_or_cancel"));
  CHECK(refuse([](CreateOrder& o) { o.tif = TimeInForce::FOK; o.post_only = false; }, "expiration_time only"));
  CHECK(refuse([](CreateOrder& o) { o.tif = TimeInForce::IOC; o.expiration_s = 0; }, "post_only only"));
  CHECK(refuse([](CreateOrder& o) { o.expiration_s = 0; }, "no GTC without an expiration_time"));
  CHECK(refuse([](CreateOrder& o) { o.order_group_id = "g/1"; }, "order_group_id"));

  // intents in venue terms (YES terms both sides; the price is always the YES price)
  PlaceIntent in;
  in.cid = "M-7";
  in.owner = Owner::MAN;
  in.ticker = fx::T;
  in.side = Side::SELL;
  in.price = 6100;
  in.count = 500;
  in.tif = Tif::POST;
  in.ttl_s = 15;
  const CreateOrder c = from_intent(in, 0, 1791300600999);
  CHECK(c.side == BookSide::ASK && c.price == 6100 && c.count == 500 && c.tif == TimeInForce::GTC && c.post_only &&
        c.cancel_on_pause && c.expiration_s == 1791300615 && c.stp == Stp::MAKER && c.client_order_id == "M-7" &&
        check_create(c).empty());
  in.exp_ms = 1791303000000;   // "until the start": an absolute second
  CHECK(from_intent(in, 0, 1791300600999).expiration_s == 1791303000);
  in.owner = Owner::SYS;
  in.side = Side::BUY;
  in.tif = Tif::IOC;
  in.reduce_only = true;
  const CreateOrder t = from_intent(in, 2, 1791300600999);
  CHECK(t.side == BookSide::BID && t.tif == TimeInForce::IOC && !t.post_only && t.expiration_s == 0 &&
        t.stp == Stp::TAKER_AT_CROSS && t.reduce_only && t.exchange_index == 2 && check_create(t).empty());
  in.tif = Tif::FOK;
  in.reduce_only = false;
  CHECK(from_intent(in, 0, 0).tif == TimeInForce::FOK);

  // cancels
  CHECK(build_cancel("ord-1", 2, fx::G, -1, &r, &why));
  CHECK(r.method == "DELETE" && r.path == "/trade-api/v2/portfolio/events/orders/ord-1" &&
        r.query == "exchange_index=2&market_ticker=KXNATGAS15M-26OCT061145-45" && r.write && r.op == WireOp::CANCEL);
  CHECK(build_cancel("ord-1", 0, "", 3, &r, &why) && r.query == "exchange_index=0&subaccount=3");
  CHECK(!build_cancel("ord/1", 0, "", -1, &r, &why) && !build_cancel("ord-1", -1, "", -1, &r, &why));
  std::vector<BatchCancelItem> items = {{"a", 0, fx::T, -1}, {"b", 2, "", 0}};
  CHECK(build_batch_cancel(items, &r, &why));
  CHECK(r.method == "DELETE" && r.path == "/trade-api/v2/portfolio/events/orders/batched" &&
        r.op == WireOp::BATCH_CANCEL &&
        json::parse(r.body) == json::parse(R"({"orders":[{"order_id":"a","exchange_index":0,
        "market_ticker":"KXTEST-26OCT06-A"},{"order_id":"b","exchange_index":2,"subaccount":0}]})"));
  CHECK(!build_batch_cancel({}, &r, &why));
  CHECK(!build_batch_cancel(std::vector<BatchCancelItem>(21, BatchCancelItem{"a", 0, "", -1}), &r, &why));
  CHECK(!build_batch_cancel({{"a", -1, "", -1}}, &r, &why));
  const WireRequest all = build_cancel_all();
  CHECK(all.method == "DELETE" && all.path == "/trade-api/v2/portfolio/events/orders" && all.query.empty() &&
        all.op == WireOp::CANCEL_ALL && all.write);
  CHECK(build_cancel_all(0).query == "subaccount=0");

  // reads
  ListQuery q;
  q.ticker = fx::T;
  q.status = "resting";
  q.min_ts = 1791300000;
  q.limit = 1000;
  q.cursor = "abc+/=";
  q.exchange_index = 0;
  CHECK(build_get_orders(q).target() ==
        "/trade-api/v2/portfolio/orders?ticker=KXTEST-26OCT06-A&status=resting&min_ts=1791300000&limit=1000&"
        "cursor=abc%2B%2F%3D&exchange_index=0");
  q.order_id = "o-1";
  CHECK(build_get_fills(q).target() ==
        "/trade-api/v2/portfolio/fills?ticker=KXTEST-26OCT06-A&order_id=o-1&min_ts=1791300000&limit=1000&"
        "cursor=abc%2B%2F%3D&exchange_index=0");
  ListQuery pq;
  pq.count_filter = "position";
  pq.settlement_status = "unsettled";
  CHECK(build_get_positions(pq).target() ==
        "/trade-api/v2/portfolio/positions?count_filter=position&settlement_status=unsettled");
  CHECK(build_get_balance().target() == "/trade-api/v2/portfolio/balance" &&
        build_get_balance(2, 0).target() == "/trade-api/v2/portfolio/balance?subaccount=0&exchange_index=2");
  CHECK(build_get_order("o-1").path == "/trade-api/v2/portfolio/orders/o-1" && build_get_order("o-1").auth &&
        !build_get_order("o-1").write);
  MarketsQuery mq;
  mq.limit = 200;
  mq.status = "open";
  mq.series_ticker = "KXNATGAS15M";
  const WireRequest mk = build_get_markets(mq);
  CHECK(mk.target() == "/trade-api/v2/markets?limit=200&series_ticker=KXNATGAS15M&status=open" && !mk.auth);
  CHECK(build_get_orderbook(fx::T, 5).target() == "/trade-api/v2/markets/KXTEST-26OCT06-A/orderbook?depth=5");
  CHECK(build_get_market(fx::T).path == "/trade-api/v2/markets/KXTEST-26OCT06-A" && !build_get_market(fx::T).auth);
  CHECK(build_get_exchange_status().path == "/trade-api/v2/exchange/status");

  // the bytes: auth headers, Accept, Content-Type with a body, Connection: close for a write
  build_create(post_order(), &r, &why);
  const std::string bytes = request_bytes(r, "demo-api.kalshi.co", {{"KALSHI-ACCESS-KEY", "kid"}});
  CHECK(bytes.rfind("POST /trade-api/v2/portfolio/events/orders HTTP/1.1\r\n", 0) == 0);
  CHECK(bytes.find("\r\nHost: demo-api.kalshi.co\r\nKALSHI-ACCESS-KEY: kid\r\nAccept: application/json\r\n") !=
        std::string::npos);
  CHECK(bytes.find("Content-Type: application/json\r\n") != std::string::npos &&
        bytes.find("Connection: close\r\n\r\n{") != std::string::npos);
  const std::string rb = request_bytes(build_get_balance(), "demo-api.kalshi.co", {});
  CHECK(rb.find("Connection: keep-alive") != std::string::npos && rb.find("Content-Type") == std::string::npos);
}

// ---- parsers: the documented shapes --------------------------------------------------------------

static void test_parsers() {
  std::string why;
  auto a = parse_create_ack(R"({"order_id":"ee7d","client_order_id":"M-1","fill_count":"2.00","remaining_count":"0.00",
      "average_fill_price":"0.4200","average_fee_paid":"0.017400","ts_ms":1791300600123})", &why);
  CHECK(a && a->order_id == "ee7d" && a->client_order_id == "M-1" && a->fill_count == 200 && a->remaining == 0 &&
        a->avg_fill_price == 420000 && a->avg_fee == 17400 && a->ts_ms == 1791300600123);
  auto a0 = parse_create_ack(R"({"order_id":"x","fill_count":"0.00","remaining_count":"5.00","ts_ms":1})", &why);
  CHECK(a0 && !a0->avg_fill_price && !a0->avg_fee && a0->client_order_id.empty());   // absent, not 0
  CHECK(!parse_create_ack(R"({"order_id":"x","remaining_count":"5.00","ts_ms":1})", &why) &&
        why == "create.fill_count: missing");
  CHECK(!parse_create_ack(R"({"order_id":"x","fill_count":2,"remaining_count":"5.00","ts_ms":1})", &why) &&
        why == "create.fill_count: not a string");
  CHECK(!parse_create_ack(R"({"order_id":"x","fill_count":"2.001","remaining_count":"5.00","ts_ms":1})", &why) &&
        why.find("not a count") != std::string::npos);
  CHECK(!parse_create_ack("<html>", &why) && why == "not JSON");
  CHECK(!parse_create_ack("[]", &why) && why == "not a JSON object");

  auto c = parse_cancel_ack(R"({"order_id":"o","client_order_id":"M-2","reduced_by":"10.00","ts_ms":7})", &why);
  CHECK(c && c->reduced_by == 1000 && c->ts_ms == 7 && c->client_order_id == "M-2");
  auto bc = parse_batch_cancel(R"({"orders":[{"order_id":"a","client_order_id":"M-1","reduced_by":"1.00","ts_ms":5,
      "error":null},{"order_id":"b","client_order_id":null,"reduced_by":"0","ts_ms":null,
      "error":{"code":"not_found","message":"order not found"}}]})", &why);
  CHECK(bc && bc->size() == 2 && (*bc)[0].reduced_by == 100 && (*bc)[0].ts_ms == 5 && !(*bc)[0].error &&
        (*bc)[1].error && (*bc)[1].error->code == "not_found" && !(*bc)[1].ts_ms && (*bc)[1].reduced_by == 0);
  CHECK(bc && classify_refusal(WireOp::BATCH_CANCEL, 404, (*bc)[1].error->text()) == RejectClass::GONE);

  const std::string order = R"({"order_id":"o-1","user_id":"u","client_order_id":"M-1","ticker":"KXTEST-26OCT06-A",
      "outcome_side":"yes","book_side":"bid","type":"limit","status":"resting","yes_price_dollars":"0.3000",
      "no_price_dollars":"0.7000","fill_count_fp":"0.00","remaining_count_fp":"1.00","initial_count_fp":"1.00",
      "taker_fees_dollars":"0.0000","maker_fees_dollars":"0.0000","taker_fill_cost_dollars":"0.0000",
      "maker_fill_cost_dollars":"0.0000","expiration_time":"2026-10-06T15:30:15Z",
      "created_time":"2026-10-06T15:30:00.123Z","last_update_time":"2026-10-06T15:30:00.123Z",
      "self_trade_prevention_type":"maker","order_group_id":null,"cancel_order_on_pause":true,
      "subaccount_number":0,"exchange_index":0})";
  auto o = parse_order("{\"order\":" + order + "}", &why);
  CHECK(o && o->order_id == "o-1" && o->status == OrderStatus::RESTING && o->side == BookSide::BID &&
        o->yes_price == 3000 && o->remaining == 100 && o->initial == 100 && o->filled == 0 &&
        o->expiration_ms == 1791300615000 && o->created_ms == 1791300600123 && o->stp == Stp::MAKER &&
        o->order_group_id.empty() && o->cancel_on_pause && o->subaccount == 0 && o->exchange_index == 0);
  json no_side = json::parse(order);
  no_side.erase("book_side");
  CHECK(!parse_order(json{{"order", no_side}}.dump(), &why) && why == "order.book_side: missing");
  json bad_status = json::parse(order);
  bad_status["status"] = "pending";
  CHECK(!parse_order(json{{"order", bad_status}}.dump(), &why) && why.find("order.status") == 0);
  json minimal = json::parse(order);
  for (const char* k : {"expiration_time", "created_time", "last_update_time", "self_trade_prevention_type",
                        "order_group_id", "cancel_order_on_pause", "subaccount_number", "exchange_index"})
    minimal.erase(k);
  auto om = parse_order(json{{"order", minimal}}.dump(), &why);
  CHECK(om && !om->expiration_ms && !om->exchange_index && !om->subaccount && !om->stp);
  auto page = parse_orders("{\"orders\":[" + order + "," + order + "],\"cursor\":\"next\"}", &why);
  CHECK(page && page->rows.size() == 2 && page->cursor == "next");
  CHECK(parse_orders(R"({"orders":[],"cursor":""})", &why)->rows.empty());
  CHECK(!parse_orders(R"({"cursor":""})", &why));
  json bad_row = json::parse(order);
  bad_row["yes_price_dollars"] = "0.30005";
  CHECK(!parse_orders("{\"orders\":[" + order + "," + bad_row.dump() + "],\"cursor\":\"\"}", &why) &&
        why.find("orders[1].yes_price_dollars") == 0);

  auto fills = parse_fills(R"({"fills":[{"fill_id":"f1","exchange_index":0,"trade_id":"f1","order_id":"o-1",
      "ticker":"KXTEST-26OCT06-A","market_ticker":"KXTEST-26OCT06-A","outcome_side":"yes","book_side":"bid",
      "count_fp":"2.00","yes_price_dollars":"0.4200","no_price_dollars":"0.5800","is_taker":true,
      "created_time":"2026-10-06T15:30:00.5Z","fee_cost":"0.034000","subaccount_number":0,"ts":1791300600},
      {"fill_id":"f2","exchange_index":2,"trade_id":"f2","order_id":"o-2","ticker":"G","market_ticker":"G",
      "outcome_side":"no","book_side":"ask","count_fp":"1.00","yes_price_dollars":"0.9950","no_price_dollars":"0.0050",
      "is_taker":false,"fee_cost":"0.000000","ts":1791300601}],"cursor":""})", &why);
  CHECK(fills && fills->rows.size() == 2);
  if (fills && fills->rows.size() == 2) {
    const FillRow& f = fills->rows[0];
    CHECK(f.count == 200 && f.yes_price == 4200 && f.taker && f.fee == 34000 && f.ts_ms == 1791300600500 &&
          f.side == BookSide::BID && f.subaccount == 0);
    const FillRow& g = fills->rows[1];
    CHECK(g.side == BookSide::ASK && g.yes_price == 9950 && !g.taker && g.ts_ms == 1791300601000 && !g.subaccount &&
          g.exchange_index == 2);
  }
  auto pos = parse_positions(R"({"market_positions":[{"ticker":"KXTEST-26OCT06-A","exchange_index":0,
      "total_traded_dollars":"1.2600","position_fp":"-3.00","market_exposure_dollars":"1.7400",
      "realized_pnl_dollars":"-0.0200","fees_paid_dollars":"0.051000","last_updated_ts":"2026-10-06T15:30:00Z"}],
      "event_positions":[],"cursor":""})", &why);
  CHECK(pos && pos->rows.size() == 1 && pos->rows[0].position == -300 && pos->rows[0].exposure == 1740000 &&
        pos->rows[0].realized == -20000 && pos->rows[0].fees == 51000 && pos->rows[0].updated_ms == 1791300600000);
  auto bal = parse_balance(R"({"balance":12345,"balance_dollars":"123.4500","portfolio_value":500,
      "updated_ts":1791300600,"balance_breakdown":[{"exchange_index":0,"balance":"100.0000"},
      {"exchange_index":2,"balance":"23.4500"}]})", &why);
  CHECK(bal && bal->available == 1234500 && bal->portfolio_value == 50000 && bal->updated_ts == 1791300600 &&
        bal->by_shard.size() == 2 && bal->by_shard[1].first == 2 && bal->by_shard[1].second == 234500);
  auto bal_cents = parse_balance(R"({"balance":0,"portfolio_value":0,"updated_ts":1})", &why);
  CHECK(bal_cents && bal_cents->available == 0 && bal_cents->by_shard.empty());
  CHECK(!parse_balance(R"({"balance_dollars":"1.0000","updated_ts":1})", &why) && why == "balance.balance: missing");

  const std::string market = R"({"ticker":"KXNATGAS15M-26OCT061145-45","event_ticker":"KXNATGAS15M-26OCT061145",
      "market_type":"binary","status":"active","exchange_index":2,"yes_bid_dollars":"0.4500","yes_ask_dollars":"0.4700",
      "no_bid_dollars":"0.5300","no_ask_dollars":"0.5500","last_price_dollars":"0.4600","yes_bid_size_fp":"30.00",
      "yes_ask_size_fp":"12.00","volume_fp":"1234.00","open_interest_fp":"800.00",
      "close_time":"2026-10-06T15:45:00Z","price_level_structure":"tapered_deci_cent",
      "price_ranges":[{"start":"0.0000","end":"0.1000","step":"0.0010"},{"start":"0.1000","end":"0.9000",
      "step":"0.0100"},{"start":"0.9000","end":"1.0000","step":"0.0010"}],"rules_primary":"..."})";
  auto m = parse_market("{\"market\":" + market + "}", &why);
  CHECK(m && m->exchange_index == 2 && m->yes_bid == 4500 && m->yes_ask == 4700 && m->yes_bid_size == 3000 &&
        m->close_ms == 1791301500000 && m->price_ranges.size() == 3 && m->price_ranges[0].step == 10 &&
        m->price_ranges[1].step == 100);
  CHECK(m && !m->fractional);   // absent: whole contracts only
  auto mf = parse_market(R"({"market":{"ticker":"T","status":"active","fractional_trading_enabled":true}})", &why);
  CHECK(mf && mf->fractional && *mf->fractional);
  CHECK(!parse_market(R"({"market":{"ticker":"T","status":"active","fractional_trading_enabled":"yes"}})", &why));
  auto ms = parse_markets("{\"markets\":[" + market + "],\"cursor\":\"c2\"}", &why);
  CHECK(ms && ms->rows.size() == 1 && ms->cursor == "c2");
  auto ob = parse_orderbook(R"({"orderbook_fp":{"yes_dollars":[["0.3900","10.00"],["0.4000","5.00"]],
      "no_dollars":[["0.5800","3.00"]]}})", &why);
  CHECK(ob && ob->yes.size() == 2 && ob->yes.back().first == 4000 && ob->yes.back().second == 500 && ob->no.size() == 1);
  CHECK(parse_orderbook(R"({"orderbook_fp":{"yes_dollars":[],"no_dollars":null}})", &why)->no.empty());
  CHECK(!parse_orderbook(R"({"orderbook_fp":{"yes_dollars":[["0.39"]]}})", &why));
  auto st = parse_exchange_status(R"({"exchange_active":true,"trading_active":false})", &why);
  CHECK(st && st->exchange_active && !st->trading_active);

  // errors: the venue's shape, the 429's, and bodies from something in between
  ApiError e = parse_error(400, R"({"error":{"code":"invalid_order","message":"invalid order","details":"post only cross"}})");
  CHECK(e.venue_body && e.code == "invalid_order" && e.details == "post only cross");
  CHECK(parse_error(429, R"({"error":"too many requests"})").venue_body);
  CHECK(!parse_error(400, R"({"error":"too many requests"})").venue_body);
  CHECK(!parse_error(502, "<html>bad gateway</html>").venue_body);
  CHECK(!parse_error(400, R"({"message":"x"})").venue_body);
}

// The replies demo-api.kalshi.co sent tools/kalshi_demo_probe on 2026-10-07, as sent (the
// account's user_id replaced, the markets page cut to its first three; tests/deskd/fixtures/
// kalshi_demo_*.json), read by the same parsers. What they taught: a field with no value is left
// out rather than null; an order never echoes cancel_order_on_pause; a cancel's reply has no
// client_order_id; the deprecated side and action are still sent; the status lists every shard.
static void test_demo_shapes() {
  const std::string dir = std::string(DESKD_SOURCE_DIR) + "/tests/deskd/fixtures/";
  auto read = [&](const char* name) {
    std::ifstream in(dir + name);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  std::string why;
  auto bal = parse_balance(read("kalshi_demo_balance.json"), &why);
  CHECK(bal && bal->available == 500 * kOneDollar && bal->portfolio_value == 0 && bal->by_shard.size() == 4 &&
        bal->by_shard[0].first == 0 && bal->by_shard[0].second == 500 * kOneDollar);
  auto st = parse_exchange_status(read("kalshi_demo_exchange_status.json"), &why);
  CHECK(st && st->exchange_active && st->trading_active && st->shards.size() == 4 && st->shards[3].exchange_index == 3 &&
        st->shards[1].transfers_active == true);
  auto m = parse_markets(read("kalshi_demo_markets.json"), &why);
  CHECK(m && m->rows.size() == 3 && !m->cursor.empty() && m->rows[2].exchange_index == 0 &&
        m->rows[2].price_ranges.size() == 1 && m->rows[2].price_ranges[0].step == 100 && m->rows[2].close_ms);
  if (!m) std::fprintf(stderr, "  demo markets: %s\n", why.c_str());
  auto ack = parse_create_ack(read("kalshi_demo_create.json"), &why);
  CHECK(ack && ack->fill_count == 0 && ack->remaining == 100 && ack->client_order_id.rfind("P-", 0) == 0 &&
        !ack->avg_fill_price && ack->ts_ms > 1791300000000);
  CHECK(classify_write(WireOp::CREATE, replied(201, read("kalshi_demo_create.json"))).kind == K::ACK);
  auto o = parse_order(read("kalshi_demo_order.json"), &why);
  CHECK(o && o->status == OrderStatus::RESTING && o->yes_price == 100 && o->remaining == 100 && o->initial == 100 &&
        o->side == BookSide::BID && o->stp == Stp::MAKER && !o->cancel_on_pause && o->order_group_id.empty() &&
        o->exchange_index == 0 && o->subaccount == 0 && o->expiration_ms && o->created_ms &&
        (*o->expiration_ms - *o->created_ms) / 1000 == 14);   // sent with expiration now + 15 s, whole seconds
  auto oc = parse_order(read("kalshi_demo_order_canceled.json"), &why);
  CHECK(oc && oc->status == OrderStatus::CANCELED && oc->remaining == 0 && oc->filled == 0);
  auto c = parse_cancel_ack(read("kalshi_demo_cancel.json"), &why);
  CHECK(c && c->reduced_by == 100 && c->client_order_id.empty() && o && c->order_id == o->order_id);
  CHECK(classify_write(WireOp::CANCEL, replied(200, read("kalshi_demo_cancel.json"))).kind == K::ACK);
  // the grid demo used for its multivariate markets, 0.0001 steps at the edges: whole units in
  // deskd's 1/10,000 dollar (the markets page of 2026-10-07, before mve_filter=exclude)
  auto mve = parse_market(R"({"market":{"ticker":"KXMVECROSSCATEGORY-S2026CC84B06EBDE-1F310C3F174","status":"active",
      "exchange_index":1,"price_level_structure":"center_deci_edge_centi_cent","price_ranges":[{"end":"0.0100",
      "start":"0.0000","step":"0.0001"},{"end":"0.9900","start":"0.0100","step":"0.0010"},{"end":"1.0000",
      "start":"0.9900","step":"0.0001"}],"yes_bid_dollars":"0.0000","yes_ask_dollars":"1.0000"}})", &why);
  CHECK(mve && mve->price_ranges.size() == 3 && mve->price_ranges[0].step == 1 && mve->price_ranges[1].step == 10 &&
        mve->exchange_index == 1);
}

// ---- what a write's reply means -----------------------------------------------------------------

static void test_classify() {
  net::Exchange ns;
  ns.fate = net::Exchange::Fate::NOT_SENT;
  ns.why = "closed before the write";
  CHECK(classify_write(WireOp::CREATE, ns).kind == K::NOT_SENT);
  net::Exchange un;
  un.fate = net::Exchange::Fate::UNKNOWN;
  un.why = "body cut at 50 of 109 bytes";
  CHECK(classify_write(WireOp::CREATE, un).kind == K::UNKNOWN && classify_write(WireOp::CREATE, un).why == un.why);
  const std::string ack = R"({"order_id":"x","fill_count":"0.00","remaining_count":"1.00","ts_ms":1})";
  CHECK(classify_write(WireOp::CREATE, replied(201, ack)).kind == K::ACK);
  CHECK(classify_write(WireOp::CREATE, replied(201, "{\"order_id\":\"x\"}")).kind == K::UNKNOWN);   // took it, unreadable
  CHECK(classify_write(WireOp::CREATE, replied(200, ack)).kind == K::UNKNOWN);   // create answers 201
  CHECK(classify_write(WireOp::CREATE, replied(201, "")).kind == K::UNKNOWN);
  for (int s : {500, 502, 503, 504, 408})
    CHECK(classify_write(WireOp::CREATE, replied(s, R"({"error":{"code":"x","message":"y"}})")).kind == K::UNKNOWN);
  CHECK(classify_write(WireOp::CREATE, replied(302, "")).kind == K::UNKNOWN);
  CHECK(classify_write(WireOp::CREATE, replied(400, "<html>400 Bad Request</html>")).kind == K::UNKNOWN);
  CHECK(classify_write(WireOp::CREATE, replied(403, "Forbidden")).kind == K::UNKNOWN);
  auto refused = [](WireOp op, int st, const std::string& body) {
    const WriteOutcome w = classify_write(op, replied(st, body));
    return w.kind == K::REFUSED ? w.cls : static_cast<RejectClass>(-1);
  };
  auto err = [](const char* code, const char* msg, const char* details = "") {
    json e{{"code", code}, {"message", msg}};
    if (*details) e["details"] = details;
    return json{{"error", e}}.dump();
  };
  CHECK(refused(WireOp::CREATE, 400, err("invalid_order", "invalid order", "post only cross")) == RejectClass::SOFT_MARKET);
  CHECK(refused(WireOp::CREATE, 400, err("market_closed", "market is closed")) == RejectClass::SOFT_MARKET);
  CHECK(refused(WireOp::CREATE, 400, err("trading_paused", "trading is paused")) == RejectClass::SOFT_MARKET);
  CHECK(refused(WireOp::CREATE, 400, err("insufficient_balance", "insufficient balance")) == RejectClass::SOFT_COLLATERAL);
  CHECK(refused(WireOp::CREATE, 400, err("invalid_order", "invalid order", "order group triggered")) == RejectClass::SOFT_GROUP);
  CHECK(refused(WireOp::CREATE, 400, err("self_trade", "order would self trade")) == RejectClass::SOFT_SELF_TRADE);
  CHECK(refused(WireOp::CREATE, 409, err("order_already_exists", "an order with this client_order_id already exists")) ==
        RejectClass::DUPLICATE);
  CHECK(refused(WireOp::CREATE, 400, err("invalid_order", "invalid order", "order group not found")) ==
        RejectClass::HARD_INVALID);
  CHECK(refused(WireOp::CREATE, 400, err("invalid_parameters", "count must be positive")) == RejectClass::HARD_INVALID);
  CHECK(refused(WireOp::CREATE, 404, err("not_found", "market not found")) == RejectClass::HARD_INVALID);
  CHECK(refused(WireOp::CANCEL, 404, err("not_found", "order not found")) == RejectClass::GONE);
  CHECK(refused(WireOp::CREATE, 401, err("authentication_error", "signature does not verify")) == RejectClass::AUTH);
  CHECK(refused(WireOp::CREATE, 403, err("forbidden", "no trade permission")) == RejectClass::AUTH);
  CHECK(refused(WireOp::CREATE, 429, R"({"error":"too many requests"})") == RejectClass::RETRYABLE);
  CHECK(refused(WireOp::CREATE, 429, "slow down") == RejectClass::RETRYABLE);
  CHECK(refused(WireOp::CREATE, 418, err("teapot", "unclassified")) == RejectClass::UNKNOWN);
  CHECK(reject_trips_kill(RejectClass::UNKNOWN) && reject_trips_kill(RejectClass::HARD_INVALID) &&
        !reject_trips_kill(RejectClass::SOFT_SELF_TRADE) && !reject_trips_kill(RejectClass::SOFT_GROUP) &&
        !reject_trips_kill(RejectClass::DUPLICATE) && !reject_trips_kill(RejectClass::GONE));
  // a bare "post_only" in a validation error is the desk's mistake, not the market moving
  CHECK(refused(WireOp::CREATE, 400, err("invalid_parameters", "post_only requires good_till_canceled")) ==
        RejectClass::HARD_INVALID);
  CHECK(classify_write(WireOp::CANCEL, replied(200, R"({"order_id":"o","reduced_by":"1.00","ts_ms":2})")).kind == K::ACK);
  CHECK(classify_write(WireOp::CANCEL_ALL, replied(204, "")).kind == K::ACK);
  CHECK(classify_write(WireOp::BATCH_CANCEL, replied(200, R"({"orders":[]})")).kind == K::ACK);
  CHECK(classify_write(WireOp::BATCH_CANCEL, replied(200, R"({"orders":{}})")).kind == K::UNKNOWN);
}

// ---- round trips against the fake over TLS ------------------------------------------------------

namespace {
struct Rt {
  fx::Venue& v;
  ConnFactory f;
  SparePool pool;
  const KalshiSigner* signer;
  Rt(fx::Venue& venue, const KalshiSigner* s)
      : v(venue), f(fx::demo_guard(), fx::trust_test_ca()), pool(f, cfg(venue)), signer(s) {
    pool.start();
    pool.wait_ready(1, 3000);
  }
  static PoolConfig cfg(fx::Venue& venue) {
    PoolConfig c;
    c.name = "rt";
    c.endpoint = venue.ep();
    c.spares = 2;
    return c;
  }
  net::Exchange send(const WireRequest& r, int deadline_ms = 3000, std::int64_t ts = 0) {
    auto c = pool.take(2000);
    if (!c) return net::Exchange{};
    std::vector<std::pair<std::string, std::string>> h;
    if (r.auth && signer) h = signer->headers(ts ? ts : wall_ms(), r.method, r.path);
    return net::exchange(*c, request_bytes(r, c->endpoint().host_header(), h), fx::deadline_ms(deadline_ms));
  }
  WriteOutcome write(const WireRequest& r, net::Exchange* out = nullptr) {
    net::Exchange x = send(r);
    if (out) *out = x;
    return classify_write(r.op, x);
  }
  std::optional<CreateAck> create(const CreateOrder& o, WriteOutcome* w = nullptr) {
    WireRequest r;
    std::string why;
    if (!build_create(o, &r, &why)) return std::nullopt;
    net::Exchange x;
    const WriteOutcome out = write(r, &x);
    if (w) *w = out;
    if (out.kind != K::ACK) return std::nullopt;
    return parse_create_ack(x.res.body, &why);
  }
};

CreateOrder order_on(const std::string& ticker, int shard, BookSide side, Qty count, Px price, TimeInForce tif,
                     const std::string& cid) {
  CreateOrder o;
  o.ticker = ticker;
  o.exchange_index = shard;
  o.side = side;
  o.count = count;
  o.price = price;
  o.tif = tif;
  o.client_order_id = cid;
  o.stp = Stp::TAKER_AT_CROSS;
  if (tif == TimeInForce::GTC) {
    o.post_only = true;
    o.cancel_on_pause = true;
    o.expiration_s = wall_ms() / 1000 + 60;
  }
  return o;
}
}  // namespace

static void round_trip(const std::string& key_type) {
  std::string err;
  auto signer = KalshiSigner::from_pem("rt-key", key_type == "rsa" ? new_key_pem("RSA", 2048) : new_key_pem("ED25519"),
                                       key_type, &err);
  CHECK(signer != nullptr);
  if (!signer) return;
  fakex::FakeConfig cfg;
  cfg.auth = fakex::AuthMode::VERIFY;
  cfg.public_key_pem = signer->public_pem();
  cfg.auth_max_skew_ms = 5000;
  fx::Venue v(cfg);
  CHECK(v.ok);
  Rt rt(v, signer.get());
  std::string why;

  // a post-only bid rests; GET shows it as sent
  const CreateOrder post = order_on(fx::T, 0, BookSide::BID, 100, 3000, TimeInForce::GTC, "M-rt-post");
  WriteOutcome w;
  auto ack = rt.create(post, &w);
  CHECK(ack && ack->fill_count == 0 && ack->remaining == 100 && ack->client_order_id == "M-rt-post" && !ack->avg_fill_price);
  if (!ack) {
    std::fprintf(stderr, "  %s create: %s %s\n", key_type.c_str(), write_kind_name(w.kind), w.why.c_str());
    return;
  }
  net::Exchange g = rt.send(build_get_order(ack->order_id));
  auto o = parse_order(g.res.body, &why);
  CHECK(o && o->order_id == ack->order_id && o->status == OrderStatus::RESTING && o->yes_price == 3000 &&
        o->remaining == 100 && o->initial == 100 && o->side == BookSide::BID && o->stp == Stp::TAKER_AT_CROSS &&
        !o->cancel_on_pause /* never echoed */ && o->expiration_ms == post.expiration_s * 1000 && o->exchange_index == 0 &&
        o->client_order_id == "M-rt-post");
  ListQuery q;
  q.status = "resting";
  q.ticker = fx::T;
  auto page = parse_orders(rt.send(build_get_orders(q)).res.body, &why);
  CHECK(page && page->rows.size() == 1 && page->rows[0].order_id == ack->order_id);

  // an IOC bid takes 2 of the street's 3 at 0.42; the 201 carries the fill
  auto take = rt.create(order_on(fx::T, 0, BookSide::BID, 200, 4300, TimeInForce::IOC, "M-rt-take"));
  CHECK(take && take->fill_count == 200 && take->remaining == 0 && take->avg_fill_price == 420000 && take->avg_fee &&
        *take->avg_fee > 0);
  ListQuery fq;
  fq.ticker = fx::T;
  auto fills = parse_fills(rt.send(build_get_fills(fq)).res.body, &why);
  CHECK(fills && fills->rows.size() == 1 && fills->rows[0].count == 200 && fills->rows[0].yes_price == 4200 &&
        fills->rows[0].taker && fills->rows[0].fee > 0 && take && fills->rows[0].order_id == take->order_id);
  // reduce-only IOC ask sells 1 back into the street's 0.40 bid
  CreateOrder flat = order_on(fx::T, 0, BookSide::ASK, 100, 4000, TimeInForce::IOC, "M-rt-flat");
  flat.reduce_only = true;
  auto fl = rt.create(flat);
  CHECK(fl && fl->fill_count == 100);
  auto pos = parse_positions(rt.send(build_get_positions(ListQuery{})).res.body, &why);
  CHECK(pos && pos->rows.size() == 1 && pos->rows[0].ticker == fx::T && pos->rows[0].position == 100);

  // cancel: reduced_by is what was resting; the order then reads canceled; a second cancel is GONE
  WireRequest cr;
  CHECK(build_cancel(ack->order_id, 0, fx::T, -1, &cr, &why));
  net::Exchange cx;
  const WriteOutcome cw = rt.write(cr, &cx);
  auto cack = parse_cancel_ack(cx.res.body, &why);
  CHECK(cw.kind == K::ACK && cack && cack->reduced_by == 100 && cack->order_id == ack->order_id);
  CHECK(parse_order(rt.send(build_get_order(ack->order_id)).res.body, &why)->status == OrderStatus::CANCELED);
  const WriteOutcome again = rt.write(cr);
  CHECK(again.kind == K::REFUSED && again.cls == RejectClass::GONE && again.http == 404);

  // batch cancel: three resting orders and one that does not exist
  std::vector<BatchCancelItem> items;
  for (int i = 0; i < 3; ++i) {
    auto a = rt.create(order_on(fx::T, 0, BookSide::BID, 100, 2000 + i * 100, TimeInForce::GTC, "M-rt-b" + std::to_string(i)));
    CHECK(a.has_value());
    if (a) items.push_back({a->order_id, 0, fx::T, -1});
  }
  items.push_back({"00000000-0000-4000-8000-000000000000", 0, fx::T, -1});
  WireRequest br;
  CHECK(build_batch_cancel(items, &br, &why));
  net::Exchange bx;
  CHECK(rt.write(br, &bx).kind == K::ACK);
  auto rows = parse_batch_cancel(bx.res.body, &why);
  CHECK(rows && rows->size() == 4 && (*rows)[0].reduced_by == 100 && !(*rows)[0].error && (*rows)[3].error &&
        classify_refusal(WireOp::BATCH_CANCEL, 404, (*rows)[3].error->text()) == RejectClass::GONE);

  // the soft refusals
  WriteOutcome sw;
  CHECK(!rt.create(order_on(fx::T, 0, BookSide::BID, 100, 4200, TimeInForce::GTC, "M-rt-cross"), &sw));
  CHECK(sw.kind == K::REFUSED && sw.cls == RejectClass::SOFT_MARKET);
  CHECK(!rt.create(order_on(fx::G, 2, BookSide::BID, 1000000, 5000, TimeInForce::GTC, "M-rt-cash"), &sw));
  CHECK(sw.kind == K::REFUSED && sw.cls == RejectClass::SOFT_COLLATERAL);
  CHECK(rt.create(order_on(fx::T, 0, BookSide::BID, 100, 1000, TimeInForce::GTC, "M-rt-dup")).has_value());
  CHECK(!rt.create(order_on(fx::T, 0, BookSide::BID, 100, 1100, TimeInForce::GTC, "M-rt-dup"), &sw));
  CHECK(sw.kind == K::REFUSED && sw.cls == RejectClass::DUPLICATE && sw.http == 409);
  const std::string group = v.f->with_engine([](fakex::Engine& e, std::int64_t now) {
    const std::string id = e.group_create(0, 0, 1000, now);
    e.group_trigger(id, 0, 0, now);
    return id;
  });
  CreateOrder grouped = order_on(fx::T, 0, BookSide::BID, 100, 1200, TimeInForce::GTC, "M-rt-group");
  grouped.order_group_id = group;
  CHECK(!rt.create(grouped, &sw) && sw.kind == K::REFUSED && sw.cls == RejectClass::SOFT_GROUP);

  // cancel-all: 204
  CHECK(rt.write(build_cancel_all()).kind == K::ACK);
  CHECK(parse_orders(rt.send(build_get_orders(q)).res.body, &why)->rows.empty());

  // balance per shard, markets, the book, the exchange
  auto bal = parse_balance(rt.send(build_get_balance()).res.body, &why);
  CHECK(bal && bal->available < 2000 * 10000 && bal->by_shard.size() == 2);
  auto b2 = parse_balance(rt.send(build_get_balance(2)).res.body, &why);
  CHECK(b2 && b2->available == 1000 * 10000);
  auto mkt = parse_market(rt.send(build_get_market(fx::G)).res.body, &why);
  CHECK(mkt && mkt->exchange_index == 2 && mkt->price_ranges.size() == 3 && mkt->price_level_structure == "tapered_deci_cent");
  CHECK(mkt && mkt->fractional && !*mkt->fractional);   // the fake lists the flag, false by default
  MarketsQuery mq;
  mq.limit = 1;
  auto mp = parse_markets(rt.send(build_get_markets(mq)).res.body, &why);
  CHECK(mp && mp->rows.size() == 1 && !mp->cursor.empty());
  mq.cursor = mp ? mp->cursor : "";
  auto mp2 = parse_markets(rt.send(build_get_markets(mq)).res.body, &why);
  CHECK(mp2 && mp2->rows.size() == 1 && mp2->cursor.empty() && mp && mp2->rows[0].ticker != mp->rows[0].ticker);
  auto book = parse_orderbook(rt.send(build_get_orderbook(fx::T)).res.body, &why);
  CHECK(book && !book->yes.empty() && book->yes.back().first == 4000);
  CHECK(parse_exchange_status(rt.send(build_get_exchange_status()).res.body, &why).has_value());

  // a signature from another key, and a stale timestamp: AUTH, never a soft refusal
  auto other = KalshiSigner::from_pem("rt-key", new_key_pem("ED25519"), "ed25519", &err);
  rt.signer = other.get();
  const WriteOutcome bad_sig = rt.write(build_cancel_all());
  CHECK(bad_sig.kind == K::REFUSED && bad_sig.cls == RejectClass::AUTH && bad_sig.http == 401);
  rt.signer = signer.get();
  const net::Exchange stale = rt.send(build_get_balance(), 3000, wall_ms() - 60000);
  CHECK(stale.res.status == 401);

  // the faults that leave a write UNKNOWN (and one the venue never took)
  using FK = fakex::HttpFault::Kind;
  auto faulted = [&](FK k, const std::string& cid) {
    fakex::HttpFault h;
    h.kind = k;
    v.f->add_fault(h);
    WriteOutcome out;
    rt.create(order_on(fx::T, 0, BookSide::BID, 100, 1500, TimeInForce::GTC, cid), &out);
    return out;
  };
  CHECK(faulted(FK::STATUS_AFTER_ACCEPT, "M-rt-500").kind == K::UNKNOWN && v.has_cid("M-rt-500"));
  CHECK(faulted(FK::CUT_BODY, "M-rt-cut").kind == K::UNKNOWN && v.has_cid("M-rt-cut"));
  CHECK(faulted(FK::DROP_AFTER_ACCEPT, "M-rt-drop").kind == K::UNKNOWN && v.has_cid("M-rt-drop"));
  CHECK(faulted(FK::DROP_BEFORE_REPLY, "M-rt-lost").kind == K::UNKNOWN && !v.has_cid("M-rt-lost"));
  CHECK(faulted(FK::CHUNKED, "M-rt-chunk").kind == K::ACK);
  const WriteOutcome limited = faulted(FK::RATE_LIMIT, "M-rt-429");
  CHECK(limited.kind == K::REFUSED && limited.cls == RejectClass::RETRYABLE && !v.has_cid("M-rt-429"));

  // every request rode its own connection
  const PoolStats ps = rt.pool.stats();
  CHECK(ps.takes == v.f->http_requests() && ps.hits + ps.misses == ps.takes);
  std::printf("round trip (%s): %llu requests, %llu handshakes, %llu misses\n", key_type.c_str(),
              static_cast<unsigned long long>(ps.takes), static_cast<unsigned long long>(ps.handshakes),
              static_cast<unsigned long long>(ps.misses));
}

// ---- the demo probe binary against the fake -----------------------------------------------------
// tools/kalshi_demo_probe through its real command line: a key file in ~/.kalshi/demo.json's shape
// naming a key made for this test, the fake over TLS as the venue (its CA trusted for loopback
// only), the handshake figures, the reads, the order check (a post-only bid at $0.01 placed, read
// back, cancelled, read back, gone from the resting list) and the dumps. Its output never carries
// key material, the key id or a signature. A production base, a key file for another environment
// and a key type that does not match the PEM are each refused before any socket opens.
static void test_probe_binary() {
  std::string err;
  const std::string pem = new_key_pem("ED25519");
  auto signer = KalshiSigner::from_pem("probe-key", pem, "ed25519", &err);
  CHECK(signer != nullptr);
  if (!signer) return;
  fakex::FakeConfig cfg;
  cfg.auth = fakex::AuthMode::VERIFY;
  cfg.public_key_pem = signer->public_pem();
  cfg.auth_max_skew_ms = 5000;
  fx::Venue v(cfg);
  CHECK(v.ok);
  TempDir d;
  write_file(d.file("key.pem"), pem);   // made a moment ago for this test: not a credential
  auto key_file = [&](const char* name, const char* env, const char* type) {
    write_file(d.file(name), json{{"env", env}, {"key_id", "probe-key"}, {"private_key_path", d.file("key.pem")},
                                  {"key_type", type}}.dump());
    return d.file(name);
  };
  auto run = [&](const std::string& key, const std::string& args, int* rc) {
    const std::string cmd = std::string(KALSHI_DEMO_PROBE_BIN) + " --key " + key + " --loopback-ca " + fx::pki().p.ca_cert +
                            " " + args + " 2>&1";
    FILE* p = ::popen(cmd.c_str(), "r");
    std::string out;
    if (!p) {
      *rc = -2;
      return out;
    }
    char b[4096];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof b, p)) > 0) out.append(b, n);
    const int st = ::pclose(p);
    *rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
  };
  const std::string demo_key = key_file("demo.json", "demo", "ed25519");
  int rc = -1;
  const std::string out = run(demo_key, "--base " + v.ep().url() + " --handshakes 3 --order-check --order-ticker " + fx::T +
                                            " --dump " + d.path, &rc);
  CHECK(rc == 0);
  if (rc != 0) std::fprintf(stderr, "probe output:\n%s\n", out.c_str());
  auto has = [&](const char* s) { return out.find(s) != std::string::npos; };
  CHECK(has("key: ed25519, id of 9 characters, from "));
  CHECK(has("guard: allowed, loopback"));
  CHECK(has("production check: api.elections.kalshi.com is a production host") && has("(sockets opened: 0)"));
  CHECK(has("resumption on : TLSv1.3") && has("resumption off: TLSv1.3") && has(", 0 resumed"));
  CHECK(has(", 2 resumed") || has(", 1 resumed"));   // the first of three cannot resume
  CHECK(has("exchange_active true, trading_active true"));
  CHECK(has("BALANCE available $2000.0000") && has("shard 0 $1000.0000") && has("shard 2 $1000.0000"));
  CHECK(has("    resting orders: 0\n"));
  CHECK(has("2 markets (1 with a YES bid), no cursor"));
  CHECK(has("post-only bid 1 @ 0.0100, expires in 15 s, cid P-") && has("(best bid 0.4000, best ask 0.4200)"));
  CHECK(has("outcome ACK: order_id ") && has(", filled 0.00, remaining 1.00"));
  CHECK(has("read back resting after"));
  CHECK(has("expiration_time - created_time = 1"));   // 14-15 s: whole seconds
  CHECK(has("outcome ACK: reduced_by 1.00"));
  CHECK(has("read back canceled after"));
  CHECK(has("resting orders: 0 (this check's: 0)"));
  CHECK(has("write->reply") && has("pool: ") && has("sign (ed25519, this key): p50"));
  CHECK(has("\nok\n"));
  // nothing of the key, its id or a signature
  CHECK(!has("BEGIN") && !has("KALSHI-ACCESS") && !has("probe-key") && !has("?"));
  // the fake saw exactly one order from it, post-only at $0.01, cancelled
  CHECK(v.f->with_engine([](fakex::Engine& e, std::int64_t) {
    const auto os = e.user_orders();
    return os.size() == 1 && os[0]->status == fakex::OrdStatus::CANCELED && os[0]->price == 100 && os[0]->post_only &&
           os[0]->initial == 100 && os[0]->client_order_id.rfind("P-", 0) == 0;
  }));
  for (const char* n : {"exchange_status", "balance", "markets", "create", "order", "cancel", "order_canceled"})
    CHECK(std::filesystem::exists(d.file(std::string("kalshi_demo_") + n + ".json")));
  std::printf("probe against the fake: exit %d, %zu bytes of output\n", rc, out.size());

  // a production base: the guard refuses it before any socket, exit 2, no handshake section
  const std::string prod = run(demo_key, "--base https://api.elections.kalshi.com/trade-api/v2 --handshakes 1", &rc);
  CHECK(rc == 2 && prod.find("is a production host") != std::string::npos && prod.find("handshakes (") == std::string::npos);
  // a key file for another environment, and a key type that does not match the PEM
  const std::string wrong_env = run(key_file("prod.json", "prod", "ed25519"), "--base " + v.ep().url(), &rc);
  CHECK(rc == 2 && wrong_env.find("only ever touches the demo exchange") != std::string::npos);
  const std::string wrong_type = run(key_file("rsa.json", "demo", "rsa"), "--base " + v.ep().url(), &rc);
  CHECK(rc == 2 && wrong_type.find("the config says rsa") != std::string::npos);
  CHECK(v.f->connections_accepted() > 0);   // the first run connected; the three refusals added nothing
}

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  CHECK(fx::pki().ok);
  test_signer();
  test_values();
  test_builders();
  test_parsers();
  test_demo_shapes();
  test_classify();
  round_trip("ed25519");
  round_trip("rsa");
  test_probe_binary();
  return finish("deskd-wire");
}
