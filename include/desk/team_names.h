#pragma once
// Comparing a Kalshi team name with a feed's (ESPN, The Odds API). Kalshi names are short and
// often end in a single letter that tells two clubs of one city apart ("Chicago C", "Los Angeles D",
// "New York Y"); the feeds use full names ("Chicago Cubs"). Review 2026-09-23 found the old rule
// ignored one-letter tokens, so "Chicago C" matched the White Sox as well as the Cubs, while "LA
// Rams", "NY Giants", "A's" and "Texas A&M" matched nothing.
#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace desk {

inline std::vector<std::string> team_tokens(const std::string& name) {
  std::string s;
  for (char c : name) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // The Athletics are "A's" on Kalshi.
  for (size_t p = s.find("a's"); p != std::string::npos; p = s.find("a's", p + 9))
    if ((p == 0 || !std::isalnum(static_cast<unsigned char>(s[p - 1]))) &&
        (p + 3 >= s.size() || !std::isalnum(static_cast<unsigned char>(s[p + 3]))))
      s.replace(p, 3, "athletics");
  static const std::map<std::string, std::vector<std::string>> kCity = {
      {"la", {"los", "angeles"}}, {"ny", {"new", "york"}},  {"sf", {"san", "francisco"}},
      {"kc", {"kansas", "city"}}, {"tb", {"tampa", "bay"}}, {"gb", {"green", "bay"}},
      {"lv", {"las", "vegas"}}};
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (cur.empty()) return;
    auto it = kCity.find(cur);
    if (it != kCity.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    else out.push_back(cur);
    cur.clear();
  };
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
    else flush();   // '&' splits "a&m" into a, m
  }
  flush();
  return out;
}

// Share of the shorter name's tokens found in the other, each used once. A one-letter token is an
// initial: it matches a token that starts with that letter ("c" -> "cubs").
inline double team_name_overlap(const std::string& a, const std::string& b) {
  auto ta = team_tokens(a), tb = team_tokens(b);
  if (ta.empty() || tb.empty()) return 0;
  const auto& small = ta.size() <= tb.size() ? ta : tb;
  const auto& big = ta.size() <= tb.size() ? tb : ta;
  std::vector<bool> used(big.size(), false);
  int hit = 0;
  // Whole words first, so an initial cannot take a word a whole-word match needs.
  std::vector<bool> done(small.size(), false);
  for (size_t i = 0; i < small.size(); ++i) {
    if (small[i].size() < 2) continue;
    for (size_t j = 0; j < big.size(); ++j)
      if (!used[j] && big[j] == small[i]) { used[j] = true; done[i] = true; ++hit; break; }
  }
  for (size_t i = 0; i < small.size(); ++i) {
    if (done[i] || small[i].size() != 1) continue;
    for (size_t j = 0; j < big.size(); ++j)
      if (!used[j] && big[j][0] == small[i][0]) { used[j] = true; ++hit; break; }
  }
  return static_cast<double>(hit) / static_cast<double>(small.size());
}

}  // namespace desk
