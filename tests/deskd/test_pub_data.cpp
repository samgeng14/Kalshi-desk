// The publication owner is tested without HTTP or exchange I/O. Captured events and
// snapshots are also checked by the actual frontend contract, not a copied C++ schema.
#include <chrono>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>

#include "check.h"
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/build_paths.h"
#include "deskd/web/pub.h"

using namespace deskd;
using json = nlohmann::json;
using namespace std::chrono_literals;

static json capture = {{"snapshots", json::array()}, {"events", json::array()}, {"responses", json::array()},
                       {"requests", json::array()}, {"queries", json::array()}};

static bool eventually(const std::function<bool()>& fn) {
  const auto until = std::chrono::steady_clock::now() + 2s;
  do { if (fn()) return true; std::this_thread::sleep_for(5ms); } while (std::chrono::steady_clock::now() < until);
  return false;
}

static json market(const std::string& ticker, const std::string& venue = "K") {
  const auto now = now_ms();
  return {{"v", venue}, {"tk", ticker}, {"ev", "TEST-EVENT"}, {"title", "Test market"}, {"short", ticker},
          {"grp", "TEST"}, {"kind", "OTHER"}, {"og", "TEST-EVENT"}, {"oc", "YES"}, {"open_ms", now - 1000},
          {"close_ms", now + 3600000}, {"status", "OPEN"}, {"result", nullptr},
          {"grid", json::array({{{"start", "0.0000"}, {"end", "1.0000"}, {"step", "0.0100"}}})},
          {"fee", {{"type", "FREE"}, {"mult", 0}}}, {"shard", venue == "K" ? json("S0") : json(nullptr)},
          {"lip", false}, {"start_ms", nullptr}, {"man", false}, {"cat", "OTHER"}, {"date", et_date(now)},
          {"pin", true}, {"gk", "TEST"}, {"live", true}};
}
static json print(std::int64_t ts) { return {{"px", "0.5100"}, {"qty", 1}, {"side", "BUY"}, {"ts_ms", ts}}; }
static json book(const std::string& ticker, const std::string& venue = "K") {
  return {{"v", venue}, {"tk", ticker}, {"depth", "FULL"},
          {"bids", json::array({json::array({"0.4900", 10}), json::array({"0.4800", 20})})},
          {"asks", json::array({json::array({"0.5100", 11}), json::array({"0.5200", 21})})},
          {"trades", json::array()}, {"last", nullptr}, {"vol", 0},
          {"ref", {{"fair", nullptr}, {"sd", nullptr}, {"espn", nullptr}, {"espn_ts_ms", nullptr}, {"game", nullptr}}},
          {"ex_ms", now_ms()}};
}
static json order(const std::string& cid, int rev = 1) {
  const auto now = now_ms();
  return {{"cid", cid}, {"oid", nullptr}, {"v", "K"}, {"tk", "MARKET-A"}, {"side", "BUY"}, {"px", "0.4900"},
          {"qty", 2}, {"filled", 0}, {"left", 2}, {"avg", nullptr}, {"tif", "POST"}, {"exp_ms", now + 300000},
          {"st", "PEND"}, {"why", ""}, {"own", "MAN"}, {"lane", nullptr}, {"piq", nullptr}, {"fee", "0.0000"},
          {"touch", "0.4900"}, {"edge", nullptr},
          {"t", {{"click", now}, {"recv", now}, {"sent", nullptr}, {"ack", nullptr}, {"done", nullptr}}}, {"rev", rev}};
}
static json fill(const std::string& fid, std::int64_t ts) {
  return {{"fid", fid}, {"cid", "Mtest-01"}, {"oid", "venue-order"}, {"v", "K"}, {"tk", "MARKET-A"},
          {"side", "BUY"}, {"px", "0.4900"}, {"qty", 1}, {"liq", "M"}, {"fee", "0.0000"}, {"ts_ms", ts},
          {"own", "MAN"}, {"lane", nullptr}, {"mid", nullptr}, {"fair", nullptr}, {"real", "0.0000"}};
}
static json position(const std::string& ticker, const std::string& venue = "K") {
  return {{"v", venue}, {"tk", ticker}, {"pos", 2}, {"avg", "0.4900"}, {"own", "MAN"}, {"mark", "0.5000"},
          {"unrl", "0.0200"}, {"real", "0.0000"}, {"fees", "0.0000"}, {"maxl", "0.9800"},
          {"if_yes", "1.0200"}, {"if_no", "-0.9800"}};
}

