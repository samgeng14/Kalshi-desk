#pragma once
// Polymarket US: the CFTC-regulated exchange (api.polymarket.us for trading, gateway.polymarket.us
// for public data), Ed25519-signed requests, the same binary contracts as Kalshi with a different
// fee schedule (taker 0.06 p(1-p) a contract, makers are PAID 0.0125 p(1-p)) and a 0.005 tick on
// game markets.
//
// The desk uses it three ways:
//   1. as a second venue it can read and trade by hand from a ticket behind its own rails;
//   2. as a price source: a Kalshi game market that also trades on Polymarket gets a fair from
//      the Polymarket book (the `xvenue` strategy), which the board and the systematic trader
//      then treat like any other estimate, except that this one is a market, not a model;
//   3. as the other leg of a pair: the same outcome bought on the cheap venue and sold on the
//      rich one locks the difference to settlement (`xvenue_trades`, the pair rule).
//
// Matching is by league, date and team, from the Kalshi event ticker and title against
// Polymarket's team metadata; a pair exists only when both teams of the game map to the two
// sides of one Polymarket event.
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "desk/config.h"
#include "desk/executor.h"
#include "desk/http.h"
#include "desk/store.h"
#include "desk/types.h"

namespace desk {

// ---- wire -----------------------------------------------------------------------------------------

class PolyAuth {
 public:
  // secret_b64: the base64 secret shown once at key creation (32-byte seed, or 64 bytes of
  // which the first 32 are the seed). Never logged, never stored by the desk.
  PolyAuth(std::string key_id, const std::string& secret_b64);
  ~PolyAuth();
  PolyAuth(const PolyAuth&) = delete;
  PolyAuth& operator=(const PolyAuth&) = delete;
  // Ed25519 over timestamp_ms + METHOD + path (no query), base64.
  std::string sign(const std::string& text) const;
  std::map<std::string, std::string> headers(const std::string& method, const std::string& path, long long timestamp_ms) const;
  std::string public_key_b64() const;
  static bool verify(const std::string& public_key_b64, const std::string& text, const std::string& sig_b64);
  const std::string& key_id() const { return key_id_; }

 private:
  std::string key_id_;
  void* pkey_ = nullptr;   // EVP_PKEY*
};

class PolyClient {
 public:
  PolyClient(std::string api_base, std::string gateway_base, std::shared_ptr<PolyAuth> auth, std::string user_agent);
  bool authenticated() const { return auth_ != nullptr; }
  // Public (gateway) or authenticated (api) call. `path` starts with /v1 or /v2.
  json call(const std::string& method, const std::string& path, const std::vector<std::pair<std::string, std::string>>& query = {},
            const json* body = nullptr, bool authenticated = false);

