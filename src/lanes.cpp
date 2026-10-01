#include "desk/lanes.h"

#include <algorithm>
#include <cmath>

namespace desk {

namespace {
const json* lane_of(const json& s, const std::string& model) {
  if (model.empty() || !s.is_object() || !s.contains("lanes") || !s["lanes"].is_object()) return nullptr;
  auto it = s["lanes"].find(model);
  return it == s["lanes"].end() || !it->is_object() ? nullptr : &*it;
}
double num(const json& j, const char* k, double d) {
  auto it = j.find(k);
  return it != j.end() && it->is_number() ? it->get<double>() : d;
}
std::string str(const json& j, const char* k, const std::string& d = "") {
  auto it = j.find(k);
  return it != j.end() && it->is_string() ? it->get<std::string>() : d;
}
std::string money(double x) {
  char b[32];
  std::snprintf(b, sizeof b, "$%.2f", x);
  return b;
}
}  // namespace

bool valid_lane_stage(const std::string& s) {
  return s == "research" || s == "paper" || s == "live_candidate" || s == "live";
}

bool lane_managed(const json& s, const std::string& model) { return lane_of(s, model) != nullptr; }

std::string lane_stage(const json& s, const std::string& model) {
  const json* l = lane_of(s, model);
  return l ? str(*l, "stage", "research") : "";
}

bool lane_trades_live(const json& s, const std::string& model, const std::string& today) {
  const json* l = lane_of(s, model);
  if (!l) return false;
  const std::string st = str(*l, "stage", "research");
  if (st != "live_candidate" && st != "live") return false;
  return today.empty() || str(*l, "halted_day") != today;
}

double lane_stake(const json& s, const std::string& model) {
  const json* l = lane_of(s, model);
  const double base = num(s, "lane_base_stake", 5.0);
  return l ? num(*l, "stake", base) : base;
}

bool lane_asymmetric(const json& s, const std::string& model) {
  const json* l = lane_of(s, model);
  if (!l) return false;
  auto it = l->find("asymmetric");
  const bool lane_flag = it == l->end() || !it->is_boolean() || it->get<bool>();
  auto f = s.find("asymmetric_filter");
  return lane_flag && (f == s.end() || !f->is_boolean() || f->get<bool>());
}

json lane_object(const json& partial, double base_stake) {
  json l = {{"stage", "research"}, {"stake", base_stake}, {"evaluated_trades", 0}, {"negative_windows", 0},
            {"since_ts", 0}, {"halted_day", ""}, {"windows", json::array()}, {"asymmetric", true}};
  if (partial.is_object())
    for (auto it = partial.begin(); it != partial.end(); ++it) l[it.key()] = it.value();
  return l;
}

LaneWindow evaluate_lane_window(const std::vector<double>& pnl, const std::vector<double>& fees, double z) {
  LaneWindow w;
  w.n = static_cast<int>(pnl.size());
  if (w.n == 0) { w.verdict = "hold"; return w; }
  double sum = 0, fee_sum = 0;
  for (double x : pnl) sum += x;
  for (double f : fees) fee_sum += f;
  w.mean = sum / w.n;
  w.fee = fees.empty() ? 0 : fee_sum / static_cast<double>(fees.size());
  double ss = 0;
  for (double x : pnl) ss += (x - w.mean) * (x - w.mean);
  w.sd = w.n > 1 ? std::sqrt(ss / (w.n - 1)) : 0;
  w.lower = w.mean - z * w.sd / std::sqrt(static_cast<double>(w.n));
  if (w.mean > 0 && w.lower > -w.fee) w.verdict = "double";
  else if (w.mean < 0) w.verdict = "halve";
  else w.verdict = "hold";
  return w;
}

json apply_lane_windows(json lane, const std::string& model, const std::vector<double>& pnl,
                        const std::vector<double>& fees, const json& s, std::vector<std::string>* notes) {
  const double base = num(s, "lane_base_stake", 5.0);
  lane = lane_object(lane, base);
  const int window = std::max(1, static_cast<int>(num(s, "lane_window_trades", 30)));
  const double cap = num(s, "lane_max_stake", 40.0);
  const double z = num(s, "lane_band_z", 1.2816);
  const int to_research = std::max(1, static_cast<int>(num(s, "lane_negative_windows_to_research", 3)));
  auto note = [&](const std::string& m) { if (notes) notes->push_back(model + ": " + m); };
  int done = static_cast<int>(num(lane, "evaluated_trades", 0));
  if (done < 0 || done > static_cast<int>(pnl.size())) done = 0;   // history rewritten: start over
  while (static_cast<int>(pnl.size()) - done >= window) {
    const std::string stage = str(lane, "stage", "research");
    std::vector<double> p(pnl.begin() + done, pnl.begin() + done + window);
    std::vector<double> f;
    if (fees.size() >= static_cast<size_t>(done + window)) f.assign(fees.begin() + done, fees.begin() + done + window);
    const LaneWindow w = evaluate_lane_window(p, f, z);
    done += window;
    double stake = num(lane, "stake", base);
    const double before = stake;
    int neg = static_cast<int>(num(lane, "negative_windows", 0));
    // Only a lane the algorithm trades moves its stake: a window of trades made before a demotion
    // is still counted (so evaluated_trades stays in step), but it does not re-promote anything.
    const bool trading = stage == "live_candidate" || stage == "live";
    if (w.verdict == "double") {
      neg = 0;
      if (trading) stake = std::min(cap, stake * 2);
    } else if (w.verdict == "halve") {
      ++neg;
      // Halve, down to the settings' minimum stake; never raise a stake that is already small.
      if (trading) stake = std::min(stake, std::max(0.5, stake / 2));
    } else {
      neg = 0;
    }
    lane["negative_windows"] = neg;
    lane["stake"] = std::round(stake * 100) / 100;
    if (trading) lane["stage"] = stake > base + 1e-9 ? "live" : "live_candidate";
    json rec = {{"trades", w.n}, {"mean", std::round(w.mean * 1e4) / 1e4}, {"sd", std::round(w.sd * 1e4) / 1e4},
                {"lower", std::round(w.lower * 1e4) / 1e4}, {"fee", std::round(w.fee * 1e4) / 1e4},
                {"verdict", w.verdict}, {"stake_before", before}, {"stake_after", lane["stake"]}};
    note("window of " + std::to_string(w.n) + " settled trades: mean " + money(w.mean) + "/trade, lower band " +
         money(w.lower) + " against one fee " + money(w.fee) + " -> " + w.verdict + ", stake " + money(before) +
         " -> " + money(lane["stake"].get<double>()));
    if (trading && neg >= to_research) {
      lane["stage"] = "research";
      rec["demoted"] = true;
      note(std::to_string(neg) + " negative windows in a row: back to research (promote by hand to trade it again)");
    }
    json& hist = lane["windows"];
    if (!hist.is_array()) hist = json::array();
    hist.push_back(rec);
    while (hist.size() > 12) hist.erase(hist.begin());
  }
  lane["evaluated_trades"] = done;
  return lane;
}

}  // namespace desk
