#pragma once
// SQLite persistence: everything the desk sees or does, so research and the
// $100 -> $1,000 gate can be computed from one file.
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "desk/types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace desk {

struct Proposal {
  long long id = 0, ts = 0, updated_ts = 0;
  std::string ticker, event_ticker, series_ticker, strategy, mode, outcome_side,
      exec_style,  // maker | taker
      status,      // pending | submitted | rejected | expired | failed | canceled
      order_id, note, cluster,
      entry_kind = "new";  // new | addition | hedge | reduce
  double price = 0, count = 0, p_model = 0, sigma = 0, fee_per = 0, net_edge = 0, yes_bid = 0,
         yes_ask = 0, required_edge = 0;
  bool approval_required = false;
  // The decision snapshot (trade_decisions) behind this proposal: what probability and what book
  // caused it. Every order the executor sends gets one (decision_log.h).
  std::string decision_id;
  json to_json() const;
};

struct FairValue {
  std::string ticker, note;
  double p = 0, sigma = 0;
  long long ts = 0;
};

struct Hypothesis {
  long long id = 0, updated_ts = 0;
  std::string key, category, title, statement, data_needed, test, status, notes;
};

using SqlValue = std::variant<std::nullptr_t, long long, double, std::string>;

// One quote-history row (the quotes table), exactly as Store::insert_quote writes it.
struct QuoteRow {
  long long ts = 0;
  std::string ticker;
  double yes_bid = 0, yes_ask = 0, no_bid = 0, no_ask = 0, last = 0, yes_bid_size = 0, yes_ask_size = 0,
         volume = 0, open_interest = 0;
  static QuoteRow of(long long ts, const Market& m);
};

// How refresh_markets' market rows reach the database (config markets_write_mode, 2026-10-01).
//   Write   every upsert writes the row, updated_ts = now. The behaviour before 2026-10-01.
//   Shadow  exactly Write, plus checks: the change detector's verdict is compared with the stored
//           row, and the in-memory poll time with the row's updated_ts. Nothing read changes.
//   Skip    a row is written only when a stored field changed, or as a 10-minute heartbeat; the
//           poll time lives in memory and every reader of Market::quote_ts sees it.
enum class MarketsWriteMode { Write, Shadow, Skip };
// "write" | "shadow" | "skip"; false for anything else.
bool parse_markets_write_mode(const std::string& s, MarketsWriteMode* out);
const char* markets_write_mode_name(MarketsWriteMode m);

