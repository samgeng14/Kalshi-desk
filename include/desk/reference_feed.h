#pragma once
// The settlement reference for the short-duration commodity contracts, as it arrives.
//
// Kalshi's 15-minute gold, silver, WTI and natural gas markets settle on a Pyth index value, and
// Kalshi publishes that value on its own socket (channel `pyth_value`). This class keeps that
// socket up, holds the recent observations in memory for the model, appends them to
// `commodity_reference` for replay, and answers two questions the router cares about: what is the
// value now, and is the feed fresh enough to trade on.
//
// It also checks itself. At every window open Kalshi publishes a strike, which by the contract is
// the one-minute candle close of the same feed. `replication()` compares that strike with the
// candle this feed computed. Until that matches, the models stay RESEARCH: a probability on a feed
// that is not the settlement feed is a guess about the wrong number.
//
// Threading: the socket runs on its own thread and never touches the store. `flush()` is called
// from the engine's cycle and is the only writer. Readers take a lock and copy.
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "desk/store.h"
#include "desk/ws_client.h"

namespace desk {

class Signer;

struct ReferenceFeedSettings {
  bool enabled = true;
  std::string ws_url;                      // empty: the feed does not run
  std::string sign_path = "/trade-api/ws/v2";
  std::vector<std::string> subscribe = {"all"};   // Kalshi's underlying_tickers
  int record_min_interval_ms = 1000;       // per symbol; 0 records every observation
  int history_seconds = 5400;              // kept in memory per symbol (90 minutes)
  int stale_after_seconds = 20;            // a value older than this is not tradable
  // The first frames of a run, kept byte for byte in `commodity_reference_raw`, plus every frame
  // that surprised the parser. The schema of this channel is documented but has never been seen
  // by this desk, and a schema you have inferred from your own parser is not evidence.
  int raw_frames_to_keep = 100;
  // How long a silence has to be before the desk stops trusting the recency of the value it is
  // holding. This is a statement about OUR observation and nothing else.
  //
  // Kalshi documents this channel as deduplicated. That tells us repeats are removed; it does not
  // tell us whether a message is emitted on every scheduled source update when the value has not
  // changed. So a silence has two possible explanations - the source published nothing, or the
  // value did not change and the venue sent nothing - and this desk cannot presently tell them
  // apart. It therefore does not claim to. `observed_value_fresh` means "we saw a message for
  // this symbol recently enough to stand behind the number", which is all that is needed to
  // decide whether to trade on it, and the counter below is what would settle the question:
  // a single message carrying a value identical to the previous one proves the channel does NOT
  // suppress unchanged values, and the interpretation can be strengthened.
  double long_gap_multiple = 8.0;
  int long_gap_floor_ms = 2000;            // never call a gap long below this, whatever the cadence
  int ws_stale_after_seconds = 45;         // socket silence that counts as a dead socket
  int ping_interval_seconds = 10;
  size_t max_pending_rows = 20000;         // if the cycle stalls, drop and count rather than grow
  std::string name = "pyth";
};

ReferenceFeedSettings reference_feed_settings(const nlohmann::json& params, const std::string& env);

class ReferenceFeed {
 public:
  struct Value {
    double price = 0;
    long long source_ts_ms = 0;       // the source's own clock, as the venue relayed it
    long long received_ts_ms = 0;     // ours: when this message arrived
    long long sequence = 0;
    // Two different ages, because they answer two different questions and conflating them is how
    // a desk teaches itself that a quiet market is a broken feed.
    long long message_gap_ms = 0;     // since the PREVIOUS MESSAGE for this symbol
    long long unchanged_age_ms = 0;   // since the value last actually changed
    long long value_change_ts_ms = 0; // when that was
    bool value_changed = true;        // did this message carry a different number?
    // Whether we can stand behind the recency of this value. False means the message gap exceeded
    // the limit - not that the source stopped publishing, which we cannot see from here.
    bool observed_value_fresh = true;
  };
  struct Replication {
    long long checked_ts = 0;    // when we ran the check (wall clock)
    long long window_open = 0;   // the window whose strike was checked. Not the same thing: one
                                 // is our clock, the other is the contract's, and confusing them
                                 // makes "already checked" true for every window still open.
    std::string symbol;
    double published_strike = 0, computed_close = 0, error = 0;
    bool matched = false;
    // How many windows in a row this feed has reproduced, and how many have been checked at all.
    // One match is a coincidence on a quiet minute; a run of them is evidence.
    long long consecutive_matches = 0, windows_checked = 0, windows_matched = 0;
  };

