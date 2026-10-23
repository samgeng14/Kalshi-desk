#include "deskd/strat/lane_gate.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>

namespace deskd {

namespace {
using json = nlohmann::json;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
// a string field: "" when absent or null (the Python tools write None for "no reason"); any other
// type throws, and the file is malformed
std::string text_of(const json& j, const char* key) {
  if (!j.contains(key) || j.at(key).is_null()) return "";
  return j.at(key).get<std::string>();
}
// a sha256 as hashlib writes it, lower-cased: "" when absent, nullopt when not 64 hex digits
std::optional<std::string> sha_of(const json& j, const char* key) {
  const auto s = lower(text_of(j, key));
  if (s.empty() || (s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); })))
    return s;
  return std::nullopt;
}
std::string brief(const std::string& sha) { return sha.substr(0, 12); }
}  // namespace

std::string lane_gate_path(const std::string& gates_dir, const std::string& lane) { return gates_dir + "/" + lane + ".json"; }

std::optional<LaneGate> parse_lane_gate(const std::string& text, const std::string& lane, const std::string& model_sha256,
                                        const std::vector<std::string>& params, std::string* why) {
  auto fail = [&](const std::string& w) { if (why) *why = w; return std::optional<LaneGate>(); };
  const auto j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return fail("not a JSON object");
  LaneGate g;
  try {
    g.lane = text_of(j, "lane");
    if (g.lane != lane) return fail(g.lane.empty() ? "names no lane" : "is lane " + g.lane + "'s gate, not " + lane + "'s");
    g.verdict = text_of(j, "verdict");
    g.why = text_of(j, "why");
    if (g.verdict == "FAIL") return fail("verdict FAIL" + (g.why.empty() ? std::string() : ": " + g.why));
    if (g.verdict != "PASS") return fail("verdict \"" + g.verdict + "\" is neither PASS nor FAIL");
    g.stage = text_of(j, "stage");
    if (g.stage != "shadow" && g.stage != "live") return fail("unknown stage \"" + g.stage + "\" (shadow or live)");
    if (!j.contains("params") || !j.at("params").is_object()) return fail("no params object");
    g.params = j.at("params");
    for (const auto& p : params)
      if (!g.params.contains(p) || g.params.at(p).is_null()) return fail("params has no " + p);
    const auto sha = sha_of(j, "model_sha256");
    if (!sha) return fail("model_sha256 is not 64 hex digits");
    g.model_sha256 = *sha;
    const auto loaded = lower(model_sha256);
    if (g.model_sha256 != loaded) {
      if (loaded.empty()) return fail("measured model " + brief(g.model_sha256) + ", and no model is loaded");
      if (g.model_sha256.empty()) return fail("names no model, and model " + brief(loaded) + " is loaded");
      return fail("measured another model (" + brief(g.model_sha256) + ", loaded " + brief(loaded) + ")");
    }
    if (j.contains("series") && !j.at("series").is_null()) {
      if (!j.at("series").is_object()) return fail("series is not an object");
      for (const auto& [name, s] : j.at("series").items()) {
        if (!s.is_object() || !s.contains("pass") || !s.at("pass").is_boolean()) return fail("series " + name + " has no pass (true or false)");
        LaneGate::Series r;
        r.pass = s.at("pass").get<bool>();
        r.why = text_of(s, "why");
        r.level = text_of(s, "level");
        r.leader_only = s.contains("leader_only") && !s.at("leader_only").is_null() && s.at("leader_only").get<bool>();
        const auto ss = sha_of(s, "model_sha256");
        if (!ss) return fail("series " + name + ": model_sha256 is not 64 hex digits");
        r.model_sha256 = *ss;
        g.series[name] = r;
      }
    }
    g.computed = text_of(j, "computed");
    g.tool = text_of(j, "tool");
    if (j.contains("evidence") && !j.at("evidence").is_null()) g.evidence = j.at("evidence");
  } catch (const std::exception& e) {
    return fail(std::string("malformed: ") + e.what());
  }
  return g;
}

std::optional<LaneGate> load_lane_gate(const std::string& path, const std::string& lane, const std::string& model_sha256,
                                       const std::vector<std::string>& params, std::string* why) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) && !ec) { if (why) *why = "no gate file " + path; return std::nullopt; }
  std::ifstream f(path);
  if (!f) { if (why) *why = "cannot read " + path; return std::nullopt; }
  std::stringstream s;
  s << f.rdbuf();
  auto g = parse_lane_gate(s.str(), lane, model_sha256, params, why);
  if (g) g->path = path;
  else if (why) *why = path + ": " + *why;
  return g;
}

std::string lane_gate_why(const std::optional<LaneGate>& g, const std::string& why) {
  if (!g) return why.empty() ? "no gate" : why;
  if (g->live()) return "";
  return (g->path.empty() ? std::string() : g->path + ": ") + "stage shadow (shadow signals only until its gate is live)";
}

std::map<std::string, std::string> tradable_series(const LaneGate& g, const std::map<std::string, std::string>& loaded_sha) {
  std::map<std::string, std::string> out;
  for (const auto& kv : g.series) out[kv.first];
  for (const auto& kv : loaded_sha) out[kv.first];
  for (auto& [name, why] : out) {
    const auto s = g.series.find(name);
    const auto m = loaded_sha.find(name);
    if (!g.live()) why = "the gate is at stage shadow";
    else if (s == g.series.end()) why = "the gate did not measure it";
    else if (!s->second.pass) why = "did not pass the gate" + (s->second.why.empty() ? std::string() : ": " + s->second.why);
    else if (m == loaded_sha.end()) why = "no model loaded for it";
    else if (lower(m->second) != s->second.model_sha256) why = "the gate measured another model";
  }
  return out;
}

std::string sha256_hex(const std::string& bytes) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  // never "" on a failure: "" means "no model", and a gate naming none would then open
  if (EVP_Digest(bytes.data(), bytes.size(), md, &n, EVP_sha256(), nullptr) != 1 || n != 32)
    throw std::runtime_error("sha256: OpenSSL's EVP_Digest failed");
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 * n);
  for (unsigned int i = 0; i < n; ++i) {
    out += kHex[md[i] >> 4];
    out += kHex[md[i] & 15];
  }
  return out;
}

}  // namespace deskd