 private:
  std::string api_base_, gateway_base_;
  std::shared_ptr<PolyAuth> auth_;
  Http http_;
  std::mutex mu_;
  long long last_call_ms_ = 0;
};

// ---- data -----------------------------------------------------------------------------------------

struct PolySide {
  std::string description, abbr, name, safe_name, alias;
  bool is_long = false;
  int team_id = 0;
};
struct PolyMarket {
  std::string slug, event_slug, event_title, question, league, sports_type;
  double tick = 0.01, min_qty = 1, fee_coeff = 0.06, bid = 0, ask = 0;
  long long start_ts = 0, end_ts = 0, quote_ts = 0, updated_ts = 0;
  bool active = true, closed = false;
  std::vector<PolySide> sides;   // the long side first when known
  double mid() const { return bid > 0 && ask > 0 ? .5 * (bid + ask) : (bid > 0 ? bid : ask); }
  bool two_sided() const { return bid > 0 && ask > 0 && ask > bid && ask < 1; }
};
// One Kalshi market (a team's "wins" contract) against one Polymarket market side.
struct PolyPair {
  std::string kalshi_ticker, kalshi_event, kalshi_series, kalshi_team, slug, league, pm_team, matched_by;
  bool pm_yes_is_kalshi_yes = true;   // false: Kalshi YES on this team is Polymarket's NO side
  int score = 0;
};
struct PolyPosition {
  std::string slug;
  double net = 0, cost = 0, realized = 0, unrealized = 0, available = 0;
  bool expired = false;
  long long update_ts = 0;
};
struct PolyBalance {
  double current_balance = 0, buying_power = 0, asset_notional = 0, asset_available = 0;
  long long updated_ts = 0;
};
struct PolyOrder {
  std::string id, slug, side, type, tif, intent, state, note, origin;
  double price = 0, quantity = 0, cum = 0, leaves = 0, avg_px = 0, commission = 0;
  long long created_ts = 0, updated_ts = 0;
};
struct PolyTrade {
  std::string id, slug, state;
  double price = 0, qty = 0, cost_basis = 0, realized = 0;
  bool aggressor = false;
  long long ts = 0;
};
struct PolyOrderRequest {
  std::string slug;
  std::string action = "BUY";        // BUY or SELL of the YES instrument; SELL when flat opens a short
  double price = 0, quantity = 0;   // YES price, contracts
  bool post_only = false;
  std::string tif = "GTC";           // GTC | IOC | FOK
  bool manual = true;
};

PolyMarket parse_poly_market(const json& j, const std::string& league, const std::string& event_slug, const std::string& event_title);
PolyPosition parse_poly_position(const std::string& slug, const json& j);
PolyBalance parse_poly_balance(const json& j);
PolyOrder parse_poly_order(const json& j);
PolyTrade parse_poly_trade(const json& j);
json poly_order_body(const PolyOrderRequest& r);
// Polymarket US fee per contract at price p: taker pays coeff*p(1-p); a maker receives 0.0125*p(1-p).
double poly_fee(double p, bool taker, double coeff = 0.06);
// Which Polymarket league a Kalshi series maps to ("" = not a game series the desk pairs).
std::string poly_league_for_series(const std::string& series_ticker);
// The team matcher, exposed for tests: how well a Kalshi team (code from the ticker, name from
// the title) matches a Polymarket side. 0 = no match.
int poly_team_score(const std::string& kalshi_code, const std::string& kalshi_name, const PolySide& side);
// Kalshi event ticker -> (yyyy-mm-dd, the concatenated team codes in `away`, "" in `home`, and
// the HHMM Eastern start when the ticker carries one) or false on failure.
bool parse_kalshi_game_event(const std::string& event_ticker, std::string* date, std::string* away, std::string* home, std::string* hhmm = nullptr);

struct PolyConfig {
  bool enabled = false;               // on when key_id + secret_path are set, or explicitly
  std::string key_id, secret_path;    // the secret lives in a file with 0600 permissions, never in the config
  std::string api_base = "https://api.polymarket.us", gateway_base = "https://gateway.polymarket.us";
  std::vector<std::string> leagues = {"nfl", "cfb", "mlb", "nba", "nhl", "wnba", "epl", "lal", "sea", "bun", "lg1", "mls", "ucl"};
  double max_order_dollars = 25.0;    // cost (or margin) per order
  double max_total_dollars = 100.0;   // cost of positions plus resting orders
  double price_band = 0.10;           // a limit further than this from the mid is a fat finger
  int refresh_seconds = 10;           // quotes for matched markets
  int universe_refresh_seconds = 600; // league events
  int account_refresh_seconds = 30;
};
PolyConfig poly_config(const nlohmann::json& params);
// The client for a config: the secret is read from `secret_path` (a file holding the base64
// secret, ideally mode 0600; "~" expands). A missing or unreadable file gives a public-only
// client and a warning, never a crash. Null when Polymarket is not enabled.
std::shared_ptr<PolyClient> make_poly_client(const PolyConfig& pc, const std::string& user_agent);

// ---- the desk ---------------------------------------------------------------------------------------

class PolyDesk {
 public:
  struct Hooks {
    std::function<bool()> killed;
    std::function<void()> request_reconcile;
    Hooks() : killed([] { return false; }), request_reconcile([] {}) {}
    Hooks(std::function<bool()> k, std::function<void()> rr) : killed(std::move(k)), request_reconcile(std::move(rr)) {}
  };
  // `exec` may be null: the pair rule then cannot go live (paper only) and pairs cannot be sent.
  PolyDesk(const Config& cfg, Store& store, std::shared_ptr<PolyClient> client, Executor* exec, Hooks hooks = {});

