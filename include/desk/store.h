#pragma once
// SQLite persistence: everything the desk sees or does, so research and the
// $100 -> $1,000 gate can be computed from one file.
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "desk/types.h"

struct sqlite3;

namespace desk {

struct Proposal {
  long long id = 0, ts = 0, updated_ts = 0;
  std::string ticker, event_ticker, series_ticker, strategy, mode, outcome_side,
      exec_style,  // maker | taker
      status,      // pending | submitted | rejected | expired | failed | canceled
      order_id, note, cluster;
  double price = 0, count = 0, p_model = 0, sigma = 0, fee_per = 0, net_edge = 0, yes_bid = 0,
         yes_ask = 0;
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

class Store {
 public:
  explicit Store(const std::string& path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Generic read: rows as JSON objects keyed by column name.
  json query(const std::string& sql, const std::vector<SqlValue>& params = {});
  void exec(const std::string& sql, const std::vector<SqlValue>& params = {});

  // reference data
  void upsert_series(const Series& s);
  std::optional<Series> get_series(const std::string& ticker);
  void upsert_market(const Market& m);
  std::optional<Market> get_market(const std::string& ticker);
  std::vector<Market> markets_in_event(const std::string& event_ticker);
  std::vector<Market> open_markets(const std::string& series_ticker = "");
  void insert_quote(long long ts, const Market& m);
  void insert_book(long long ts, const Orderbook& b);

  // fair values
  void set_fair(const FairValue& f);
  void clear_fair(const std::string& ticker);
  std::vector<FairValue> fairs();
  void insert_model_fair(long long ts, const std::string& ticker, const std::string& strategy, double p,
                         double sigma, double yes_bid, double yes_ask);

  // proposals
  long long insert_proposal(Proposal& p);
  void update_proposal(const Proposal& p);
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

  // key/value
  void kv_set(const std::string& k, const std::string& v);
  std::optional<std::string> kv_get(const std::string& k);

  // events (alerts strip on the dashboard)
  void log_event(const std::string& level, const std::string& msg);
  json events(int limit = 50);

  static Proposal proposal_from_row(const json& r);

 private:
  void migrate();
  sqlite3* db_ = nullptr;
  std::recursive_mutex mu_;
};

}  // namespace desk
