#pragma once
// What drives the fake's books: a recorded tape or a scripted scenario.
//
// The recorded tape is the old desk's archive of Kalshi's book socket, one gzip CSV per day,
// written by scripts/archive_quotes.py --table commodity_book_events:
//
//   id, ts_received_ms, ts_exchange_ms, ticker, kind, sequence, side, price, delta, raw_json
//
// Read as include/desk/reference_feed.h documents it (checked against 500,465 frames, 2026-09-23/24):
//   - rows in id order are receipt order; seq is per subscription across every market on it and
//     starts again at 1 on each connection;
//   - orderbook_snapshot: the levels are only in raw_json (msg.yes_dollars_fp / msg.no_dollars_fp,
//     pairs of [price_dollars, count_fp]; either side may be absent; an empty one is a closed
//     window or a market with no book);
//   - orderbook_delta: price (dollars), delta (contracts, negative removes), side 'yes'/'no';
//     raw_json is NULL on deltas the columns hold whole (scripts/backfill_book_columns.py --trim);
//     rows written before 2026-09-24 have price and delta 0 with raw_json intact, so raw_json wins
//     whenever it is there;
//   - ok, error, subscribed, unsubscribed (the venue's replies), connect, dropped and
//     paused_low_disk (the recorder's own markers) carry no book change and are counted, not played.
// Python's csv module quotes raw_json, which holds commas, quotes and the venue's trailing
// newline, so a record can span lines.
//
// A tape's clock is ts_received_ms. Playing it at `speed` from engine time T puts an event at
// T + (t - t0) / speed; a market's close time, read from its ticker, moves the same way.
//
// The scenario format is JSON lines, one event each, `t` in ms from the start:
//   {"t":0,"market":{"ticker":"KXNATGAS15M-26OCT061215-15","exchange_index":2,
//                    "ticks":"tapered_deci_cent","fee_type":"quadratic","fee_multiplier":1,
//                    "close_in_ms":900000}}
//   {"t":0,"snapshot":{"ticker":"...","yes":[["0.4500","100.00"]],"no":[["0.5300","80.00"]]}}
//   {"t":250,"delta":{"ticker":"...","side":"yes","price":"0.4600","delta":"10.00"}}
//   {"t":500,"trade":{"ticker":"...","taker_book_side":"ask","price":"0.4500","count":"5.00"}}
//   {"t":900,"pause":{"ticker":"..."}}   also "resume" and "close"; "#" lines are comments
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine.h"

namespace fakex {

struct TapeEvent {
  enum class Kind : std::uint8_t { SNAPSHOT, DELTA, TRADE, MARKET, PAUSE, RESUME, CLOSE };
  Kind kind = Kind::DELTA;
  std::int64_t t_ms = 0;
  std::string ticker;
  bool no_side = false;   // DELTA
  Px px = 0;              // DELTA: the side's own price; TRADE: the taker's YES limit
  Qty qty = 0;            // DELTA: signed; TRADE: contracts
  Side side = Side::BID;  // TRADE: the taker's book side
  std::vector<std::pair<Px, Qty>> yes, no;  // SNAPSHOT
  std::shared_ptr<MarketSpec> spec;          // MARKET: close_ms on the tape's clock
  std::int64_t seq = 0;   // the archive's, for reading a tape, not used to play it
};

struct Tape {
  std::vector<TapeEvent> events;              // in tape order
  std::map<std::string, std::size_t> kinds;   // every row by kind, the unplayed ones included
  std::size_t rows = 0;
  std::size_t seq_gaps = 0;                   // breaks in the archive's per-sid seq (not replies)
  std::int64_t t0() const { return events.empty() ? 0 : events.front().t_ms; }
  std::int64_t t1() const { return events.empty() ? 0 : events.back().t_ms; }
};

struct TapeFilter {
  std::string series;          // only tickers of this series ("KXNATGAS15M"); "" all
  std::int64_t from_ms = 0, to_ms = 0;  // on the tape's clock; 0 open
  std::size_t max_events = 0;  // 0: all. A day of the five commodity series is millions of rows.
};

// A .csv or .csv.gz in the archive's format.
bool load_archive_csv(const std::string& path, Tape& out, std::string* err, const TapeFilter& f = {});
bool load_scenario(const std::string& path, Tape& out, std::string* err);
bool parse_scenario(const std::string& text, Tape& out, std::string* err);

// A 15-minute (or hourly) ticker's close in UTC ms: KXNATGAS15M-26SEP241145-45 closes at
// 2026-09-24 11:45 New York time, 15:45Z (the recording's 11:30 windows closed at 15:30:00Z).
std::optional<std::int64_t> close_ms_from_ticker(const std::string& ticker);
// A market a tape names but does not describe: the 15-minute commodity and crypto series live on
// shard 2 with the tapered tenth-of-a-cent grid and no maker fee; anything else on shard 0 with a
// one-cent grid (redesign_all kalshi K7, K12). FX 15-minute series sit on shard 0 with 1c ticks.
MarketSpec default_spec(const std::string& ticker);

}  // namespace fakex