struct Harness {
  Feeds feeds;
  Pub pub{"pub-data-test", feeds, [] { return HdrInputs{}; }, 4};
  Worker worker{"pub-test"};
  std::shared_ptr<Stream> full, top;
  std::atomic<int> subscriptions{0};
  Harness() {
    pub.set_subs_callback([this](const std::string&, const Subs&) { ++subscriptions; });
    full = open("full"); top = open("top");
    worker.start([this](Worker& w) { pub.run(w); });
  }
  ~Harness() { worker.request_stop(); worker.join(); }
  std::shared_ptr<Stream> open(const std::string& sid) {
    auto stream = std::make_shared<Stream>(sid, 4096, mono_ms());
    stream->q.offer("hb", now_ms(), pub.hb_data(sid, now_ms()));
    std::string err;
    CHECK(pub.streams().open(stream, &err));
    return stream;
  }
  PubReply snapshot(const std::string& sid = "full") {
    auto reply = std::make_shared<std::promise<PubReply>>(); auto future = reply->get_future();
    CHECK(pub.post(PubSnapshot{sid, reply}));
    if (future.wait_for(2s) != std::future_status::ready) { CHECK(false); return {500, json::object()}; }
    auto result = future.get();
    if (result.status == 200) capture["snapshots"].push_back(result.body);
    return result;
  }
  void subscribe(const std::string& sid, Subs subs) {
    auto reply = std::make_shared<std::promise<PubReply>>(); auto future = reply->get_future();
    CHECK(pub.post(PubSubscribe{sid, std::move(subs), reply}));
    CHECK(future.wait_for(2s) == std::future_status::ready);
    CHECK(future.get().status == 200);
  }
  void update(std::string type, json row, bool mark_only = false) { CHECK(pub.post(PubData{std::move(type), std::move(row), mark_only})); }
};

static std::vector<json> drain(const std::shared_ptr<Stream>& stream) {
  std::vector<json> messages;
  const auto frames = stream->q.drain_for(0ms);
  std::size_t at = 0;
  while ((at = frames.find("data: ", at)) != std::string::npos) {
    at += 6; const auto end = frames.find('\n', at);
    const auto message = json::parse(frames.substr(at, end - at));
    capture["events"].push_back(message); messages.push_back(message); at = end;
  }
  return messages;
}
static std::vector<json> of_type(const std::vector<json>& events, const std::string& type) {
  std::vector<json> result;
  for (const auto& event : events) if (event.at("type") == type) result.push_back(event.at("data"));
  return result;
}

static void books_and_subscriptions() {
  Harness h;
  h.subscribe("full", Subs{{{Venue::K, "MARKET-A"}}, false, {}});
  h.subscribe("top", Subs{{}, true, {}});
  CHECK(h.subscriptions == 2);
  h.update("mkt", market("MARKET-A")); h.update("mkt", market("MARKET-B"));
  h.update("book", book("MARKET-A")); h.update("book", book("MARKET-B"));
  CHECK(eventually([&] { return h.snapshot("top").body.at("books").size() == 2; }));
  const auto full = h.snapshot().body;
  CHECK(full.at("books").size() == 1);
  CHECK(full.at("books")[0].at("depth") == "FULL");
  CHECK(full.at("books")[0].at("bids").size() == 2);
  for (const auto& b : h.snapshot("top").body.at("books")) {
    CHECK(b.at("depth") == "TOP"); CHECK(b.at("asks").size() == 1);
  }
  auto first_events = drain(h.full);
  CHECK(of_type(first_events, "mkt").size() == 2);
  CHECK(of_type(first_events, "book").size() == 1);
  CHECK(first_events[1].at("type") == "mkt"); // first frame is the opening hb
  drain(h.top);
  h.subscribe("full", Subs{{{Venue::K, "MARKET-A"}, {Venue::K, "MARKET-B"}}, false, {}});
  auto joined = of_type(drain(h.full), "book");
  CHECK(joined.size() == 1);
  if (!joined.empty()) CHECK(joined[0].at("tk") == "MARKET-B");
  const auto after_sub = h.snapshot().body;
  CHECK(after_sub.at("books").size() == 2);
  CHECK(after_sub.at("seq").get<std::uint64_t>() == h.full->q.last_seq());

  // Many replacements within a 50 ms window produce complete paired sides and all prints.
  const auto started = mono_ms();
  for (int i = 0; i < 40; ++i) {
    auto b = book("MARKET-A"); b["vol"] = i + 1;
    b["bids"][0][1] = i + 1; b["asks"][0][1] = 100 + i;
    b["trades"].push_back(print(now_ms())); h.update("book", std::move(b));
  }
  CHECK(eventually([&] { return h.snapshot().body.at("books")[0].at("vol") == 40; }));
  const auto elapsed = mono_ms() - started;
  auto updates = of_type(drain(h.full), "book");
  CHECK(updates.size() <= static_cast<std::size_t>(elapsed / 50 + 2));
  std::size_t prints = 0;
  for (const auto& b : updates) {
    prints += b.at("trades").size();
    CHECK(b.at("asks")[0][1].get<int>() - b.at("bids")[0][1].get<int>() == 99);
  }
  CHECK(prints == 40);
  CHECK(h.snapshot().body.at("books")[0].at("trades").empty());

  // Removing a live market removes both the cached book and queued coalesced updates.
  auto leave = market("MARKET-A"); leave["live"] = false;
  h.update("mkt", leave); h.update("book", book("MARKET-A"));
  CHECK(eventually([&] { return h.snapshot().body.at("markets").size() == 1; }));
  CHECK(h.snapshot().body.at("books").size() == 1);
  auto events = drain(h.full);
  CHECK(of_type(events, "mkt").size() == 1);
  CHECK(of_type(events, "book").empty());
  drain(h.top);
}

