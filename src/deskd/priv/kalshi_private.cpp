#include "deskd/priv/kalshi_private.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <map>
#include <set>
#include <thread>
#include "deskd/app/log.h"
#include "deskd/app/worker.h"
#include "deskd/gw/kalshi_sign.h"
#include "deskd/net/ws.h"

namespace deskd {
using json = nlohmann::json;
std::optional<PrivateEvent> parse_private_event(const json& frame, std::int64_t received,
                                               std::string* why) {
  auto fail = [&]() -> std::optional<PrivateEvent> {
    if (why) *why = "malformed private event";
    return {};
  };
  if (!frame.is_object() || !frame.contains("type") || !frame["type"].is_string() ||
      !frame.contains("msg") || !frame["msg"].is_object()) return fail();
  PrivateEvent out; out.received_ms = received;
  auto body = frame["msg"];
  if (body.contains("client_order_id")) {
    if (!body["client_order_id"].is_string()) return fail();
    out.cid = body["client_order_id"].get<std::string>();
  }
  if (frame["type"] == "user_order") {
    auto order = kalshi::parse_order(json{{"order",body}}.dump(),why);
    if (!order) return {};
    out.order = std::move(*order);
    return out;
  }
  if (frame["type"] == "order_group_updates") {
    out.kind = PrivateEvent::Kind::GROUP;
    if (!body.contains("order_group_id") || !body["order_group_id"].is_string() ||
        !body.contains("event_type") || !body["event_type"].is_string()) return fail();
    out.group_id = body["order_group_id"].get<std::string>();
    out.group_event = body["event_type"].get<std::string>();
    if (out.group_id.empty()) return fail();
    return out;
  }
  if (frame["type"] != "fill") return fail();
  out.kind = PrivateEvent::Kind::FILL;
  if (!body.contains("trade_id") || !body["trade_id"].is_string() ||
      body["trade_id"].get<std::string>().empty()) return fail();
  // A private fill has no fill_id: its trade_id is its stable execution identity.
  body["fill_id"] = body["trade_id"];
  if (body.contains("market_ticker")) body["ticker"] = body["market_ticker"];
  if (body.contains("subaccount")) body["subaccount_number"] = body["subaccount"];
  if (body.contains("ts_ms")) {
    if (!body["ts_ms"].is_number_integer()) return fail();
    const auto ts = body["ts_ms"].get<std::int64_t>();
    if (ts < 1000000000000LL || ts > 100000000000000LL) return fail();
    body["created_time"] = iso_ms(ts);
  }
  if (body.contains("post_position_fp")) {
    if (!body["post_position_fp"].is_string()) return fail();
    out.post_position = kalshi::parse_count(body["post_position_fp"].get<std::string>(),true);
    if (!out.post_position) return fail();
  }
  auto fills = kalshi::parse_fills(json{{"fills",json::array({body})},{"cursor",""}}.dump(),why);
  if (!fills || fills->rows.size() != 1) return {};
  out.fill = std::move(fills->rows.front());
  return out;
}

KalshiPrivate::KalshiPrivate(net::Endpoint ep, net::ConnFactory& factory, const KalshiSigner& signer,
                             FeedState& feed, EventFn event, ReconcileFn reconcile)
    : KalshiPrivate(std::move(ep),factory,signer,feed,std::move(event),std::move(reconcile),Options{}) {}
KalshiPrivate::KalshiPrivate(net::Endpoint ep, net::ConnFactory& factory, const KalshiSigner& signer,
                             FeedState& feed, EventFn event, ReconcileFn reconcile, Options options)
    : endpoint_(std::move(ep)),factory_(factory),signer_(signer),feed_(feed),event_(std::move(event)),
      reconcile_(std::move(reconcile)),options_(options) {}
std::uint64_t KalshiPrivate::fence() const {
  return subscribed_.load(std::memory_order_acquire) ? generation_.load(std::memory_order_acquire) : 0;
}
bool KalshiPrivate::reconcile(std::uint64_t token) {
  if (!token || token != fence()) return false;
  confirmed_.store(token,std::memory_order_release);
  return token == fence();
}

void KalshiPrivate::run(Worker& worker) {
  int backoff = std::max(20,options_.backoff_min_ms);
  while (!worker.stopping()) {
    subscribed_.store(false,std::memory_order_release);
    generation_.fetch_add(1,std::memory_order_acq_rel);
    confirmed_.store(0,std::memory_order_release);
    feed_.set(false,true);
    worker.beat();
    net::WsClient ws; std::string why;
    const auto auth = signer_.headers(now_ms(),"GET",endpoint_.base_path);
    bool connected = !auth.empty() && ws.connect(factory_,endpoint_,auth,options_.connect_ms,&why,[&]{worker.beat();});
    // Account channels intentionally omit the market filter: all THIS account's orders and
    // fills are needed for reconciliation. This is not an empty public market subscription.
    if (connected) connected = ws.send(net::WsOp::TEXT,
        json{{"id",1},{"cmd","subscribe"},{"params",{{"channels",{"user_orders","fill"}}}}}.dump(),
        500,&why,[&]{worker.beat();});
    // order_group_updates on its own command: Systematic's groups learn of a venue trigger from
    // it, but a refusal of it costs only that, never the orders and fills.
    if (connected) connected = ws.send(net::WsOp::TEXT,
        json{{"id",2},{"cmd","subscribe"},{"params",{{"channels",{"order_group_updates"}}}}}.dump(),
        500,&why,[&]{worker.beat();});
    std::map<std::int64_t,std::string> sids;
    std::set<std::string> channels;
    const auto started = mono_ms(); auto last_ping = started;
    std::int64_t ping_sent = 0; std::string ping;
    int lag = -1;
    while (connected && !worker.stopping()) {
      worker.beat();
      const auto now = mono_ms();
      if (channels.size()!=2 && now-started>options_.ack_ms) break;
      if (ping_sent && now-ping_sent>options_.pong_ms) break;
      if (!ping_sent && now-last_ping>=options_.ping_ms) {
        ping="private-"+std::to_string(now); ping_sent=now; last_ping=now;
        if (!ws.send(net::WsOp::PING,ping,250,&why)) break;
      }
      const auto gen = fence();
      feed_.set(true,!gen || confirmed_.load(std::memory_order_acquire)!=gen,lag);
      net::WsEvent frame;
      const auto status=ws.read(&frame,100,&why);
      if (status==net::WsClient::Read::TIMEOUT) continue;
      if (status!=net::WsClient::Read::EVENT) break;
      if (frame.op==net::WsOp::PING) continue;
      if (frame.op==net::WsOp::PONG) { if(frame.payload==ping) ping_sent=0; continue; }
      if (frame.op!=net::WsOp::TEXT) break;
      const auto j=json::parse(frame.payload,nullptr,false);
      if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) break;
      const auto type=j["type"].get<std::string>();
      if (type=="error" && j.contains("id") && j["id"]==2) {
        DLOG_WARN("Kalshi refused order_group_updates: Systematic sees a venue-triggered group only by its refusals");
        continue;
      }
      if (type=="subscribed") {
        if (!j.contains("id") || !j.contains("msg") || !j["msg"].is_object()) break;
        const auto& msg=j["msg"];
        if (!msg.contains("sid") || !msg["sid"].is_number_integer() || !msg.contains("channel") || !msg["channel"].is_string()) break;
        const auto sid=msg["sid"].get<std::int64_t>(); const auto channel=msg["channel"].get<std::string>();
        if (j["id"]==2) {
          if (channel!="order_group_updates" || sid<=0 || sids.count(sid)) break;
          sids[sid]=channel;
          continue;
        }
        if (j["id"]!=1) break;
        if (sid<=0 || sids.count(sid) || (channel!="fill" && channel!="user_orders") || !channels.insert(channel).second) break;
        sids[sid]=channel;
        if (channels.size()==2) {
          subscribed_.store(true,std::memory_order_release);
          if (reconcile_) reconcile_();
          backoff=std::max(20,options_.backoff_min_ms);
        }
        continue;
      }
      if (!j.contains("sid") || !j["sid"].is_number_integer()) break;
      const auto sid=j["sid"].get<std::int64_t>();
      if (!sids.count(sid)) break;
      if (type=="order_group_updates") {
        // group state is not account state: it neither moves the reconcile fence nor waits for it
        if (sids[sid]!="order_group_updates") break;
        if (const auto g=parse_private_event(j,now_ms(),&why); g && event_) event_(*g);
        continue;
      }
      if (type=="fill" ? sids[sid]!="fill" : type!="user_order" || sids[sid]!="user_orders") break;
      const auto event=parse_private_event(j,now_ms(),&why);
      if (!event) break;
      const auto old=generation_.fetch_add(1,std::memory_order_acq_rel);
      const bool trusted=confirmed_.load(std::memory_order_acquire)==old;
      // An untrusted event's uncertainty is published before it crosses owners, and one gw did not
      // take right after. A trusted one that gw took raises none: gw applies it in order, and
      // marking every event a gap until the next pass refused orders "private feed not up" between
      // two lines of this loop (16 in a 4-hour TSan soak, 2026-10-13).
      if (!trusted) feed_.set(true,true,lag);
      const bool applied=event_ && event_(*event);
      if (trusted && applied) confirmed_.store(old+1,std::memory_order_release);
      else {
        feed_.set(true,true,lag);
        if (reconcile_) reconcile_();
      }
      if (j.contains("sending_ts_ms") && j["sending_ts_ms"].is_number_integer()) {
        const auto delta=now_ms()-j["sending_ts_ms"].get<std::int64_t>();
        if (delta>=0 && delta<=INT_MAX) lag=static_cast<int>(delta);
      }
    }
    subscribed_.store(false,std::memory_order_release);
    generation_.fetch_add(1,std::memory_order_acq_rel);
    confirmed_.store(0,std::memory_order_release); feed_.set(false,true); ws.close();
    if (reconcile_) reconcile_();
    const auto end=mono_ms()+backoff;
    while (!worker.stopping() && mono_ms()<end) { worker.beat(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    backoff=std::min(std::max(20,options_.backoff_max_ms),backoff*2);
  }
  subscribed_.store(false); feed_.set(false,true);
}
}
