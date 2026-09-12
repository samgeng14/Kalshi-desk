#pragma once
// The one thing a probability model produces: P(YES) for one contract, with its uncertainty and
// where it came from. Nothing else.
//
// A ProbabilityQuote is a MODEL OUTPUT. It carries no order size, no execution style, no route, no
// limit price, no take-profit and no stop. Whether a quote is worth trading, and by whom, is decided
// downstream from it: the edge engine (edge.h) prices it against the book, and the decision router
// (decision_router.h) routes it. A model can say what it knows about its own estimate (its inputs
// failed a check, it is research-only, its source is not approved for unattended use, it has
// verified what the contract settles on); it can never say "trade me automatically".
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "desk/strategy.h"
#include "desk/types.h"

namespace desk {

using json = nlohmann::json;

struct ProbabilityQuote {
  std::string ticker;
  std::string event_ticker;
  std::string category;

  std::string model_name;      // the strategy that produced it, e.g. "weather_nws"
  std::string model_version;   // e.g. "v1"; routes and graduation are per name + version

  double p_yes = NAN;          // P(YES) the desk trades on (after the market blend, if any)
  double sigma = NAN;          // uncertainty on p_yes, probability units

  int64_t model_ts = 0;        // when the model produced this estimate
  int64_t underlying_ts = 0;   // freshest timestamp of the data behind it (0 = the model did not say)
  int64_t received_ts = 0;     // when the desk received it

  bool inputs_valid = false;         // the model's own data checks passed
  bool settlement_verified = false;  // the contract's settlement target is the one the model estimates
  bool auto_eligible = false;        // the model's source may be considered for unattended execution

  // The model's own verdict on its estimate, retained as it gave it. A research-only estimate is
  // logged and shown, never recommended.
  bool research_only = false;
  std::string research_reason;

  // Everything else the model said: p_raw (before the market blend), the blend weight, its note,
  // its raw inputs, source timestamps, invalid-estimate reasons. Informational only.
  json diagnostics = json::object();

  std::string model_key() const { return model_name.empty() ? std::string() : model_name + "_" + model_version; }
  // p before the market blend; p_yes when the model did not report one.
  double p_raw() const;
};

// Invariant violations, empty when the quote is well formed: 0 <= p_yes <= 1, sigma >= 0 (both
// finite), a ticker, a model name, a model timestamp. A quote that fails any of these must also
// carry inputs_valid = false; quote_from_fair guarantees that.
std::vector<std::string> quote_problems(const ProbabilityQuote& q);
inline bool quote_valid(const ProbabilityQuote& q) { return quote_problems(q).empty(); }

json quote_to_json(const ProbabilityQuote& q);
ProbabilityQuote quote_from_json(const json& j);

// ---- adapters: the existing models, unchanged, wrapped into quotes -------------------------------

// Every model the desk runs is version v1 until its math changes; a change gets a new version so a
// route or a graduation granted to the old numbers never carries over to new ones silently.
std::string model_version_for(const std::string& model_name);

// Has this model's settlement target been checked against this contract? Per model, from the
// evidence already in the code (docs/handoff.md, session 15, lists each rule).
bool settlement_verified_for(const std::string& model_name, const Market& m);

// Wrap a model's Fair. `f` must be the Fair as the desk trades it (p after the market blend,
// p_raw before it). `model_research_only` / `model_research_reason` are the model's OWN flags,
// taken before the engine layered its gates on top. The probability math is not touched: p_yes is
// f.p exactly.
ProbabilityQuote quote_from_fair(const Market& m, const std::string& category, const std::string& model_name,
                                 const Fair& f, bool model_research_only, const std::string& model_research_reason,
                                 long long model_ts, long long received_ts);

// A board row that carries no "quote" object (a hand-built board, or a row from before this
// change): rebuild the quote from the row's fair fields. Fails closed: settlement is unverified
// unless the row says otherwise.
ProbabilityQuote quote_from_board_row(const json& row);

}  // namespace desk