  ReferenceFeed(Store& store, ReferenceFeedSettings settings, std::shared_ptr<Signer> signer);
  ~ReferenceFeed();

  void start();
  void stop();
  bool running() const;

  // Thread-safe reads for the model and the router.
  std::optional<Value> latest(const std::string& symbol) const;
  std::vector<std::pair<long long, double>> ticks(const std::string& symbol, long long from_ts) const;
  std::optional<double> candle_close(const std::string& symbol, long long minute_end) const;
  std::vector<std::string> symbols() const;
  // The symbol carrying an underlying, by pattern: "gold" matches Metal.Index.GOLD/USD whatever
  // Kalshi calls it this month. Empty when nothing seen matches.
  std::string symbol_for(const std::vector<std::string>& patterns) const;

  // What the stream has actually been sending: every field seen under `msg`, with its JSON type
  // and a count. This is the observed schema, as opposed to the documented one.
  nlohmann::json observed_schema() const;
  // The underlyings the venue itself says it has streamed recently (the channel's own
  // `underlying_list` action), rather than the ones we guessed from a pattern.
  std::vector<std::string> advertised_underlyings() const;

  // Record that a published strike did (or did not) match this feed's own candle close.
  void note_replication(const std::string& symbol, double published_strike, double computed_close,
                        double tolerance, long long now, long long window_open = 0);
  std::optional<Replication> replication(const std::string& symbol) const;

  // Called from the engine's cycle: appends what the socket collected. The only DB writer.
  int flush(long long now);
  nlohmann::json status() const;
  const ReferenceFeedSettings& settings() const { return s_; }

  // Exposed for tests: hand a message to the feed as though the socket had delivered it.
  void on_message_for_tests(const std::string& text, long long received_ms) { on_text(text, received_ms); }

 private:
  void on_text(const std::string& text, long long received_ms);

  Store& store_;
  ReferenceFeedSettings s_;
  std::shared_ptr<Signer> signer_;
  std::unique_ptr<WsClient> ws_;