  // Cycle hooks, in this order, under the engine lock. Exceptions are logged, never thrown.
  void refresh_universe(long long now);   // league events -> game-winner markets
  void refresh_quotes(long long now);     // bid/ask for matched and held markets
  void refresh_account(long long now, bool force = false);
  void match(long long now);              // Kalshi markets in the store <-> Polymarket markets
  void step(long long now);               // the pair rule

  json view();                            // the tab
  json pairs(long long now);              // every pair with both books and the lock arithmetic
  json book(const std::string& slug);
  json preview(const PolyOrderRequest& req);
  json ticket(const PolyOrderRequest& req, const std::string& note);
  json cancel(const std::string& order_id, const std::string& slug);
  int cancel_all(const std::string& why);
  json close_position(const std::string& slug);
  json configure_algo(const json& body);  // {running, paper, clear_halt, settings{}}
  // Send one pair by hand: the Kalshi leg through the executor, the Polymarket leg after it fills
  // (the rule's own machinery, one row in xvenue_trades).
  json send_pair(const std::string& kalshi_ticker, const std::string& route, double contracts, const std::string& kalshi_style,
                 const std::string& pm_style, long long now);
  json summary();
  json pnl();
  bool enabled() const { return pc_.enabled; }
  bool armed() const { return client_ && client_->authenticated(); }
  const PolyConfig& config() const { return pc_; }
  // Fresh Polymarket quote for a Kalshi ticker, in Kalshi-YES terms. Used by the xvenue strategy.
  struct Quote { double bid = 0, ask = 0; long long ts = 0; std::string slug; bool flipped = false; };
  std::optional<Quote> quote_for(const std::string& kalshi_ticker, long long now);
  std::map<std::string, PolyPair> pairs_by_ticker();

 private:
  void rails(const PolyOrderRequest& r, const PolyMarket& m, double* cost_out);
  std::optional<PolyMarket> market(const std::string& slug);
  void algo_persist();
  void algo_log(const std::string& level, const std::string& msg);
  double algo_pnl_today(long long now);
  json lock_math(const PolyPair& pr, const Market& km, const Series& ks, const PolyMarket& pm, long long now);
  void manage_pairs(long long now);
  void enter_pairs(long long now);
  bool send_kalshi_leg(json& trade, long long now, std::string* why);
  bool send_pm_leg(json& trade, long long now, std::string* why);
  // True only after the prior hedge's terminal status and cumulative fills are known.
  bool reconcile_pm_leg(json& trade, long long now, bool cancel_working, std::string* why);
  void upsert_xtrade(json& t);
  json xtrades(const std::string& states_sql);

  const Config& cfg_;
  PolyConfig pc_;
  Store& store_;
  std::shared_ptr<PolyClient> client_;
  Executor* exec_;
  Hooks hooks_;
  std::map<std::string, PolyMarket> markets_;   // slug -> market (the universe of game winners + anything held)
  std::map<std::string, PolyPair> pairs_;       // kalshi ticker -> pair
  std::vector<PolyPosition> positions_;
  PolyBalance balance_;
  json algo_state_;
  std::map<std::string, long long> pair_backoff_;   // kalshi ticker -> no new pair before this
  long long universe_ts_ = 0, quotes_ts_ = 0, account_ts_ = 0, match_ts_ = 0;
  std::string last_error_;
  std::recursive_mutex mu_;
  using Lock = std::lock_guard<std::recursive_mutex>;
};

}  // namespace desk
