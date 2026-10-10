#include "deskd/strat/maker.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "deskd/app/log.h"
#include "deskd/rest/state.h"

namespace deskd {
using json = nlohmann::json;

Px grid_step(Px p, const std::vector<kalshi::PriceRange>& grid) {
  for (const auto& r : grid)
    if (p >= r.start && p < r.end && r.step > 0) return r.step;
  if (!grid.empty() && p == grid.back().end && grid.back().step > 0) return grid.back().step;
  return 100;
}

// Both clamp to the tradable (0, $1) first: at the edge a range's own end ($1, or 0) is not a
// price, and without the clamp a lower range would win (9000 for $1 on the tapered grid).
std::optional<Px> grid_floor(Px p, const std::vector<kalshi::PriceRange>& grid) {
  p = std::min<Px>(p, kOneDollar - 1);
  std::optional<Px> best;
  for (const auto& r : grid) {
    if (r.step <= 0 || p < r.start) continue;
    const Px v = r.start + (std::min(p, r.end) - r.start) / r.step * r.step;
    if (v > 0 && v < kOneDollar && (!best || v > *best)) best = v;
  }
  return best;
}

std::optional<Px> grid_ceil(Px p, const std::vector<kalshi::PriceRange>& grid) {
  p = std::max<Px>(p, 1);
  std::optional<Px> best;
  for (const auto& r : grid) {
    if (r.step <= 0 || p > r.end) continue;
    const Px from = std::max(p, r.start);
    const Px v = r.start + (from - r.start + r.step - 1) / r.step * r.step;
    if (v <= r.end && v > 0 && v < kOneDollar && (!best || v < *best)) best = v;
  }
  return best;
}

Px maker_fee(const kalshi::MarketRow& m, Px p) {
  if (m.fee_type != "quadratic_with_maker_fees" || !m.fee_multiplier) return 0;
  const double x = static_cast<double>(p) / kOneDollar;
  return static_cast<Px>(std::ceil(0.0175 * *m.fee_multiplier * x * (1 - x) * kOneDollar - 1e-9));
}

MakerDecision maker_quote(const MakerInput& in, const MakerParams& p) {
  MakerDecision d;
  auto pull = [&](const char* why) { d.quotes.clear(); d.pull = why; return d; };
  if (!in.book || in.book->stale) return pull("book stale");
  if (!in.grid || in.grid->empty()) return pull("no price grid");
  if (!in.private_up) return pull("private feed down");
  if (in.close_ms <= in.now_ms) return pull("closed");
  if (in.book->bids.empty() || in.book->asks.empty()) return pull("one-sided book");
  const Px bb = in.book->bids.front().px, ba = in.book->asks.front().px;
  const Qty bs = in.book->bids.front().qty, as = in.book->asks.front().qty;
  if (bb >= ba || bs <= 0 || as <= 0) return pull("crossed book");
  const Qty q = in.position;
  // T-60 s: a position over 10 contracts goes, IOC at the touch. Before the pull rules below,
  // which are for quotes: an anchor far from the book or a quiet reference is exactly when the
  // last minute's model is steepest, and never a reason to ride a position into settlement.
  const std::int64_t left = in.close_ms - in.now_ms;
  if (left <= p.flatten_ms) {
    if (std::abs(q) > p.flatten_over) {
      d.quotes.push_back({q > 0 ? Side::SELL : Side::BUY, q > 0 ? bb : ba, std::abs(q) / kOneContract * kOneContract, true});
      return d;
    }
    return pull("last minute");
  }
  // a gap in the underlying's reference pulls, gate or no gate (plan section 5)
  if (!in.reference_fresh) return pull("reference not fresh");
  const Px tick = grid_step(bb, *in.grid);
  const long double micro = (static_cast<long double>(bb) * as + static_cast<long double>(ba) * bs) / (bs + as);
  long double fair = (ba - bb) > p.mid_over_ticks * tick ? (bb + ba) / 2.0L : micro;
  Px e = p.edge_alone;
  if (in.anchor) {
    if (std::fabs(static_cast<double>(*in.anchor - micro)) > p.anchor_gap) return pull("anchor and microprice over 3c apart");
    fair = (fair + *in.anchor) / 2.0L;
    e = p.edge_anchor + in.maker_fee;
  }
  d.fair = static_cast<Px>(std::llround(static_cast<double>(fair)));
  const long double skew = static_cast<long double>(p.skew_per_contract) * q / kOneContract;
  const Px bid_cap = static_cast<Px>(std::floor(static_cast<double>(fair - e - skew)));
  const Px ask_floor = static_cast<Px>(std::ceil(static_cast<double>(fair + e - skew)));
  const bool two_sided = in.now_ms - in.open_ms < p.two_sided_ms;
  auto whole = [](Qty x) { return x / kOneContract * kOneContract; };
  // The bid: one tick better than the best bid, or the best bid itself when that would reach the
  // ask (a 1-tick book), never above fair - e - skew. The cap is held under the ask before it is
  // rounded to the grid, so a cap at or through the ask joins the touch instead of dropping the
  // quote. The ask mirrors it.
  if (two_sided || q < 0) {
    const auto better = grid_ceil(bb + 1, *in.grid), cap = grid_floor(std::min<Px>(bid_cap, ba - 1), *in.grid);
    if (cap) {
      const Px px = better ? std::min(*better, *cap) : *cap;
      const Qty size = two_sided ? p.size : std::min(p.size, whole(-q));
      if (px >= bb && px < ba && size > 0) d.quotes.push_back({Side::BUY, px, size, false});
    }
  }
  if (two_sided || q > 0) {
    const auto better = grid_floor(ba - 1, *in.grid), floor = grid_ceil(std::max<Px>(ask_floor, bb + 1), *in.grid);
    if (floor) {
      const Px px = better ? std::max(*better, *floor) : *floor;
      const Qty size = two_sided ? p.size : std::min(p.size, whole(q));
      if (px <= ba && px > bb && size > 0) d.quotes.push_back({Side::SELL, px, size, false});
    }
  }
  if (d.quotes.empty()) d.pull = two_sided ? "fair too close to the touch" : q ? "nothing to reduce at fair" : "past minute 10, flat";
  return d;
}

MakerDecision band_quote(const MakerInput& in, const MakerParams& p) {
  MakerDecision d;
  auto pull = [&](const char* why) { d.quotes.clear(); d.pull = why; return d; };
  if (!in.book || in.book->stale) return pull("book stale");
  if (!in.private_up) return pull("private feed down");
  if (in.close_ms <= in.now_ms) return pull("closed");
  const auto& bids = in.book->bids;
  const auto& asks = in.book->asks;
  const Px bb = bids.empty() ? 0 : bids.front().px, ba = asks.empty() ? kOneDollar : asks.front().px;
  if (!bids.empty() && !asks.empty() && bb >= ba) return pull("crossed book");
  if (bb >= 9600 && bb <= 9900) {
    if (in.position >= p.size) return pull("holding the favourite");
    d.fair = bb;
    d.quotes.push_back({Side::BUY, bb, std::min(p.size, (p.size - std::max<Qty>(0, in.position)) / kOneContract * kOneContract), false});
  } else if (!asks.empty() && ba >= 100 && ba <= 400) {
    if (-in.position >= p.size) return pull("holding the favourite");
    d.fair = ba;
    d.quotes.push_back({Side::SELL, ba, std::min(p.size, (p.size - std::max<Qty>(0, -in.position)) / kOneContract * kOneContract), false});
  } else {
    return pull("no favourite at 96-99c");
  }
  if (d.quotes.back().size <= 0) return pull("holding the favourite");
  return d;
}

MakerEngine::MakerEngine(std::string lane, std::vector<std::string> series, Env env, MakerParams params, Decide decide)
    : lane_(std::move(lane)), series_(std::move(series)), env_(std::move(env)), p_(params),
      decide_(decide ? std::move(decide) : Decide(maker_quote)) {}

// what the lane holds and works in one event, at worst: a position at $1 a contract, a working
// bid at its price, a working ask at 1 - its price (micro-dollars)
std::int64_t MakerEngine::event_worst(const Gateway::SysView& v, const rest::Snapshot& s, const std::string& event) const {
  std::int64_t w = 0;
  auto in_event = [&](const std::string& tk) {
    const auto it = s.markets.find(tk);
    return it != s.markets.end() && it->second.event_ticker == event;
  };
  for (const auto& [cid, o] : v.orders)
    if (o.lane == lane_ && !o.terminal && in_event(o.ticker))
      w += (o.side == Side::BUY ? o.price : kOneDollar - o.price) * o.left;
  for (const auto& [key, pos] : v.positions) {
    const auto bar = key.find('|');
    if (key.compare(0, bar, lane_) == 0 && in_event(key.substr(bar + 1))) w += kOneDollar * std::abs(pos);
  }
  return w;
}

// Everyone else's book: Systematic's resting quotes on this market (acknowledged and not ended)
// come off their levels, and a level they made alone goes.
void MakerEngine::without_own(MdBook& b, const Gateway::SysView& v, const std::string& tk) {
  auto take = [](std::vector<MdLevel>& side, Px px, Qty q) {
    for (auto it = side.begin(); it != side.end(); ++it) {
      if (it->px != px) continue;
      it->qty -= q;
      if (it->qty <= 0) side.erase(it);
      return;
    }
  };
  for (const auto& [cid, o] : v.orders) {
    if (o.ticker != tk || o.terminal || o.left <= 0 || (o.state != OrdState::WORK && o.state != OrdState::PART)) continue;
    take(o.side == Side::BUY ? b.bids : b.asks, o.price, o.left);
  }
}

std::string MakerEngine::new_cid() {
  // S<lane>-<ms in base 36>-<seq>: at most 40 characters, [A-Za-z0-9-]
  std::string t;
  for (auto v = static_cast<std::uint64_t>(now_ms()); v; v /= 36) t.insert(t.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]);
  return "S" + lane_ + "-" + t + "-" + std::to_string(++seq_);
}

