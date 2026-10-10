// Markouts (strat/markouts.h): the mid at fill + 1/10/60 s from md's history, + in our favour.
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
  // 3 minutes kept, plus the mid in force before them
  m.mid("TK", T + 500000, 5400);
  CHECK(m.mid_at("TK", T + 400000) == Px{5300} && !m.mid_at("TK", T + 10));
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
    CHECK(rows[0] == nlohmann::json({{"fid", "fid-buy"}, {"h", "1s"}, {"mo", "0.0300"}, {"mid", "0.5200"}}));   // bought at 0.49
    CHECK(rows[1] == nlohmann::json({{"fid", "fid-sell"}, {"h", "1s"}, {"mo", "-0.0100"}, {"mid", "0.5200"}}));  // sold at 0.51
  }
  m.mid("TK", T + 9000, -1);      // stale at 10 s: skipped, never guessed
  CHECK(m.step(T + 10600).empty());
  m.mid("TK", T + 20000, 4800);
  rows = m.step(T + 60600);
  CHECK(rows.size() == 2 && rows[0]["h"] == "60s" && rows[0]["mo"] == "-0.0100" && rows[1]["mo"] == "0.0300");
  CHECK(m.step(T + 70000).empty());   // each once
  const auto st = m.status();
  CHECK(st["pending"] == 0 && st["lanes"]["M1"]["1s"]["fills"] == 1 && st["lanes"]["M1"]["1s"]["mean"] == "0.0300");
  CHECK(st["lanes"]["MAN"]["60s"]["mean"] == "0.0300" && !st["lanes"]["M1"].contains("10s"));
}
}  // namespace

int main() {
  history(); markouts();
  return finish("deskd-markouts");
}
