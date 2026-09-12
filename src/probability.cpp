#include "desk/probability.h"

#include <algorithm>
#include <cctype>

#include "desk/risk.h"
#include "desk/systematic.h"
#include "desk/universe.h"

namespace desk {
namespace {

bool finite_num(const json& j, const char* key) {
  return j.is_object() && j.contains(key) && j[key].is_number() && std::isfinite(j[key].get<double>());
}
double num_or(const json& j, const char* key, double fallback) {
  return finite_num(j, key) ? j[key].get<double>() : fallback;
}
long long ll_or(const json& j, const char* key, long long fallback) {
  return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<long long>() : fallback;
}
std::string str_or(const json& j, const char* key, const std::string& fallback = "") {
  return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// The model's input health, read the way the desk always has: absent means nothing was checked,
// present-and-false means do not trade on it.
bool inputs_ok(const json& inputs) {
  return !(inputs.is_object() && inputs.contains("valid") && inputs["valid"].is_boolean() && !inputs["valid"].get<bool>());
}
bool auto_ok(const json& inputs, bool fallback) {
  if (inputs.is_object() && inputs.contains("auto_eligible") && inputs["auto_eligible"].is_boolean())
    return inputs["auto_eligible"].get<bool>();
  return fallback;
}

// Source timestamps the models already report in their inputs. The freshest is the quote's
// underlying_ts; every one found is kept in diagnostics.
void source_times(const json& inputs, long long model_ts, ProbabilityQuote& q) {
  static const char* kKeys[] = {"underlying_ts", "source_ts", "spot_ts", "observation_ts", "forecast_issued_ts",
                                "data_ts", "volatility_ts"};
  json found = json::object();
  long long best = 0;
  if (inputs.is_object()) {
    for (const char* k : kKeys) {
      long long v = ll_or(inputs, k, 0);
      if (v > 0) { found[k] = v; best = std::max(best, v); }
    }
    // xvenue reports an age, not a timestamp.
    long long age = ll_or(inputs, "polymarket_quote_age_s", -1);
    if (age >= 0 && model_ts > 0) { found["polymarket_quote_ts"] = model_ts - age; best = std::max(best, model_ts - age); }
  }
  q.underlying_ts = best;
  if (!found.empty()) q.diagnostics["source_timestamps"] = found;
}

void mark_invalid(ProbabilityQuote& q) {
  // An estimate that breaks the invariants is explicitly invalid, whatever its inputs said.
  auto problems = quote_problems(q);
  if (!problems.empty()) {
    q.inputs_valid = false;
    q.auto_eligible = false;
    q.diagnostics["invalid_reasons"] = problems;
  }
}

}  // namespace

double ProbabilityQuote::p_raw() const {
  return finite_num(diagnostics, "p_raw") ? diagnostics["p_raw"].get<double>() : p_yes;
}

std::vector<std::string> quote_problems(const ProbabilityQuote& q) {
  std::vector<std::string> out;
  if (q.ticker.empty()) out.push_back("quote has no ticker");
  if (q.model_name.empty()) out.push_back("quote has no model name");
  if (!std::isfinite(q.p_yes) || q.p_yes < 0 || q.p_yes > 1) out.push_back("p_yes is missing or outside [0, 1]");
  if (!std::isfinite(q.sigma) || q.sigma < 0) out.push_back("sigma is missing or negative");
  if (q.model_ts <= 0) out.push_back("quote has no model timestamp");
  return out;
}

json quote_to_json(const ProbabilityQuote& q) {
  auto num = [](double x) { return std::isfinite(x) ? json(x) : json(nullptr); };
  return json{{"ticker", q.ticker},
              {"event_ticker", q.event_ticker},
              {"category", q.category},
              {"model_name", q.model_name},
              {"model_version", q.model_version},
              {"model_key", q.model_key()},
              {"p_yes", num(q.p_yes)},
              {"sigma", num(q.sigma)},
              {"model_ts", q.model_ts},
              {"underlying_ts", q.underlying_ts},
              {"received_ts", q.received_ts},
              {"inputs_valid", q.inputs_valid},
              {"settlement_verified", q.settlement_verified},
              {"auto_eligible", q.auto_eligible},
              {"research_only", q.research_only},
              {"research_reason", q.research_reason},
              {"diagnostics", q.diagnostics}};
}

ProbabilityQuote quote_from_json(const json& j) {
  ProbabilityQuote q;
  if (!j.is_object()) return q;
  q.ticker = str_or(j, "ticker");
  q.event_ticker = str_or(j, "event_ticker");
  q.category = str_or(j, "category");
  q.model_name = str_or(j, "model_name");
  q.model_version = str_or(j, "model_version", "v1");
  q.p_yes = num_or(j, "p_yes", NAN);
  q.sigma = num_or(j, "sigma", NAN);
  q.model_ts = ll_or(j, "model_ts", 0);
  q.underlying_ts = ll_or(j, "underlying_ts", 0);
  q.received_ts = ll_or(j, "received_ts", 0);
  q.inputs_valid = j.value("inputs_valid", false);
  q.settlement_verified = j.value("settlement_verified", false);
  q.auto_eligible = j.value("auto_eligible", false);
  q.research_only = j.value("research_only", false);
  q.research_reason = str_or(j, "research_reason");
  if (j.contains("diagnostics") && j["diagnostics"].is_object()) q.diagnostics = j["diagnostics"];
  return q;
}

std::string model_version_for(const std::string& /*model_name*/) {
  // No model's math has changed since routes and graduation became per model: all are v1.
  return "v1";
}

bool settlement_verified_for(const std::string& model, const Market& m) {
  if (model == "weather_nws") {
    // The contract names the NWS (not The Weather Company) and the series has a read, mapped
    // station product (universe.cpp, docs/settlement.md). The engine has refused the other case
    // as "settlement source not validated" since session 4.
    const SettlementSpec* spec = settlement_for(m.series_ticker);
    return weather_source_supported(m) && spec && spec->product && *spec->product;
  }
  if (model == "sports_inplay") {
    // Soccer result markets settle after 90 minutes plus stoppage, which is what the in-play
    // model prices; only series whose rules were read are mapped.
    const SettlementSpec* spec = settlement_for(m.series_ticker);
    return spec && std::string(spec->kind) == "soccer_90";
  }
  if (model == "sports_totals") return true;   // it refuses to price a contract without rules_verified
  if (model == "sports_live") return true;     // game-winner markets settle on the official result it estimates
  if (model == "crypto_lognormal") {
    // Settles on the CF Benchmarks real-time index; the model prices it off Coinbase spot with an
    // explicit basis uncertainty. Verified when the contract's rules name that index.
    const std::string rules = lower(m.rules_primary);
    return rules.find("cf benchmarks") != std::string::npos || rules.find("real-time index") != std::string::npos;
  }
  if (model == "manual_fair") return true;     // your own number: you answer for what it settles on
  // xvenue: payoff parity between Kalshi and Polymarket is read for some leagues, not verified.
  return false;
}

ProbabilityQuote quote_from_fair(const Market& m, const std::string& category, const std::string& model_name,
                                 const Fair& f, bool model_research_only, const std::string& model_research_reason,
                                 long long model_ts, long long received_ts) {
  ProbabilityQuote q;
  q.ticker = m.ticker;
  q.event_ticker = m.event_ticker;
  q.category = category;
  q.model_name = model_name;
  q.model_version = model_version_for(model_name);
  q.p_yes = f.p;
  q.sigma = f.sigma;
  q.model_ts = model_ts;
  q.received_ts = received_ts;
  q.inputs_valid = inputs_ok(f.inputs);
  q.auto_eligible = auto_ok(f.inputs, q.inputs_valid);
  q.settlement_verified = settlement_verified_for(model_name, m);
  q.research_only = model_research_only;
  q.research_reason = model_research_reason;
  q.diagnostics = json{{"p_raw", f.p_raw >= 0 ? f.p_raw : f.p},
                       {"market_blend_weight", f.market_blend_weight},
                       {"note", f.note},
                       {"cluster", f.cluster},
                       {"inputs", f.inputs.is_object() ? f.inputs : json::object()}};
  source_times(f.inputs, model_ts, q);
  mark_invalid(q);
  return q;
}

ProbabilityQuote quote_from_board_row(const json& row) {
  ProbabilityQuote q;
  q.ticker = str_or(row, "ticker");
  q.event_ticker = str_or(row, "event_ticker");
  q.category = systematic_category(row);
  q.model_name = str_or(row, "fair_strategy");
  q.model_version = str_or(row, "model_version", "v1");
  q.p_yes = num_or(row, "p_model", NAN);
  q.sigma = num_or(row, "sigma", NAN);
  q.model_ts = ll_or(row, "fair_ts", 0);
  q.received_ts = q.model_ts;
  const json inputs = row.contains("model_inputs") && row["model_inputs"].is_object() ? row["model_inputs"] : json::object();
  q.inputs_valid = inputs_ok(inputs);
  q.auto_eligible = auto_ok(inputs, true);
  q.settlement_verified = row.value("settlement_verified", false);
  q.research_only = row.value("model_research_only", false);
  q.research_reason = str_or(row, "model_research_reason", str_or(row, "research_reason"));
  q.diagnostics = json{{"p_raw", num_or(row, "p_raw", q.p_yes)}, {"note", str_or(row, "fair_note")}, {"inputs", inputs},
                       {"rebuilt_from_board_row", true}};
  if (!std::isfinite(q.p_yes)) q.diagnostics["p_raw"] = nullptr;
  source_times(inputs, q.model_ts, q);
  mark_invalid(q);
  return q;
}

}  // namespace desk