  mutable std::mutex mu_;
  struct Series {
    std::deque<std::pair<long long, double>> ticks;   // (unix seconds, price), oldest first
    Value last;
    long long last_recorded_ms = 0;
    // The symbol's own measured cadence, so the limit is judged against what this feed does
    // rather than against a number someone typed. Median of the recent inter-arrival times.
    std::deque<long long> gaps;
    long long long_message_gaps = 0, longest_gap_ms = 0;
    // The measurement that would settle what a silence means. If this is ever non-zero the
    // channel emits unchanged values, so a silence really is the source going quiet; if it stays
    // at zero over a long recording, silence and "no change" remain indistinguishable from here.
    long long unchanged_value_messages = 0, value_changes = 0;
    long long value_change_ts_ms = 0;
    // The newest observation of the minute now in progress, held back until a later minute proves
    // it was that minute's close. `commodity_reference` is the replay's only record of the feed,
    // and the settlement candle is the one value in it that must never be the one we dropped.
    long long pending_minute = 0, pending_close_recv_ms = 0;
    std::optional<nlohmann::json> pending_close;
    long long messages = 0, out_of_order = 0, duplicates = 0;
    std::optional<Replication> replication;
  };
  std::map<std::string, Series> series_;
  std::vector<nlohmann::json> pending_;       // rows waiting for the cycle to write them
  std::vector<nlohmann::json> pending_raw_;   // frames waiting for the cycle to write them
  long long dropped_rows_ = 0, messages_ = 0, unparsed_ = 0, recorded_ = 0, write_failures_ = 0;
  long long raw_kept_ = 0, schema_surprises_ = 0;
  std::map<std::string, std::pair<std::string, long long>> schema_;   // field -> (json type, count)
  std::vector<std::string> advertised_;
  void note_schema(const nlohmann::json& msg, const std::string& text, long long received_ms);
  void keep_raw(const std::string& text, long long received_ms, const std::string& symbol,
                long long seq, const char* why);
  long long last_gap_seq_ = 0, seq_gaps_ = 0, last_seq_ = 0, seq_restarts_ = 0;
};

// The exchange's own quotes for the windows being priced, at the resolution the question needs.
//
// Session 17 priority 7 asks how long Kalshi takes to reprice after the reference moves, at 50 ms
// to 5 s. The desk's ordinary market data is a REST poll every five seconds, which cannot answer
// that question at all: it would produce a table with the right column headings and no
// information in it. So the same socket machinery carries the exchange's `ticker` channel for the
// handful of markets in play, which stamps every update with the venue's own millisecond clock,
// and this records that against our receipt time.
//
// The set of markets changes every fifteen minutes. `watch()` is called from the cycle with the
// tickers currently open and sends the difference, rather than reconnecting.
//
// Two channels, because they answer two different questions and one of them cannot answer the
// other. `ticker` says whether the QUOTED PRICE moved. `orderbook_delta` (with the snapshot that
// seeds it, and a sequence number on every message) is what says whether a given quantity could
// have been EXECUTED. A study that used ticker data to make a claim about fills would be
// describing a price nobody could have taken, so the book events are recorded separately, as the
// venue sent them, for a reconstruction done offline where it can be tested. No such
// reconstruction exists yet (2026-09-24): nothing reads commodity_book_events but the retention
// prune and the archive script, so nothing has yet refused a stretch for a gap.
//
// How to read commodity_book_events (checked against 500,465 recorded frames, 2026-09-23/24):
//   - Replay in `id` order, which is receipt order. `seq` is numbered once per subscription (sid)
//     across every market on it, not per market, and starts again at 1 on every connection while
//     the sid stays the same, so ordering by the (ticker, sequence) index puts a later connection's
//     stream before an earlier one's.
//   - 'orderbook_snapshot' replaces that market's book. Its levels are only in raw_json
//     (msg.yes_dollars_fp / msg.no_dollars_fp, pairs of [price_dollars, count_fp], either side may
//     be absent), and side, price, delta and ts_exchange_ms are NULL: a snapshot carries none of
//     them. One with no levels means the venue has no book for the market; in the sample every
//     window close sent one, but a re-subscription 177 s after the 03:00 windows closed (ids
//     50147884-50147888) got full books for all five, so use the market's close time, not an
//     empty snapshot, to know a window is over.
//   - 'orderbook_delta': price is msg.price_dollars in dollars, delta is msg.delta_fp in contracts
//     (negative removes), side is 'yes' or 'no', ts_exchange_ms is msg.ts_ms.
//   - 'ok', 'error', 'subscribed', 'unsubscribed' are the venue's replies to our commands, verbatim.
//     A reply to update_subscription takes a number in the sid's sequence, so without these rows
//     every window roll looks like a lost message.
//   - 'connect' and 'dropped' are written by this desk, not the venue: a new connection (no book
//     survives it without its next snapshot; raw_json lists what was subscribed), and rows this
//     desk's own queue had to drop (a different fact from the venue losing them).
// Rows written before 2026-09-24 differ: price and delta are 0 on every delta and snapshots hold
// 0/'' instead of NULL (the parser asked for keys the venue does not send), and replies and
// connections were not recorded. raw_json is intact on all of them; see the backfill notes.
class MarketQuoteFeed {
 public:
  struct Settings {
    bool enabled = true;
    std::string ws_url;
    std::string sign_path = "/trade-api/ws/v2";
    int ws_stale_after_seconds = 45;
    int ping_interval_seconds = 10;
    size_t max_pending_rows = 50000;
    std::string name = "kalshi-ticker";
    // Book events stop being recorded while the database's disk has less than this many GB free,
    // and start again 2 GB above it (params.commodity.quotes.min_free_gb; 0 turns the floor off).
    // The book stream is by far the largest writer on the desk; see Engine's quotes step.
    double min_free_gb = 10.0;
    // A delta whose every field is held by the columns is stored without its raw frame (about 265
    // of its 414 bytes). Set this to keep every frame whole
    // (params.commodity.quotes.keep_delta_raw).
    bool keep_delta_raw = false;
  };

  MarketQuoteFeed(Store& store, Settings settings, std::shared_ptr<Signer> signer);
  const Settings& settings() const { return s_; }
  ~MarketQuoteFeed();