static void account_revisions_replacement_and_retention() {
  Harness h;
  auto current = order("Mtest-01", 3); current["st"] = "WORK";
  h.update("ord", current); h.update("ord", order("Mtest-01", 2));
  const auto now = now_ms();
  h.update("fill", fill("fill-new", now)); h.update("fill", fill("fill-new", now));
  h.update("fill", fill("fill-old", now - 100));
  h.update("fill", fill("fill-yesterday", now - 86400000));
  auto expired = order("Mexpired-01"); expired["st"] = "EXP"; expired["left"] = 0;
  expired["why"] = "EXPIRY"; expired["t"]["done"] = now - 121000; h.update("ord", expired);
  h.update("pos", position("MARKET-A")); h.update("pos", position("MARKET-P", "P"));
  CHECK(eventually([&] { return h.snapshot().body.at("pos").size() == 2; }));
  const auto snap = h.snapshot().body;
  CHECK(snap.at("orders").size() == 1);
  CHECK(snap.at("orders")[0].at("rev") == 3);
  CHECK(snap.at("orders")[0].at("st") == "WORK");
  CHECK(snap.at("fills").size() == 2);
  CHECK(snap.at("fills")[0].at("fid") == "fill-old");
  auto fills = of_type(drain(h.full), "fill");
  CHECK(fills.size() == 3); // duplicate never streamed, old-day fill subsequently pruned

  CHECK(h.pub.post(PubReplace{"ord", {}, "K"}));
  CHECK(h.pub.post(PubReplace{"pos", {}, "K"}));
  CHECK(eventually([&] { return h.full->q.closed(); }));
  h.full = h.open("full");
  const auto replaced = h.snapshot().body;
  CHECK(replaced.at("orders").size() == 1); // absence cannot turn uncertain orders into cancelled
  CHECK(replaced.at("pos").size() == 1);
  CHECK(replaced.at("pos")[0].at("v") == "P");
  drain(h.full); drain(h.top);
}

static void position_marks_and_fill_override() {
  Harness h;
  h.update("pos", position("MARKET-A"));
  CHECK(eventually([&] { return h.snapshot().body.at("pos").size() == 1; }));
  drain(h.full);
  for (int i = 0; i < 10; ++i) {
    auto row = position("MARKET-A"); row["mark"] = "0.5500"; row["unrl"] = "0.1200";
    h.update("pos", row, true);
  }
  // A fill/reconciliation must replace pending marks immediately, not wait the mark throttle.
  auto filled = position("MARKET-A"); filled["pos"] = 3; h.update("pos", filled);
  CHECK(eventually([&] { return h.snapshot().body.at("pos")[0].at("pos") == 3; }));
  const auto events = of_type(drain(h.full), "pos");
  CHECK(events.size() == 1);
  std::this_thread::sleep_for(1050ms);
  CHECK(h.snapshot().body.at("pos")[0].at("pos") == 3); // old pending mark did not overwrite the fill
  CHECK(of_type(drain(h.full), "pos").empty());
  drain(h.top);
}

static void contract_validation() {
  if (std::string(DESKD_NODE).empty()) { std::printf("node missing: contract validation SKIPPED\n"); return; }
  TempDir dir;
  const auto file = dir.file("pub-capture.json");
  { std::ofstream f(file); f << capture.dump(); }
  const std::string cmd = std::string("\"") + DESKD_NODE + "\" \"" + DESKD_SOURCE_DIR +
                          "/tests/deskd/contract_check.cjs\" \"" + file + "\"";
  CHECK(std::system(cmd.c_str()) == 0);
}

static void upstream_overflow_is_visible() {
  Feeds feeds;
  Pub pub("e-overflow", feeds, [] { return HdrInputs{}; }, 1);
  bool dropped = false;
  for (int i=0; i<4100; ++i)
    if (!pub.post(PubData{"pos",position("OVERFLOW-"+std::to_string(i))})) dropped = true;
  CHECK(dropped && !pub.healthy());
  // The latch differs from a slow-tab SSE overflow: the publisher's own cache lost an update.
  CHECK(!pub.healthy());
}

int main() {
  books_and_subscriptions();
  account_revisions_replacement_and_retention();
  position_marks_and_fill_override();
  upstream_overflow_is_visible();
  contract_validation();
  return finish("deskd pub data");
}
