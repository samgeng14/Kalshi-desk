// Markouts (strat/markouts.h): the mid at fill + 1/10/60/300 s from md's history, + in our favour,
// gross and net of the fill's fee; only the contract's horizons go on the stream.
#include "check.h"
#include "deskd/strat/markouts.h"
using namespace deskd;
namespace {
const std::int64_t T = 1791000000000;

void history() {
  Markouts m;
  CHECK(!m.mid_at("TK", T));
  m.mid("TK", T, 5000);
  m.mid("TK", T + 50, 5100);       // the same 200 ms slice: its latest mid
  CHECK(m.mid_at("TK", T) == Px{5100});
  m.mid("TK", T + 5000, 5200);
  m.mid("TK", T + 9000, 5200);     // unchanged: no new sample, the last still holds
  CHECK(m.mid_at("TK", T + 4999) == Px{5100} && m.mid_at("TK", T + 60000) == Px{5200});
  m.mid("TK", T + 70000, -1);      // the book went stale
  CHECK(!m.mid_at("TK", T + 75000));
  m.mid("TK", T + 80000, 5300);
  CHECK(m.mid_at("TK", T + 80000) == Px{5300});
  m.mid("TK", T + 4000, 4000);     // out of order: ignored
  CHECK(m.mid_at("TK", T + 4999) == Px{5100});
  // 330 s kept (the 300 s horizon, read late), plus the mid in force before them
  m.mid("TK", T + 400000, 5400);
  CHECK(m.mid_at("TK", T + 60000) == Px{5200} && m.mid_at("TK", T + 400000) == Px{5400});
  m.mid("TK", T + 500000, 5500);
  CHECK(m.mid_at("TK", T + 170000) == Px{5300} && m.mid_at("TK", T + 400000) == Px{5400} && !m.mid_at("TK", T + 79999));
}

void markouts() {
  Markouts m;
  m.mid("TK", T - 1000, 5000);
  m.fill("fid-buy", "M1", "TK", Side::BUY, 4900, 1000, T);
  m.fill("fid-sell", "", "TK", Side::SELL, 5100, 500, T);
  m.fill("fid-buy", "M1", "TK", Side::BUY, 4900, 1000, T);   // seen: once
  m.mid("TK", T + 800, 5200);     // the mid moves up 2c before 1 s
  CHECK(m.step(T + 1400).empty());   // half a second's grace after the horizon
  auto rows = m.step(T + 1600);
  CHECK(rows.size() == 2);
  if (rows.size() == 2) {
    CHECK(rows[0].pub() == nlohmann::json({{"fid", "fid-buy"}, {"h", "1s"}, {"mo", "0.0300"}, {"mid", "0.5200"}}));   // bought at 0.49
    CHECK(rows[1].pub() == nlohmann::json({{"fid", "fid-sell"}, {"h", "1s"}, {"mo", "-0.0100"}, {"mid", "0.5200"}}));  // sold at 0.51
    CHECK(rows[0].on_stream() && rows[0].net == rows[0].mo && rows[0].lane == "M1" && rows[0].qty == 1000);   // no fee
  }
  m.mid("TK", T + 9000, -1);      // stale at 10 s: skipped, never guessed
  CHECK(m.step(T + 10600).empty());
  m.mid("TK", T + 20000, 4800);
  rows = m.step(T + 60600);
  CHECK(rows.size() == 2 && rows[0].h == "60s" && rows[0].mo == -100 && rows[1].mo == 300 && rows[0].on_stream());
  CHECK(m.step(T + 70000).empty());   // each once
  // 300 s, from the mid then in force: marked and stored, never on the stream
  m.mid("TK", T + 290000, 5000);
  CHECK(m.step(T + 300400).empty());
  rows = m.step(T + 300600);
  CHECK(rows.size() == 2 && rows[0].h == "300s" && rows[0].mo == 100 && rows[1].mo == 100 && rows[0].mid == 5000);
  CHECK(rows.size() == 2 && !rows[0].on_stream() && !rows[1].on_stream());
  CHECK(m.step(T + 400000).empty());
  auto st = m.status();   // not const: a missing row reads null, a failed check, not a throw
  CHECK(st["pending"] == 0 && st["lanes"]["M1"]["1s"]["fills"] == 1 && st["lanes"]["M1"]["1s"]["mean"] == "0.0300");
  CHECK(st["lanes"]["M1"]["1s"]["net"] == "0.0300" && st["lanes"]["M1"]["300s"]["mean"] == "0.0100");
  CHECK(st["lanes"]["MAN"]["60s"]["mean"] == "0.0300" && !st["lanes"]["M1"].contains("10s"));
  const auto mean = m.lane_mean("M1", "300s");
  CHECK(mean.fills == 1 && mean.mean == 100 && mean.net == 100);
}

// Net of the fill's fee at every horizon, on any market key (a Polymarket "P|slug" too); the
// strat loop's rule: every row stored, only the contract's horizons published.
void net() {
  Markouts m;
  const std::string pk = "P|nba-lal-bos-2026-11-02";
  m.mid(pk, T - 1000, 4000);
  m.fill("f-whole", "C1", pk, Side::BUY, 4000, 300, T, 600);     // 3 contracts, 6c of fees: 2c a contract
  m.fill("f-odd", "C1", pk, Side::BUY, 4000, 700, T, 1000);      // 7 for 10c: 1.43c a contract, rounded up
  m.fill("f-rebate", "Q1", pk, Side::SELL, 4200, 100, T, -50);   // a maker rebate adds to it
  m.fill("f-half", "", pk, Side::BUY, 4000, 50, T, 100);         // half a contract for 1c: 2c a contract
  m.mid(pk, T + 200000, 4500);   // up 5c after the 60 s marks
  std::vector<MarkoutRow> all, stream;
  std::vector<nlohmann::json> stored;
  for (std::int64_t t = T; t <= T + 302000; t += 250)
    for (const auto& r : m.step(t)) {
      all.push_back(r);
      stored.push_back(r.store());
      if (r.on_stream()) stream.push_back(r);
    }
  CHECK(all.size() == 16 && stored.size() == 16 && stream.size() == 12);
  for (const auto& r : stream) CHECK(r.h != "300s" && r.pub().size() == 4);
  auto row = [&](const std::string& fid, const std::string& h) {
    for (const auto& r : all) if (r.fid == fid && r.h == h) return r;
    return MarkoutRow{};
  };
  const auto w = row("f-whole", "300s");
  CHECK(w.lane == "C1" && w.mo == 500 && w.net == 300 && w.mid == 4500 && w.qty == 300);
  CHECK(w.pub() == nlohmann::json({{"fid", "f-whole"}, {"h", "300s"}, {"mo", "0.0500"}, {"mid", "0.4500"}}));
  CHECK(row("f-whole", "1s").mo == 0 && row("f-whole", "1s").net == -200);
  CHECK(row("f-odd", "1s").net == -143 && row("f-odd", "300s").net == 357);
  CHECK(row("f-odd", "300s").store() == nlohmann::json({{"fid", "f-odd"}, {"h", "300s"}, {"mo", "0.0500"}, {"mid", "0.4500"},
                                                        {"lane", "C1"}, {"net", "0.0357"}, {"qty", 7}}));
  CHECK(row("f-rebate", "60s").mo == 200 && row("f-rebate", "60s").net == 250);
  CHECK(row("f-rebate", "300s").mo == -300 && row("f-rebate", "300s").net == -250);
  CHECK(row("f-half", "300s").net == 300 && row("f-half", "300s").store()["lane"].is_null() &&
        row("f-half", "300s").store()["qty"] == 0.5);
  // the lanes' means, contract-weighted: C1's 300 s net is (3 x 3c + 7 x 3.57c) / 10
  const auto c300 = m.lane_mean("C1", "300s"), c1 = m.lane_mean("C1", "1s");
  CHECK(c300.fills == 2 && c300.mean == 500 && c300.net == 339);
  CHECK(c1.fills == 2 && c1.mean == 0 && c1.net == -160);
  CHECK(m.lane_mean("Q1", "60s").mean == 200 && m.lane_mean("Q1", "60s").net == 250);
  CHECK(m.lane_mean("MAN", "300s").net == 300 && m.lane_mean("X1", "300s").fills == 0);
  auto st = m.status();
  CHECK(st["pending"] == 0 &&
        st["lanes"]["C1"]["300s"] == nlohmann::json({{"fills", 2}, {"mean", "0.0500"}, {"net", "0.0339"}}));
}
}  // namespace

int main() {
  history(); markouts(); net();
  return finish("deskd-markouts");
}