  void start();
  void stop();
  bool running() const;
  // The markets to follow now. Sends only the difference from what is already subscribed.
  void watch(const std::vector<std::string>& tickers);
  int flush(long long now);
  // The disk is under the floor (true) or back above it (false). While it is, book frames are
  // followed (sequence and counts) but not kept; on the way back a 'paused_low_disk' row records
  // the stretch, so a replay reads it as missing rather than as a quiet book. The small streams
  // (replies to our commands, connects, ticker rows) are kept throughout.
  void set_disk_low(bool low, long long now_ms);
  nlohmann::json status() const;
  void on_message_for_tests(const std::string& text, long long received_ms) { on_text(text, received_ms); }
  // Exposed for tests: what the socket's connect handler does, without a socket.
  std::vector<std::string> on_connect_for_tests(long long now_ms) { return connect_frames(now_ms); }
  // Exposed for tests: the frames watch() would send on a connected socket.
  std::vector<std::string> watch_for_tests(const std::vector<std::string>& tickers) {
    std::lock_guard<std::mutex> lk(mu_);
    return watch_frames(tickers, true);
  }
  std::vector<std::string> watching() const;

 private:
  void on_text(const std::string& text, long long received_ms);
  // Called under mu_.
  std::vector<std::string> watch_frames(const std::vector<std::string>& tickers, bool has_socket);
  std::vector<std::string> connect_frames(long long now_ms);
  // Called under mu_.
  void queue_book(nlohmann::json row);
  void note_sequence(long long sid, long long seq, bool book_frame);
  void note_missing(const std::string& kind, const char* key);

  Store& store_;
  Settings s_;
  std::shared_ptr<Signer> signer_;
  std::unique_ptr<WsClient> ws_;
  mutable std::mutex mu_;
  std::set<std::string> watching_;
  std::vector<nlohmann::json> pending_, pending_book_;
  long long messages_ = 0, recorded_ = 0, dropped_rows_ = 0, unparsed_ = 0, command_id_ = 100;
  long long book_messages_ = 0, book_recorded_ = 0, snapshots_ = 0, empty_snapshots_ = 0;
  long long unwatched_ = 0, connects_ = 0, control_frames_ = 0, errors_ = 0;
  // The venue assigns a subscription id per channel, and `update_subscription` needs it. Taken
  // from the acknowledgements rather than assumed, because assuming it works until the day the
  // venue numbers them differently and the desk quietly stops following the live windows.
  std::map<std::string, long long> sids_;
  // Whether a subscribe has gone out on this connection. Until one has, there is no subscription
  // for update_subscription to change: the venue refuses it ("Unknown subscription ID").
  bool subscribed_ = false;
  // From this connection's connect handler until the socket reports it gone. The socket accepts
  // frames a moment before its connect handler runs, and a watch() in that moment would decide
  // against the old connection's state.
  bool conn_ready_ = false;
  long long forced_reconnects_ = 0, last_forced_reconnect_ms_ = 0;
  bool reconnect_due_ = false;          // a refusal came inside the minute; reconnect when it ends
  std::set<long long> add_ids_;         // this connection's add_markets commands
  // Called under mu_: drop the connection so the next one subscribes afresh.
  void force_reconnect(long long now_ms);
  // Sequence continuity per subscription id, because that is how the venue numbers it: one `seq`
  // per sid, shared by every market on it. Kept per market, as it was until 2026-09-24, every
  // switch from one market to the next looked like a gap, about 30% of all book messages on a
  // stream that had lost none. A gap here makes every book on the sid unreliable until its next
  // snapshot, since the lost message could have been for any of them.
  std::map<long long, long long> last_seq_by_sid_;
  long long sid_gaps_ = 0, seqs_missing_ = 0, seq_restarts_ = 0;
  // Book rows the queue could not hold, and the open stretch not yet written down as a marker.
  long long book_dropped_rows_ = 0, drop_rows_ = 0, drop_first_ms_ = 0, drop_first_seq_ = 0,
            drop_last_seq_ = 0;
  // Documented fields that did not arrive, by "kind.key". A column the parser could not fill is
  // NULL and counted here, never 0: a silent 0 is how every delta's price and size went unrecorded.
  std::map<std::string, long long> schema_misses_;
  long long raw_trimmed_ = 0;   // deltas stored without raw_json (Settings::keep_delta_raw)
  // The disk floor, and the stretch of book frames not kept under it.
  bool disk_low_ = false;
  long long pause_rows_ = 0, pause_first_ms_ = 0, pause_last_ms_ = 0, pause_first_seq_ = 0,
            pause_last_seq_ = 0, paused_total_ = 0;
};

MarketQuoteFeed::Settings market_quote_settings(const nlohmann::json& params, const std::string& env);

}  // namespace desk
