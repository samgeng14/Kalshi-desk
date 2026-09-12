#include "desk/universe.h"

#include "desk/log.h"

#include <cstring>
#include <map>

#include "desk/timeutil.h"

namespace desk {

// Verified by reading Kalshi's own rules text on 2026-09-06; see docs/settlement.md for the
// evidence, including the check that The Weather Company's settled value equals the NWS
// single-station climate maximum. A product of "" means nobody has read that series' rules, and
// the strategies treat that as research-only rather than guessing.
const std::vector<SettlementSpec>& settlement_specs() {
  static const std::vector<SettlementSpec> v = {
      {"KXHIGHAUS", "weather_cli", "CLIAUS", "KAUS", "America/Chicago", false, false},
      {"KXHIGHCHI", "weather_cli", "CLIMDW", "KMDW", "America/Chicago", false, false},
      {"KXHIGHNY", "weather_cli", "CLINYC", "KNYC", "America/New_York", true, false},
      {"KXHIGHMIA", "weather_cli", "CLIMIA", "KMIA", "America/New_York", false, false},
      {"KXHIGHPHIL", "weather_cli", "CLIPHL", "KPHL", "America/New_York", false, false},
      {"KXHIGHLAX", "weather_cli", "CLILAX", "KLAX", "America/Los_Angeles", false, false},
      {"KXLOWTCHI", "weather_cli", "CLIMDW", "KMDW", "America/Chicago", false, true},
      {"KXLOWTMIN", "weather_cli", "CLIMSP", "KMSP", "America/Chicago", false, true},
      // Series that exist on Kalshi but had no listed market to read on 2026-09-06.
      {"KXHIGHHOU", "weather_cli", "", "KIAH", "America/Chicago", false, false},
      {"KXDENHIGH", "weather_cli", "", "KDEN", "America/Denver", false, false},
      // Soccer, one league to start as the plan asks. Read from the rules on 2026-09-06: these
      // settle on the result "after 90 minutes plus stoppage time (does not include extra time
      // or penalties)", which is why MatchPhase::Stoppage is not a tradable phase - a model that
      // priced extra time would be pricing something the contract does not pay on.
      {"KXBRASILEIROGAME", "soccer_90", "bra.1", "", "America/Sao_Paulo", false, false},
  };
  return v;
}

// Discovered specs live beside the compiled table rather than inside it, so that reading the
// source still shows exactly which mappings a person verified by hand. The strings are owned here
// because SettlementSpec holds `const char*`: a spec pointing into a temporary json would dangle
// the moment config went out of scope, and the pointer would still look valid.
static std::vector<std::string>& spec_strings() {
  static std::vector<std::string> v;
  v.reserve(4096);
  return v;
}
static std::vector<SettlementSpec>& discovered_specs() {
  static std::vector<SettlementSpec> v;
  return v;
}
static const char* own(const std::string& s) {
  spec_strings().push_back(s);
  return spec_strings().back().c_str();
}

int add_settlement_specs(const nlohmann::json& j) {
  if (!j.is_object()) return 0;
  int added = 0;
  for (const auto& [series, v] : j.items()) {
    if (!v.is_object()) continue;
    if (settlement_for(series)) continue;          // hand-read mappings always win
    const std::string kind = v.value("kind", ""), product = v.value("product", ""),
                      station = v.value("station", ""), tz = v.value("tz", "");
    if (series.empty() || kind.empty() || product.empty() || station.empty() || tz.empty()) {
      LOG_WARN("settlement spec for %s ignored: needs kind, product, station and tz", series.c_str());
      continue;
    }
    SettlementSpec s{};
    s.series = own(series);
    s.kind = own(kind);
    s.product = own(product);
    s.station = own(station);
    s.tz = own(tz);
    s.sparse = v.value("sparse", false);
    s.low = v.value("low", false);
    discovered_specs().push_back(s);
    ++added;
  }
  if (added) LOG_INFO("settlement: %d discovered mappings added", added);
  return added;
}

const SettlementSpec* settlement_for(const std::string& series) {
  for (const auto& s : settlement_specs())
    if (series == s.series) return &s;
  for (const auto& s : discovered_specs())
    if (series == s.series) return &s;
  return nullptr;
}

const char* phase_label(ContractPhase p) {
  switch (p) {
    case ContractPhase::Future: return "future";
    case ContractPhase::Tomorrow: return "tomorrow";
    case ContractPhase::Today: return "today";
    case ContractPhase::DayDone: return "day_done";
    case ContractPhase::AwaitingSettlement: return "awaiting_settlement";
    case ContractPhase::Settled: return "settled";
    default: return "unknown";
  }
}

ContractPhase phase_from_label(const std::string& s) {
  for (ContractPhase p : {ContractPhase::Future, ContractPhase::Tomorrow, ContractPhase::Today,
                          ContractPhase::DayDone, ContractPhase::AwaitingSettlement,
                          ContractPhase::Settled})
    if (s == phase_label(p)) return p;
  return ContractPhase::Unknown;
}

ContractPhase classify_contract(const ContractTimes& t, long long now) {
  if (!t.result.empty() || t.status == "settled" || t.status == "finalized")
    return ContractPhase::Settled;
  // Without a day or a zone we cannot say which day this is about, and guessing is what produces
  // a stale selection three hours after somebody else's midnight.
  if (t.local_date.empty() || t.tz.empty()) return ContractPhase::Unknown;
  // Trading stops at whichever comes first. Weather closes an hour after local midnight and
  // settles at 19:00Z the next day, so close is the binding time; a soccer market carries a
  // close three days out as a backstop and an expected expiration hours after kick-off, so there
  // expiration is the binding one. Taking the later of them would leave a settled match sitting
  // in the universe as tradable for days.
  long long stop = t.close_ts;
  if (t.expiration_ts > 0 && (stop == 0 || t.expiration_ts < stop)) stop = t.expiration_ts;
  if (t.status == "closed" || (stop > 0 && now >= stop)) return ContractPhase::AwaitingSettlement;
  // Dates are ISO, so lexical order is chronological order.
  std::string today = trading_day(t.tz, now);
  if (t.local_date > today)
    return t.local_date == trading_day(t.tz, now + 86400) ? ContractPhase::Tomorrow
                                                          : ContractPhase::Future;
  if (t.local_date == today) return ContractPhase::Today;
  return ContractPhase::DayDone;
}

bool parse_settlement(const std::string& rules, std::string* source, std::string* product) {
  std::string src, prod;
  for (const char* lead : {"according to ", "as reported by "}) {
    auto p = rules.find(lead);
    if (p == std::string::npos) continue;
    p += std::strlen(lead);
    // Stop at the end of the clause, not at the first bracket: the older wording ends
    // "Climatological Report (Daily), then the market resolves to Yes."
    auto end = rules.find_first_of(",.", p);
    src = rules.substr(p, end == std::string::npos ? std::string::npos : end - p);
    if (src.rfind("the ", 0) == 0) src = src.substr(4);
    break;
  }
  auto o = rules.find("(CLI");
  if (o != std::string::npos) {
    auto close = rules.find(')', o);
    if (close != std::string::npos) prod = rules.substr(o + 1, close - o - 1);
  }
  if (source) *source = src;
  if (product) *product = prod;
  return !src.empty() || !prod.empty();
}

std::vector<UniverseRow> build_universe(const std::vector<Market>& markets, long long now) {
  std::vector<UniverseRow> out;
  out.reserve(markets.size());
  for (const auto& m : markets) {
    UniverseRow r;
    r.ticker = m.ticker;
    r.event_ticker = m.event_ticker;
    r.series = m.series_ticker;
    r.local_date = event_date(m.event_ticker);
    const SettlementSpec* spec = settlement_for(m.series_ticker);
    r.tz = spec ? spec->tz : "";
    parse_settlement(m.rules_primary, &r.settle_source, &r.settle_product);
    // The rules are the authority. When they name a product, that is what this contract settles
    // on even if our table disagrees; a mismatch is a finding, not something to paper over.
    if (r.settle_product.empty() && spec) r.settle_product = spec->product;
    r.close_ts = parse_iso(m.close_time);
    r.expiration_ts = parse_iso(m.expected_expiration_time);
    ContractTimes t{r.local_date, r.tz, r.close_ts, r.expiration_ts, m.status, m.result};
    r.phase = classify_contract(t, now);
    out.push_back(std::move(r));
  }
  return out;
}

void save_universe(Store& store, const std::vector<UniverseRow>& rows, long long now) {
  for (const auto& r : rows)
    store.exec(
        "INSERT INTO contract_universe(ticker,event_ticker,series_ticker,local_date,tz,"
        "settle_source,settle_product,phase,close_ts,expiration_ts,refreshed_ts) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(ticker) DO UPDATE SET "
        "local_date=excluded.local_date,tz=excluded.tz,settle_source=excluded.settle_source,"
        "settle_product=excluded.settle_product,phase=excluded.phase,close_ts=excluded.close_ts,"
        "expiration_ts=excluded.expiration_ts,refreshed_ts=excluded.refreshed_ts",
        {r.ticker, r.event_ticker, r.series, r.local_date, r.tz, r.settle_source, r.settle_product,
         std::string(phase_label(r.phase)), r.close_ts, r.expiration_ts, now});
}

}  // namespace desk
