// strat/lip: our share of a liquidity incentive program, worked by hand under its stated rule
// (target size from the best price outward, discount per cent away, both sides at target, ours
// at the back of a level), the window's estimate, and the programs' wire shape.
#include <cmath>

#include "check.h"
#include "deskd/strat/lip.h"

using namespace deskd;
using json = nlohmann::json;

namespace {

const Qty C = kOneContract;
bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

MdBook book(std::vector<MdLevel> bids, std::vector<MdLevel> asks) {
  MdBook b;
  b.ticker = "KXNATGAS15M-26OCT151430-30";
  b.bids = std::move(bids);
  b.asks = std::move(asks);
  b.stale = false;   // a live book
  return b;
}

void shares() {
  // bids 0.40 x200 (100 of them ours), 0.39 x200, 0.38 x500; asks 0.42 x300; target 300, half a
  // cent away counts half
  const auto b = book({{4000, 200 * C}, {3900, 200 * C}, {3800, 500 * C}}, {{4200, 300 * C}});
  auto s = lip_share(b, {{true, 4000, 100 * C}}, 300 * C, 5000);
  // bid side: 200 at 0.40 (ours 100 of them) + 100 of 0.39 at half = 250; ours 100
  CHECK(s.scores && near(s.bid, 100.0 / 250) && near(s.ask, 0) && near(s.share(), 0.2));
  // ours at the back of a level the target cuts: asks 0.42 x400, ours 200 of them, target 300:
  // the first 300 are the 200 ahead of us and 100 of ours
  s = lip_share(book({{4000, 300 * C}}, {{4200, 400 * C}}), {{false, 4200, 200 * C}}, 300 * C, 5000);
  CHECK(s.scores && near(s.ask, 100.0 / 300) && near(s.bid, 0));
  // one side short of the target: nothing scores (the shares are still worked out)
  s = lip_share(book({{4000, 100 * C}}, {{4200, 400 * C}}), {{true, 4000, 100 * C}}, 300 * C, 5000);
  CHECK(!s.scores && near(s.share(), 0) && near(s.bid, 1.0));
  // no discount: only the best price counts, so ours a cent behind earns nothing
  s = lip_share(book({{4000, 300 * C}, {3900, 300 * C}}, {{4200, 300 * C}}), {{true, 3900, 100 * C}}, 300 * C, 0);
  CHECK(s.scores && near(s.bid, 0));
  // all of it ours on both sides
  s = lip_share(book({{4000, 300 * C}}, {{4200, 300 * C}}), {{true, 4000, 300 * C}, {false, 4200, 300 * C}}, 300 * C, 5000);
  CHECK(s.scores && near(s.share(), 1.0));
}

void meter() {
  const std::int64_t T0 = 1791000000000;
  kalshi::IncentiveRow p;
  p.market_ticker = "KXNATGAS15M-26OCT151430-30";
  p.type = "liquidity";
  p.period_reward = 20 * 10000;   // $20 over the 15 minutes
  p.target_size = 300 * C;
  p.discount_bps = 5000;
  p.start_ms = T0;
  p.end_ms = T0 + 900000;
  const auto b = book({{4000, 200 * C}, {3900, 200 * C}, {3800, 500 * C}}, {{4200, 300 * C}});
  const std::vector<LipOwn> ours{{true, 4000, 100 * C, "M1"}};
  LipMeter m;
  m.sample(p.market_ticker, p, b, ours, T0 - 1000);                 // before the window: nothing
  for (int i = 0; i < 10; ++i) m.sample(p.market_ticker, p, b, ours, T0 + i * 1000);
  m.sample(p.market_ticker, p, b, ours, T0 + 9500);                 // the same second again: once
  auto stale = b;
  stale.stale = true;
  m.sample(p.market_ticker, p, stale, ours, T0 + 10000);            // a stale book is no snapshot
  auto st = m.status();
  CHECK(st.contains(p.market_ticker) && st[p.market_ticker]["seconds"] == 10 && st[p.market_ticker]["scored_s"] == 10 &&
        st[p.market_ticker]["share_now"] == 0.2);
  CHECK(m.finished(T0 + 899999).empty());
  const auto rows = m.finished(T0 + 900000);
  CHECK(rows.size() == 1);
  if (rows.size() == 1) {
    // 10 seconds at a fifth of $20 / 900 s: $0.0444
    CHECK(rows[0]["mean_share"] == 0.2 && rows[0]["est"] == "0.0444" && rows[0]["reward"] == "20.0000" &&
          rows[0]["quoted_s"] == 10 && rows[0]["target"] == 300 && rows[0]["discount_bps"] == 5000 &&
          rows[0]["start_ms"] == T0);
  }
  CHECK(m.finished(T0 + 900000).empty() && m.status().empty());   // it came back once
  // the day's estimate per lane: 10 scored seconds at a fifth of $20 / 900 s, all M1's
  const auto lanes = m.lane_day(T0 + 900000);
  CHECK(lanes.size() == 1 && lanes.count("M1") && lanes.at("M1") == 444);
  CHECK(m.lane_day(T0 + 2 * 86400000LL).empty());   // another ET day: nothing yet
  // two lanes resting there: the seconds' dollars split by their size (3 to 1)
  LipMeter two;
  const std::vector<LipOwn> both{{true, 4000, 75 * C, "M1"}, {true, 4000, 25 * C, "D1"}};
  for (int i = 0; i < 9; ++i) two.sample(p.market_ticker, p, b, both, T0 + i * 1000);
  const auto split = two.lane_day(T0 + 9000);
  CHECK(split.size() == 2 && split.at("M1") == 300 && split.at("D1") == 100);
}

void wire() {
  std::string why;
  const json body = {
      {"incentive_programs",
       {{{"market_ticker", "KXNATGAS15M-26OCT151430-30"}, {"incentive_type", "liquidity"}, {"period_reward", 200000},
         {"target_size_fp", "300.00"}, {"discount_factor_bps", 5000}, {"start_date", "2026-10-15T14:15:00Z"},
         {"end_date", "2026-10-15T14:30:00Z"}, {"paid_out", false}},
        // a perpetuals volume program: other fields, never read
        {{"market_ticker", "BTCPERP"}, {"incentive_type", "margin_maker_volume"}, {"period_reward", "n/a"}}}},
      {"next_cursor", "abc"}};
  auto page = kalshi::parse_incentive_programs(body.dump(), &why);
  CHECK(page && page->rows.size() == 2 && page->cursor == "abc");
  if (page && page->rows.size() == 2) {
    const auto& l = page->rows[0];
    CHECK(l.type == "liquidity" && l.period_reward == 200000 && l.target_size == 300 * C && l.discount_bps == 5000 &&
          l.end_ms - l.start_ms == 900000);
    CHECK(page->rows[1].type == "margin_maker_volume" && page->rows[1].market_ticker == "BTCPERP");
  }
  // a liquidity program that breaks its shape fails the page: loud
  json bad = body;
  bad["incentive_programs"][0]["end_date"] = "2026-10-15T14:00:00Z";   // ends before it starts
  CHECK(!kalshi::parse_incentive_programs(bad.dump(), &why) && !why.empty());
  bad = body;
  bad["incentive_programs"][0]["period_reward"] = "20";
  CHECK(!kalshi::parse_incentive_programs(bad.dump(), &why));
  // an empty list, and no cursor
  page = kalshi::parse_incentive_programs(R"({"incentive_programs": []})", &why);
  CHECK(page && page->rows.empty() && page->cursor.empty());
  const auto req = kalshi::build_get_incentive_programs("active", "c1", 1000);
  CHECK(!req.auth && req.path == "/trade-api/v2/incentive_programs" && req.query.find("status=active") != std::string::npos &&
        req.query.find("cursor=c1") != std::string::npos && req.query.find("limit=1000") != std::string::npos);
}

}  // namespace

int main() {
  shares();
  meter();
  wire();
  return finish("deskd-lip");
}
