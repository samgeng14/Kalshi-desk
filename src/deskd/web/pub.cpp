#include "deskd/web/pub.h"

#include <algorithm>
#include <chrono>
#include <set>

#include "deskd/app/log.h"
#include "deskd/app/worker.h"

namespace deskd {

using json = nlohmann::json;

namespace {
std::string market_key(const json& row) { return row.at("v").get<std::string>() + ":" + row.at("tk").get<std::string>(); }
std::string market_key(const MarketRef& ref) { return std::string(ref.v == Venue::K ? "K:" : "P:") + ref.tk; }
std::string row_key(const std::string& type, const json& row) {
  if (type == "mkt" || type == "book" || type == "pos") return market_key(row);
  if (type == "ord") return row.at("cid").get<std::string>();
  if (type == "fill") return row.at("fid").get<std::string>();
  if (type == "mo") return row.at("fid").get<std::string>() + ":" + row.at("h").get<std::string>();
  if (type == "lane") return row.at("id").get<std::string>();
  return {};
}
bool terminal(const json& order) {
  const auto st = order.value("st", "");
  return st == "FILL" || st == "CXL" || st == "REJ" || st == "EXP";
}
std::string depth_for(const Subs& subs, const std::string& key, const json& market) {
  for (const auto& ref : subs.ladders) if (market_key(ref) == key) return "FULL";
  for (const auto& ref : subs.tops) if (market_key(ref) == key) return "TOP";
  if (subs.tops_all && market.value("status", "") == "OPEN") return "TOP";
  return {};
}
json project_book(json book, const std::string& depth, bool no_prints) {
  book["depth"] = depth;
  const std::size_t cap = depth == "FULL" ? 100 : 1;
  for (const char* side : {"bids", "asks"}) {
    auto& levels = book.at(side);
    if (levels.size() > cap) levels.erase(levels.begin() + cap, levels.end());
  }
  if (no_prints) book["trades"] = json::array();
  return book;
}
template<class Rows> json array_rows(const Rows& rows) {
  json out = json::array();
  for (const auto& pair : rows) out.push_back(pair.second);
  return out;
}
}  // namespace

Pub::Pub(std::string epoch, const Feeds& feeds, HdrFn hdr, int max_streams, std::size_t stream_cap)
    : epoch_(std::move(epoch)),
      feeds_(feeds),
      hdr_fn_(std::move(hdr)),
      stream_cap_(stream_cap),
      streams_(max_streams),
      inbox_(4096, 256) {}

bool Pub::post(PubMsg m) {
  const bool waited_on = std::holds_alternative<PubSnapshot>(m) || std::holds_alternative<PubSubscribe>(m);
  const bool ok = inbox_.push(std::move(m), waited_on ? Lane::PRIORITY : Lane::NORMAL);
  // Losing a producer update differs from a slow stream: a snapshot may be missing it too.
  // Latch the screen read-only until restart; never silently show a healthy partial cache.
  if (!ok && !waited_on) overflow_.store(true, std::memory_order_release);
  return ok;
}

Pub::Rows* Pub::rows(const std::string& type) {
  if (type == "mkt") return &markets_;
  if (type == "book") return &books_;
  if (type == "ord") return &orders_;
  if (type == "pos") return &positions_;
  if (type == "fill") return &fills_;
  if (type == "lane") return &lanes_;
  if (type == "mo") return &mos_;
  return nullptr;
}
const Pub::Rows* Pub::rows(const std::string& type) const { return const_cast<Pub*>(this)->rows(type); }

void Pub::book_to(const std::shared_ptr<Stream>& stream, const Subs& subs, const json& book,
                  std::int64_t now, bool no_prints) {
  const auto key = market_key(book);
  const auto m = markets_.find(key);
  if (m == markets_.end()) return;
  const auto depth = depth_for(subs, key, m->second);
  if (!depth.empty()) stream->q.offer("book", now, project_book(book, depth, no_prints).dump());
}

void Pub::publish_book(const std::string& key, json book, std::int64_t mono) {
  // Each input is one atomic replacement of both sides: never coalesce bid and ask separately.
  book = project_book(std::move(book), "FULL", false);
  for (const auto& stream : streams_.open_streams()) book_to(stream, streams_.subs(stream->sid), book, now_ms());
  book["trades"] = json::array(); // snapshots/subscription refreshes must not replay prints
  books_[key] = std::move(book);
  book_at_[key] = mono;
}

void Pub::data(PubData update) {
  const auto mono = mono_ms();
  if (update.type == "risk") {
    if (update.mark_only && mono - risk_at_ < 1000) { pending_risk_ = std::move(update.data); return; }
    pending_risk_ = nullptr;
    risk_at_ = mono;
    if (risk_ != update.data) { risk_ = std::move(update.data); broadcast("risk", risk_.dump(), now_ms()); }
    return;
  }
  auto* cache = rows(update.type);
  if (!cache) return;
  const auto key = row_key(update.type, update.data);
  if (update.type == "book") {
    if (!markets_.count(key)) return; // a listing must precede its book, including on live-set joins
    if (update.data.value("depth", "") != "FULL") return;
    const auto& bids = update.data.at("bids");
    const auto& asks = update.data.at("asks");
    if (!bids.empty() && !asks.empty() && bids.front().at(0).get<std::string>() >= asks.front().at(0).get<std::string>()) {
      alert("WARN", "KX", "Discarded crossed book for " + update.data.at("tk").get<std::string>());
      return;
    }
    auto pending = pending_books_.find(key);
    if (pending != pending_books_.end()) {
      json prints = std::move(pending->second["trades"]);
      for (const auto& print : update.data.at("trades")) prints.push_back(print);
      // A stalled consumer must not grow memory without bound. The book remains complete;
      // the newest last/volume still report the venue state if >5000 prints arrive in 50 ms.
      if (prints.size() > 5000) prints.erase(prints.begin(), prints.end() - 5000);
      update.data["trades"] = std::move(prints);
    }
    pending_books_[key] = std::move(update.data);
    if (!book_at_.count(key) || mono - book_at_[key] >= 50) {
      auto book = std::move(pending_books_.at(key));
      pending_books_.erase(key);
      publish_book(key, std::move(book), mono);
    }
    return;
  }
  if (update.type == "mkt" && !update.data.value("live", true)) {
    if (markets_.erase(key)) broadcast("mkt", update.data.dump(), now_ms());
    books_.erase(key); pending_books_.erase(key); book_at_.erase(key); market_closed_at_.erase(key);
    return;
  }
  if (update.type == "mkt") {
    if (update.data.value("status", "") != "OPEN") {
      if (!market_closed_at_.count(key)) market_closed_at_[key] = now_ms();
    } else market_closed_at_.erase(key);
  }
  if (update.type == "pos") {
    if (update.mark_only && mono - position_at_[key] < 1000) {
      pending_positions_[key] = std::move(update.data);
      return;
    }
    pending_positions_.erase(key);
    position_at_[key] = mono;
  }
  const auto old = cache->find(key);
  if (old != cache->end()) {
    if (update.type == "fill" || old->second == update.data) return;
    if (update.type == "ord" && update.data.at("rev").get<std::uint64_t>() <= old->second.at("rev").get<std::uint64_t>()) return;
  }
  (*cache)[key] = std::move(update.data);
  broadcast(update.type, cache->at(key).dump(), now_ms());
}

void Pub::replace(const PubReplace& update) {
  auto* cache = rows(update.type);
  if (!cache || update.type == "book") return;
  std::set<std::string> keep;
  for (const auto& row : update.rows) keep.insert(row_key(update.type, row));
  bool removed = false;
  std::vector<json> leaving;
  for (auto it = cache->begin(); it != cache->end();) {
    const bool scope = update.venue.empty() || it->second.value("v", "") == update.venue;
    if (!scope || keep.count(it->first)) { ++it; continue; }
    if (update.type == "ord" && !terminal(it->second)) { ++it; continue; }
    if (update.type == "mkt") { auto row = it->second; row["live"] = false; leaving.push_back(std::move(row)); ++it; }
    else { removed = true; it = cache->erase(it); }
  }
  for (auto& row : leaving) data(PubData{"mkt", std::move(row)});
  for (const auto& row : update.rows) data(PubData{update.type, row});
  // No delete event exists for account rows in contract 1.5. An authoritative replacement
  // that removes one therefore ends streams, making the standard reconnect take a snapshot.
  if (removed) streams_.close_all();
}

void Pub::flush_due(std::int64_t mono) {
  for (auto it = pending_books_.begin(); it != pending_books_.end();) {
    if (mono - book_at_[it->first] < 50) { ++it; continue; }
    const auto key = it->first;
    auto book = std::move(it->second);
    it = pending_books_.erase(it);
    publish_book(key, std::move(book), mono);
  }
  std::vector<json> positions;
  for (auto it = pending_positions_.begin(); it != pending_positions_.end();) {
    if (mono - position_at_[it->first] < 1000) { ++it; continue; }
    positions.push_back(std::move(it->second));
    it = pending_positions_.erase(it);
  }
  for (auto& row : positions) data(PubData{"pos", std::move(row)});
  if (!pending_risk_.is_null() && mono - risk_at_ >= 1000) data(PubData{"risk", std::move(pending_risk_)});
}

void Pub::prune(std::int64_t now) {
  const auto today = et_date(now);
  if (!day_.empty() && today != day_) {
    for (auto it = positions_.begin(); it != positions_.end();) {
      if (it->second.value("pos", 0.0) == 0) {
        pending_positions_.erase(it->first); position_at_.erase(it->first); it = positions_.erase(it);
      } else ++it;
    }
  }
  day_ = today;
  for (auto it = orders_.begin(); it != orders_.end();) {
    const auto& done = it->second.at("t").at("done");
    if (terminal(it->second) && !done.is_null() && now - done.get<std::int64_t>() > 120000) it = orders_.erase(it);
    else ++it;
  }
  for (auto it = market_closed_at_.begin(); it != market_closed_at_.end();) {
    if (now - it->second <= 1800000) { ++it; continue; }
    markets_.erase(it->first); books_.erase(it->first); pending_books_.erase(it->first); book_at_.erase(it->first);
    it = market_closed_at_.erase(it);
  }
  std::vector<std::pair<std::int64_t, std::string>> timestamps;
  for (auto it = fills_.begin(); it != fills_.end();) {
    const auto ts = it->second.at("ts_ms").get<std::int64_t>();
    if (et_date(ts) != today) it = fills_.erase(it);
    else { timestamps.emplace_back(ts, it->first); ++it; }
  }
  std::sort(timestamps.begin(), timestamps.end());
  if (timestamps.size() > 5000)
    for (std::size_t i = 0; i < timestamps.size() - 5000; ++i) fills_.erase(timestamps[i].second);
  for (auto it = mos_.begin(); it != mos_.end();) {
    if (!fills_.count(it->second.at("fid").get<std::string>())) it = mos_.erase(it);
    else ++it;
  }
}

json Pub::snapshot(const std::string& sid, std::uint64_t seq, std::int64_t now) const {
  const auto subs = streams_.subs(sid);
  auto out = snapshot_json(epoch_, sid, seq, now, hdr_, alerts_json(), feeds_json(feeds_), subs);
  out["markets"] = array_rows(markets_);
  for (const auto& pair : books_) {
    const auto m = markets_.find(pair.first);
    if (m == markets_.end()) continue;
    const auto depth = depth_for(subs, pair.first, m->second);
    if (!depth.empty()) out["books"].push_back(project_book(pair.second, depth, true));
  }
  out["orders"] = array_rows(orders_);
  out["pos"] = array_rows(positions_);
  out["fills"] = array_rows(fills_);
  std::sort(out["fills"].begin(), out["fills"].end(), [](const json& a, const json& b) {
    return std::make_pair(a.at("ts_ms"), a.at("fid")) < std::make_pair(b.at("ts_ms"), b.at("fid"));
  });
  out["mos"] = array_rows(mos_);
  out["lanes"] = array_rows(lanes_);
  out["risk"] = risk_;
  return out;
}

void Pub::alert(const std::string& lvl, const std::string& src, const std::string& msg) {
  post(PubAlert{lvl, src, msg, now_ms()});
}

std::string Pub::hb_data(const std::string& sid, std::int64_t now_ms) const {
  return hb_json(epoch_, sid, now_ms, feeds_json(feeds_)).dump();
}

void Pub::broadcast(const std::string& type, const std::string& data_json, std::int64_t now) {
  for (const auto& s : streams_.open_streams()) s->q.offer(type, now, data_json);
}

void Pub::broadcast_hb(std::int64_t now) {
  const json feeds = feeds_json(feeds_);
  for (const auto& s : streams_.open_streams()) s->q.offer("hb", now, hb_json(epoch_, s->sid, now, feeds).dump());
  hb_sent_.fetch_add(1, std::memory_order_relaxed);
}

void Pub::maybe_hdr(bool force) {
  const std::int64_t mono = mono_ms();
  if (!force && mono - hdr_at_ < 250) return;   // at most 4/s
  json h = hdr_json(hdr_fn_());
  if (!healthy()) {
    h["cash"]["K"]["ro"] = true;
    h["cash"]["P"]["ro"] = true;
    if (!overflow_reported_) {
      overflow_reported_ = true;
      streams_.close_all();
      const auto a = alert_json("pub-overflow", "CRIT", "PUB",
          "Publisher inbox overflow: display is read-only; restart to reconcile every cache", now_ms());
      alerts_.push_back(a);
      if (alerts_.size() > kAlertsKept) alerts_.pop_front();
      DLOG_ERR("publisher inbox overflow: restart required");
    }
  }
  std::string d = h.dump();
  if (d == hdr_dump_) return;                  // only on change
  const bool first = hdr_dump_.empty();
  hdr_ = std::move(h);
  hdr_dump_ = std::move(d);
  hdr_at_ = mono;
  if (!first) {
    broadcast("hdr", hdr_dump_, now_ms());
    hdr_sent_.fetch_add(1, std::memory_order_relaxed);
  }
}

json Pub::alerts_json() const {
  json a = json::array();
  for (const auto& x : alerts_) a.push_back(x);
  return a;
}

void Pub::handle(PubMsg& m) {
  const std::int64_t now = now_ms();
  if (auto* s = std::get_if<PubSnapshot>(&m)) {
    PubReply r;
    auto st = streams_.stream_for(s->sid);
    if (!st) {
      r.status = 409;
      r.body = err_body("BAD_STATE", "no open stream for sid " + s->sid.substr(0, 64));
    } else {
      maybe_hdr(true);   // what the snapshot carries is what the stream has been sent
      prune(now);
      r.body = snapshot(s->sid, st->q.last_seq(), now);
    }
    if (s->reply) s->reply->set_value(std::move(r));
    return;
  }
  if (auto* sub = std::get_if<PubSubscribe>(&m)) {
    PubReply r;
    const auto previous = streams_.subs(sub->sid);
    if (!streams_.set_subs(sub->sid, sub->subs)) {
      r.status = 409;
      r.body = err_body("BAD_STATE", "no open stream for sid " + sub->sid.substr(0, 64));
    } else {
      if (subs_fn_) subs_fn_(sub->sid, sub->subs);
      const auto stream = streams_.stream_for(sub->sid);
      if (stream) {
        for (const auto& pair : books_) {
          const auto mkt = markets_.find(pair.first);
          if (mkt == markets_.end()) continue;
          const auto before = depth_for(previous, pair.first, mkt->second);
          const auto after = depth_for(sub->subs, pair.first, mkt->second);
          if (!after.empty() && after != before) book_to(stream, sub->subs, pair.second, now, true);
        }
      }
      r.body = json{{"ok", true}, {"subs", subs_json(sub->subs)}};
    }
    if (sub->reply) sub->reply->set_value(std::move(r));
    return;
  }
  if (auto* d = std::get_if<PubData>(&m)) { data(std::move(*d)); return; }
  if (auto* r = std::get_if<PubReplace>(&m)) { replace(*r); return; }
  if (auto* a = std::get_if<PubAlert>(&m)) {
    json j = alert_json("a-" + std::to_string(++alert_n_), a->lvl, a->src, a->msg, a->ts_ms ? a->ts_ms : now);
    broadcast("alert", j.dump(), now);
    alerts_.push_back(std::move(j));
    while (alerts_.size() > kAlertsKept) alerts_.pop_front();
  }
}

void Pub::run(Worker& self) {
  // the hb's schedule is on the steady clock, its srv_ms and ts_ms on the wall clock: a wall
  // clock stepped back no longer stops the hb (and with it every page) for as long as the step
  maybe_hdr(true);
  day_ = et_date(now_ms());
  std::int64_t next_hb = mono_ms() + 1000, next_forget = 0;
  while (!self.stopping()) {
    std::int64_t mono = mono_ms();
    const std::int64_t wait = std::max<std::int64_t>(0, std::min<std::int64_t>(next_hb - mono, 10));
    if (auto m = inbox_.pop_for(std::chrono::milliseconds(wait))) handle(*m);
    mono = mono_ms();
    flush_due(mono);
    // An upstream producer must resync after post(false). End streams as well: no page may
    // silently keep a view whose input updates were dropped before they received sequence IDs.
    if (inbox_.take_gap()) streams_.close_all();
    if (mono >= next_hb) {
      prune(now_ms());
      broadcast_hb(now_ms());
      next_hb += 1000;
      if (next_hb <= mono) next_hb = mono + 1000;   // after a stall: no burst of catch-up beats
    }
    maybe_hdr(false);
    if (mono >= next_forget) {
      next_forget = mono + 1000;
      streams_.forget_idle(mono, kForgetSidMs);
    }
    self.beat();
  }
}

}  // namespace deskd