void MakerEngine::step(std::int64_t now) {
  const auto view = env_.view ? env_.view() : nullptr;
  const auto snap = env_.snapshot ? env_.snapshot() : nullptr;
  if (!view || !snap) return;
  const bool running = view->run == RunState::RUN && view->lanes_on.count(lane_);
  // the windows open now in the lane's series
  std::set<std::string> markets;
  for (const auto& [tk, m] : snap->markets) {
    const bool mine = std::any_of(series_.begin(), series_.end(), [&](const std::string& s) {
      return tk.size() > s.size() && tk.compare(0, s.size(), s) == 0 && tk[s.size()] == '-';
    });
    if (mine && m.open_ms && *m.open_ms <= now && m.close_ms && *m.close_ms > now && (m.status == "active" || m.status == "open"))
      markets.insert(tk);
  }
  if (env_.want) env_.want(running ? markets : std::set<std::string>{});
  const std::set<std::string> open = markets;
  for (const auto& [tk, s] : sides_) markets.insert(tk);   // finish what is working on a closed one
  for (const auto& tk : markets) {
    auto& slots = sides_[tk];
    // answers from gw: a refusal backs that side off for 10 s
    for (auto& [slot, s] : slots) {
      if (!s.answer.valid() || s.answer.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
      const auto r = s.answer.get();
      if (!r.code.empty()) {
        ++refusals_;
        if (last_pull_[tk] != r.code) DLOG_INFO("%s %s: gw refused %s (%s)", lane_.c_str(), tk.c_str(), r.code.c_str(), r.msg.c_str());
        last_pull_[tk] = r.code;
        s.cid.clear();
        s.backoff_until = now + 10000;
      }
    }
    const auto pos_it = view->positions.find(lane_ + "|" + tk);
    const Qty pos = pos_it == view->positions.end() ? 0 : pos_it->second;
    // a fill of ours pulls both sides for a second (plan: pull on our fill)
    if (last_pos_.count(tk) && last_pos_[tk] != pos) { cool_until_[tk] = now + 1000; cool_why_[tk] = "our fill"; }
    last_pos_[tk] = pos;
    MakerDecision d;
    const auto m = snap->markets.find(tk);
    std::optional<MdBook> book = env_.book ? env_.book(tk) : std::nullopt;
    if (book) without_own(*book, *view, tk);
    const auto a = env_.anchor ? env_.anchor(tk) : Env::Anchor{};
    // the underlying moved 0.5 sigma (of its move to the close) since the quotes went out: both go
    bool resting = false;
    for (const auto& [slot, s] : slots) if (!s.cid.empty()) resting = true;
    if (!resting) quote_spot_.erase(tk);
    else if (a.spot > 0 && a.sd_log > 0 && quote_spot_.count(tk) &&
             std::fabs(std::log(a.spot / quote_spot_[tk])) > 0.5 * a.sd_log) {
      cool_until_[tk] = now + 1000;
      cool_why_[tk] = "the underlying moved 0.5 sigma";
      quote_spot_.erase(tk);
    }
    // what stops new risk; the T-60 s flatten still goes
    const auto day = view->lane_day.find(lane_);
    std::string hold;
    if (p_.lane_pause > 0 && day != view->lane_day.end() && day->second <= -p_.lane_pause) hold = "lane paused: its day is past the loss limit";
    else if (p_.mo_after > 0 && env_.markout && env_.markout().first >= p_.mo_after && env_.markout().second < -p_.mo_floor)
      hold = "lane paused: its 60 s markout is below the floor";
    else if (m != snap->markets.end() && p_.event_cap > 0 && event_worst(*view, *snap, m->second.event_ticker) >= p_.event_cap)
      hold = "event cap";
    else if (now < cool_until_[tk]) hold = cool_why_[tk];
    if (!running) d.pull = "not running";
    else if (m == snap->markets.end()) d.pull = "not listed";
    else {
      MakerInput in;
      in.book = book ? &*book : nullptr;
      in.grid = &m->second.price_ranges;
      in.position = pos;
      in.now_ms = now;
      in.open_ms = m->second.open_ms.value_or(now);
      in.close_ms = m->second.close_ms.value_or(now);
      in.private_up = env_.private_up && env_.private_up();
      in.reference_fresh = a.fresh;
      if (a.gated) {
        in.anchor = a.fair ? a.fair : std::optional<Px>(0);
        in.reference_fresh = a.fresh && a.fair.has_value();
        if (a.fair) in.maker_fee = maker_fee(m->second, *a.fair);
      }
      d = decide_(in, p_);
      if (!hold.empty()) {
        d.quotes.erase(std::remove_if(d.quotes.begin(), d.quotes.end(), [](const MakerQuote& q) { return !q.ioc; }),
                       d.quotes.end());
        if (d.quotes.empty()) d.pull = hold;
      }
    }
    last_pull_[tk] = d.pull.empty() ? "quoting" : d.pull;
    // slot 0 bid, 1 ask, 2 the flatten
    std::map<int, MakerQuote> want;
    for (const auto& q : d.quotes) want[q.ioc ? 2 : q.side == Side::BUY ? 0 : 1] = q;
    for (int slot = 0; slot < 3; ++slot) {
      auto& s = slots[slot];
      const Gateway::SysOrder* o = nullptr;
      if (!s.cid.empty()) {
        const auto it = view->orders.find(s.cid);
        if (it != view->orders.end()) o = &it->second;
        // gone: ended, or never admitted (then its refusal already cleared it)
        if ((o && o->terminal) || (!o && now - s.acted_ms > 5000)) { s.cid.clear(); o = nullptr; }
      }
      const auto w = want.find(slot);
      if (o) {
        const bool keep = w != want.end() && w->second.price == o->price && w->second.size == o->count;
        if (!keep && !o->cancel_requested && now - s.acted_ms >= 300 && env_.post) {
          CancelIntent c;
          c.scope = CancelIntent::Scope::CID;
          c.cid = o->cid;
          if (env_.post(c)) { ++cancels_; s.acted_ms = now; }
        }
        continue;
      }
      if (w == want.end() || !s.cid.empty() || now < s.backoff_until || now - s.acted_ms < 500 || !env_.post) continue;
      PlaceIntent pi;
      pi.cid = new_cid();
      pi.owner = Owner::SYS;
      pi.lane = lane_;
      pi.ticker = tk;
      pi.side = w->second.side;
      pi.price = w->second.price;
      pi.count = w->second.size;
      pi.tif = w->second.ioc ? Tif::IOC : Tif::POST;
      pi.ttl_s = w->second.ioc ? 0 : p_.ttl_s;
      pi.reduce_only = w->second.ioc;
      pi.override_guard = w->second.ioc;   // the flatten crosses at the touch by design
      pi.t_recv_ms = now;
      if (book && !book->bids.empty() && !book->asks.empty())
        pi.touch = pi.side == Side::BUY ? book->bids.front().px : book->asks.front().px;
      pi.fair = d.fair;
      s.reply = std::make_shared<std::promise<OrderReply>>();
      s.answer = s.reply->get_future();
      pi.reply = s.reply;
      const auto cid = pi.cid;
      if (env_.post(std::move(pi))) {
        s.cid = cid;
        s.acted_ms = now;
        ++places_;
        if (a.spot > 0 && !quote_spot_.count(tk)) quote_spot_[tk] = a.spot;
      }
    }
    // a market no longer quoted and with nothing working is forgotten
    if (!running || !open.count(tk)) {
      bool idle = true;
      for (const auto& [slot, s] : slots) if (!s.cid.empty()) idle = false;
      if (idle && d.quotes.empty()) sides_.erase(tk);
    }
  }
}

json MakerEngine::status() const {
  json markets = json::object();
  for (const auto& [tk, why] : last_pull_) markets[tk] = why;
  return {{"lane", lane_}, {"places", places_}, {"cancels", cancels_}, {"refusals", refusals_}, {"last", markets}};
}

}  // namespace deskd
