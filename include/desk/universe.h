#pragma once
// The tradable universe: which contracts the desk will consider, what each one settles on, and
// where each sits in its own day. Discovery that rolls over on one clock for every market serves
// stale selections: a New York contract and a Los Angeles contract change day three hours apart.
#include <string>
#include <vector>

#include "desk/store.h"
#include "desk/types.h"

namespace desk {

// What a series settles on, read from Kalshi's rules text and recorded once. This is the single
// source of truth for the settlement map; docs/settlement.md is its prose form.
struct SettlementSpec {
  const char* series;
  const char* kind;      // "weather_cli" | "soccer_90"
  const char* product;   // NWS daily climate product, or the ESPN league slug; "" = rules unread
  const char* station;   // the observing station the desk polls (weather only)
  const char* tz;        // IANA zone the contract's day is measured in
  bool sparse;           // site files far fewer observations a day than an airport ASOS
  bool low;              // series is a daily minimum
};

const std::vector<SettlementSpec>& settlement_specs();
const SettlementSpec* settlement_for(const std::string& series);

// Where a contract sits relative to the day it is about, judged in that contract's own zone.
enum class ContractPhase {
  Unknown = 0,         // no settlement mapping, or times we cannot read
  Future,              // its day is beyond tomorrow
  Tomorrow,
  Today,               // its day, still running
  DayDone,             // its day has ended locally, trading has not closed yet
  AwaitingSettlement,  // closed; the settling value is not in yet
  Settled,
};

const char* phase_label(ContractPhase p);
ContractPhase phase_from_label(const std::string& s);

struct ContractTimes {
  std::string local_date;       // "YYYY-MM-DD", the day the contract is about
  std::string tz;               // zone that day is measured in
  long long close_ts = 0;       // trading stops
  long long expiration_ts = 0;  // Kalshi's expected settlement
  std::string status;           // Kalshi's own market status
  std::string result;           // "" until settled
};

ContractPhase classify_contract(const ContractTimes& t, long long now);

// Pulls the settlement source and product identifier out of the rules text Kalshi ships with a
// market. False when the text names neither, which is itself worth recording.
bool parse_settlement(const std::string& rules, std::string* source, std::string* product);

struct UniverseRow {
  std::string ticker, event_ticker, series, local_date, tz;
  std::string settle_source;   // "The Weather Company", "the National Weather Service's ..."
  std::string settle_product;  // "CLIMDW", "" when the rules name none
  ContractPhase phase = ContractPhase::Unknown;
  long long close_ts = 0, expiration_ts = 0;
};

// Builds the universe from what the desk currently knows about these markets. A contract whose
// settlement cannot be read is kept and marked, never quietly dropped: the gap is the finding.
std::vector<UniverseRow> build_universe(const std::vector<Market>& markets, long long now);
void save_universe(Store& store, const std::vector<UniverseRow>& rows, long long now);

}  // namespace desk