class Store {
 public:
  explicit Store(const std::string& path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Generic read: rows as JSON objects keyed by column name.
  json query(const std::string& sql, const std::vector<SqlValue>& params = {});
  void exec(const std::string& sql, const std::vector<SqlValue>& params = {});
  // An INSERT and its new rowid, atomically: `SELECT last_insert_rowid()` as a separate query can
  // read another thread's insert, or (from a request thread) a read-only connection's 0.
  long long insert(const std::string& sql, const std::vector<SqlValue>& params = {});
  // Runs fn inside one transaction while holding the store lock for its whole duration, so a
  // statement from another thread cannot land inside it (or be rolled back with it). Rolls back
  // and rethrows on any exception. Must not be nested.
  // fn must take NO other lock: other threads take their own locks first and the store's second
  // (the dashboard holds Engine::snap_mu_ while it queries), so a lock taken inside fn inverts
  // that order and deadlocks. Store calls and plain computation only.
  void transaction(const std::function<void()>& fn);
  // Rows changed by the last statement on this connection. Only meaningful inside transaction().
  long long changes();

  // Readers (2026-09-23). The engine cycle is the writer; the dashboard's request threads and the
  // Polymarket thread mostly read. On one connection behind one lock, the dashboard's heavy reads
  // (the account report, execution stats, a per-market entry check on every board row) held the
  // cycle up for about 45% of its time, measured with `sample` on the live desk. So a file-backed
  // store also opens a small pool of read-only connections (SQLite WAL lets them read while the
  // writer writes), and a query goes to one of them when it is made on a thread other than the
  // writer thread and outside a transaction that thread opened itself (such a query must see the
  // transaction's own uncommitted rows). Every write, every transaction, and every query on the
  // writer thread use the one writer connection, exactly as before.
  // Until set_writer_thread() is called nothing is routed, which keeps tools and tests that never
  // run a cycle on the single connection. An in-memory store never has readers (a second
  // connection to ":memory:" would be a different, empty database).
  void set_writer_thread();
  size_t reader_count() const { return readers_.size(); }

  // reference data
  void upsert_series(const Series& s);
  std::optional<Series> get_series(const std::string& ticker);
  void upsert_market(const Market& m);
  std::optional<Market> get_market(const std::string& ticker);
  std::vector<Market> markets_in_event(const std::string& event_ticker);
  std::vector<Market> open_markets(const std::string& series_ticker = "");
  void insert_quote(long long ts, const Market& m);
  void insert_book(long long ts, const Orderbook& b);

  // ---- market rows: the write mode and the poll-time overlay (2026-10-01) -----------------------
  // macOS measured the desk dirtying 2.96 MB/s while awake (137 GB in 12.9 awake hours), 98% of it
  // rewrites, and the refresh transaction was 37% of the write-weighted stack samples on 09-28. Its
  // largest part: every listed market row (~1 KB, 3-5 to a page) rewritten every cycle only because
  // updated_ts = now, which also deletes and re-inserts its idx_markets_series entry. In Skip mode
  // the row is written when a stored field changed or its stored updated_ts is 10 minutes old, and
  // the poll time is kept here, so quote_ts still means "the last time the exchange gave us this
  // market" for every reader (market_from_row: get_market, open_markets, markets_in_event, and so
  // the stale gate, the board and the systematic screen). Store's own default is Write; the engine
  // applies Config::markets_write_mode (default shadow).
  void set_markets_write_mode(MarketsWriteMode m);
  MarketsWriteMode markets_write_mode() const { return markets_mode_.load(); }
  // Counters for the engine status: mode, shadow_rows, would_skip, detector_mismatches,
  // overlay_mismatches, written, skipped, heartbeats.
  json markets_write_stats() const;
  // refresh_results' retry stamp (UPDATE markets SET updated_ts). It still writes the row, and in
  // shadow and skip modes it also moves the poll time, so the overlay and the row agree.
  void touch_market(const std::string& ticker, long long ts);
  // The quote_ts a reader sees for a row whose stored updated_ts is `stored_ts`: the stored value in
  // write and shadow modes; in skip mode the later poll time when that row is the version the poll
  // saw (see PollTime), else the stored value.
  long long effective_quote_ts(const std::string& ticker, long long stored_ts) const;
  // Skip mode, at shutdown: writes the poll time into every row whose stored updated_ts is behind
  // it, so a restart reads the same updated_ts the old every-cycle write left. One transaction.
  // After a crash instead, a row not yet re-polled reads up to kMarketHeartbeatSeconds older than
  // its last poll: the stale gate can only refuse more, never pass an older book.
  int persist_market_poll_times();
  // Forgets markets not polled since `older_than` (they left the listings), so the maps stay the
  // size of what is open. In skip mode their rows get the poll time first, so nothing a reader sees
  // changes when the entry goes.
  void retire_market_poll_times(long long older_than);
  static constexpr long long kMarketHeartbeatSeconds = 600;
  // Tests: the clock upsert_market stamps rows and poll times with (0 = the real one).
  void set_clock_for_tests(long long now) { clock_override_.store(now); }

  // ---- quote history, buffered (2026-10-01) ------------------------------------------------------
  // Each quotes row landed on its own random idx_quotes_ticker(ticker, ts) leaf: a 4 KB page (then
  // a checkpoint copy) for a ~100-byte row, about 0.55 MB/s at the ~75 rows/s the 60 s heartbeat
  // alone produces. Rows are held here and written together every kQuoteFlushSeconds, so a ticker's
  // ~5 rows share one leaf write. The rows and their ts are exactly the ones insert_quote would have
  // written, inserted in the order it would have run, so rowids still increase with ts, which
  // scripts/archive_quotes.py's day boundary search relies on. recent_quotes() merges the buffer.
  void buffer_quotes(std::vector<QuoteRow> rows);
  // Writes everything buffered when kQuoteFlushSeconds have passed since the last flush, when the
  // buffer is past its soft cap, or when `force` (shutdown). One transaction. A failed write leaves
  // the rows buffered for the next attempt and rethrows. Returns the rows written.
  int flush_quotes(long long now, bool force = false);
  // The dashboard's history endpoint: newest first, `limit` rows, the database merged with what is
  // still buffered.
  json recent_quotes(const std::string& ticker, long long limit);
  size_t buffered_quotes() const;
  static constexpr long long kQuoteFlushSeconds = 300;
  static constexpr size_t kQuoteBufferSoftCap = 100000;   // flush at once past this
  static constexpr size_t kQuoteBufferHardCap = 300000;   // drop and count past this (writes failing)

  // fair values
  void set_fair(const FairValue& f);
  void clear_fair(const std::string& ticker);
  std::vector<FairValue> fairs();
  void insert_model_fair(long long ts, const std::string& ticker, const std::string& strategy, double p,
                         double sigma, double yes_bid, double yes_ask);
  // model_fairs rows collected during a cycle's strategy loop and written by flush_model_fairs() in
  // one transaction (2026-10-01): one autocommit per row cost the table's tail page plus a random
  // idx_model_fairs leaf, about 2.3 frames per ~120-byte row, ~8% of the 09-28 write samples. Same
  // rows, same ts, same order. The engine flushes after the strategy loop; the destructor flushes
  // whatever is left.
  void queue_model_fair(long long ts, const std::string& ticker, const std::string& strategy, double p,
                        double sigma, double yes_bid, double yes_ask);
  int flush_model_fairs();
  size_t queued_model_fairs() const;

  // proposals
  long long insert_proposal(Proposal& p);
  void update_proposal(const Proposal& p);
  // Which moves between proposal states are legal. Statuses are plain strings written from a
  // dozen places, and nothing stopped a terminal one from being revived: a filled or cancelled
  // proposal going back to pending would be picked up and sent a second time, and a submitted one
  // reset to pending would lose the order it already owns. Terminal is terminal.
  static bool proposal_transition_ok(const std::string& from, const std::string& to);
  static bool proposal_terminal(const std::string& s);
  std::optional<Proposal> get_proposal(long long id);
  std::vector<Proposal> proposals(const std::string& status, int limit = 200);
  std::optional<Proposal> find_open_proposal(const std::string& ticker, const std::string& side,
                                             const std::string& strategy);
  int expire_proposals(long long older_than_ts);

  // trading records
  void upsert_order(const Order& o, long long proposal_id = 0);
  std::vector<Order> orders(const std::string& status = "resting");
  long long proposal_for_order(const std::string& order_id);
  void upsert_fill(const Fill& f);
  void replace_positions(const std::vector<Position>& ps);
  std::vector<Position> positions();
  void upsert_settlement(const Settlement& s);
  void insert_account(long long ts, double balance, double portfolio_value);
  std::optional<Balance> last_account();
  void replace_risk_snapshot(const Balance& balance, const std::vector<Position>& positions,
                             const std::vector<Order>& orders, const json& shards, long long ts);

  // hypotheses
  void upsert_hypothesis(const Hypothesis& h);
  std::vector<Hypothesis> hypotheses();

  // key/value. A write whose value equals the stored one writes nothing (2026-10-01): INSERT OR
  // REPLACE deleted the row and re-inserted it under a new rowid, freeing and re-allocating a large
  // value's whole overflow chain every cycle even when nothing had changed.
  void kv_set(const std::string& k, const std::string& v);
  std::optional<std::string> kv_get(const std::string& k);

  // events (alerts strip on the dashboard)
  void log_event(const std::string& level, const std::string& msg);
  json events(int limit = 50);

  static Proposal proposal_from_row(const json& r);

 private:
  void migrate();
  // One prepared statement stepped once per row, inside the caller's transaction.
  void exec_rows(const std::string& sql, const std::vector<std::vector<SqlValue>>& rows);
  sqlite3* db_ = nullptr;
  std::recursive_mutex mu_;
  struct Reader { sqlite3* db = nullptr; std::mutex mu; };
  std::vector<std::unique_ptr<Reader>> readers_;
  std::atomic<size_t> next_reader_{0};
  std::atomic<std::thread::id> writer_{};
  bool use_reader() const;

  // ---- market rows (see set_markets_write_mode) ----
  // What this process last wrote (or, in shadow, would have written) for a market: the columns the
  // upsert's DO UPDATE sets, minus updated_ts, plus the routing index. Under mu_.
  struct MarketWritten {
    std::string status, result, close_time, expected_expiration_time, rules_primary, rules_secondary;
    double yes_bid = 0, yes_ask = 0, no_bid = 0, no_ask = 0, last = 0, volume = 0, open_interest = 0,
           yes_bid_size = 0, yes_ask_size = 0;
    int exchange_index = 0;
    long long written_ts = 0;   // the updated_ts that row carries (skip mode's heartbeat clock)
    static MarketWritten of(const Market& m);
    bool same_values(const MarketWritten& o) const;
  };
  Market market_from_row(const json& r, bool on_writer);
  void write_market_row(const Market& m, long long now);
  // True when the stored row equals `m` in every column the upsert sets but updated_ts (shadow).
  bool stored_market_matches(const Market& m, std::string* differs);
  // A poll time, and the updated_ts of the row version it vouches for. The poll says "the exchange
  // gave us exactly what the row with updated_ts == row holds"; a reader holding an older version of
  // the row (it read just before a write committed) gets that row's own updated_ts instead, so a poll
  // time is never put on a book it did not see.
  struct PollTime { long long poll = 0, row = 0; };
  long long quote_ts_for(const PollTime& p, long long stored_ts) const {
    return stored_ts >= p.row ? std::max(stored_ts, p.poll) : stored_ts;
  }
  void stage_market(const std::string& ticker, const MarketWritten* written, PollTime poll);
  void commit_staged_markets();   // after COMMIT (or an autocommit write)
  void discard_staged_markets();  // after ROLLBACK
  std::atomic<MarketsWriteMode> markets_mode_{MarketsWriteMode::Write};
  std::atomic<long long> clock_override_{0};
  std::unordered_map<std::string, MarketWritten> market_written_;          // under mu_
  std::unordered_map<std::string, MarketWritten> staged_written_;          // under mu_, until COMMIT
  std::unordered_map<std::string, PollTime> staged_poll_;                  // under mu_, until COMMIT
  // ticker -> the last time the exchange gave us this market (upsert_market) or refresh_results
  // stamped it (touch_market), in this process. Read by every thread, so under its own lock, which
  // is a leaf: nothing is ever locked while it is held.
  mutable std::mutex poll_mu_;
  std::unordered_map<std::string, PollTime> market_poll_ts_;
  std::atomic<long long> shadow_rows_{0}, would_skip_{0}, detector_mismatches_{0}, overlay_mismatches_{0},
      markets_written_{0}, markets_skipped_{0}, market_heartbeats_{0};
  sqlite3_stmt* shadow_stmt_ = nullptr;   // the shadow check's row read, prepared once (under mu_)

  // ---- quote history buffer (see buffer_quotes). A leaf lock: never held while taking mu_. ----
  mutable std::mutex quotes_mu_;
  std::vector<QuoteRow> quote_buffer_;   // arrival order, which is ts order
  long long quotes_flushed_ts_ = 0;      // the last flush (0: none yet; the first buffer starts the clock)
  long long quotes_dropped_ = 0;
  bool quotes_flushing_ = false;         // one flush at a time: two would write the same rows

  // ---- model_fairs queue (see queue_model_fair). A leaf lock as well. ----
  mutable std::mutex fairs_mu_;
  std::vector<std::vector<SqlValue>> fair_queue_;
};

}  // namespace desk
