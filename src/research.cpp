#include "desk/research.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "desk/eval.h"
#include "desk/fees.h"
#include "desk/fixed.h"
#include "desk/http.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"

namespace desk {

namespace {

const char* HIST_SCHEMA = R"SQL(
CREATE TABLE IF NOT EXISTS hist_markets(
  ticker TEXT PRIMARY KEY, event_ticker TEXT, series_ticker TEXT, title TEXT, yes_sub_title TEXT,
  strike_type TEXT, floor_strike REAL, cap_strike REAL, result TEXT, settlement_ts TEXT,
  close_time TEXT, expected_expiration_time TEXT, rules_primary TEXT, last_price REAL,
  volume REAL, fetched_ts INTEGER);
CREATE INDEX IF NOT EXISTS idx_hist_markets_series ON hist_markets(series_ticker);
CREATE TABLE IF NOT EXISTS hist_trades(
  trade_id TEXT PRIMARY KEY, ticker TEXT, ts INTEGER, yes_price REAL, count REAL, taker_side TEXT);
CREATE INDEX IF NOT EXISTS idx_hist_trades ON hist_trades(ticker, ts);
CREATE TABLE IF NOT EXISTS hist_backfill(ticker TEXT PRIMARY KEY, trades INTEGER, complete INTEGER, ts INTEGER);
)SQL";

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

double gnum(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null()) return 0;
  if (it->is_number()) return it->get<double>();
  if (it->is_string()) return parse_num(it->get<std::string>());
  return 0;
}
std::string gstr(const json& r, const char* k) {
  auto it = r.find(k);
  if (it == r.end() || it->is_null() || !it->is_string()) return "";
  return it->get<std::string>();
}

// ---- backfill -----------------------------------------------------------------------------

int backfill(const std::vector<std::string>& series, int max_trade_pages, Store& store, KalshiClient& client) {
  int n_markets = 0, n_trades = 0;
  for (const auto& st : series) {
    std::map<std::string, std::string> q{{"limit", "1000"}, {"series_ticker", st}, {"status", "settled"}};
    std::vector<json> rows;
    for (int page = 0; page < 20; ++page) {
      json j = client.call("GET", "/markets", q);
      for (const auto& m : j.value("markets", json::array())) rows.push_back(m);
      std::string cursor = j.value("cursor", "");
      if (cursor.empty()) break;
      q["cursor"] = cursor;
    }
    LOG_INFO("%s: %zu settled markets", st.c_str(), rows.size());
    for (const auto& m : rows) {
      Market mk = parse_market(m);
      store.exec(
          "INSERT OR REPLACE INTO hist_markets(ticker,event_ticker,series_ticker,title,yes_sub_title,strike_type,"
          "floor_strike,cap_strike,result,settlement_ts,close_time,expected_expiration_time,rules_primary,"
          "last_price,volume,fetched_ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
          {mk.ticker, mk.event_ticker, st, mk.title, mk.yes_sub_title, mk.strike_type,
           mk.floor_strike ? SqlValue(*mk.floor_strike) : SqlValue(nullptr),
           mk.cap_strike ? SqlValue(*mk.cap_strike) : SqlValue(nullptr), mk.result, gstr(m, "settlement_ts"),
           mk.close_time, mk.expected_expiration_time, mk.rules_primary, mk.last, mk.volume, now_s()});
      ++n_markets;
      if (max_trade_pages <= 0) continue;  // markets only
      json done = store.query("SELECT complete FROM hist_backfill WHERE ticker=?", {mk.ticker});
      if (!done.empty() && done[0]["complete"].get<long long>() == 1) continue;
      std::vector<Trade> trades;
      try {
        trades = client.get_trades(mk.ticker, 0, 0, max_trade_pages);
      } catch (const std::exception& e) {
        LOG_WARN("trades %s: %s", mk.ticker.c_str(), e.what());
        continue;
      }
      for (const auto& t : trades) {
        store.exec("INSERT OR IGNORE INTO hist_trades(trade_id,ticker,ts,yes_price,count,taker_side) VALUES(?,?,?,?,?,?)",
                   {t.trade_id, t.ticker, parse_iso(t.created_time), t.yes_price, t.count, t.taker_side});
      }
      n_trades += static_cast<int>(trades.size());
      bool complete = static_cast<int>(trades.size()) < max_trade_pages * 1000;
      store.exec("INSERT OR REPLACE INTO hist_backfill(ticker,trades,complete,ts) VALUES(?,?,?,?)",
                 {mk.ticker, static_cast<long long>(trades.size()), static_cast<long long>(complete ? 1 : 0), now_s()});
    }
  }
  std::printf("backfill: %d markets, %d trades\n", n_markets, n_trades);
  return 0;
}

// ---- longshot: win rate by price at a fixed time before settlement -----------------------

int longshot(const std::vector<std::string>& series, int minutes_before, Store& store, KalshiClient& client) {
  struct B { double lo, hi; int n = 0, wins = 0; double sum_p = 0; };
  std::vector<B> buckets = {{0.005, 0.055}, {0.055, 0.105}, {0.105, 0.205}, {0.205, 0.355}, {0.355, 0.505},
                            {0.505, 0.655}, {0.655, 0.805}, {0.805, 0.905}, {0.905, 0.955}, {0.955, 0.995}};
  int used = 0, skipped = 0;
  for (const auto& st : series) {
    json ms = store.query("SELECT ticker,result,settlement_ts,close_time FROM hist_markets WHERE series_ticker=? AND result IN ('yes','no')", {st});
    for (const auto& m : ms) {
      long long t_settle = parse_iso(gstr(m, "settlement_ts"));
      if (t_settle == 0) t_settle = parse_iso(gstr(m, "close_time"));
      if (t_settle == 0) { ++skipped; continue; }
      long long t_ref = t_settle - minutes_before * 60;
      std::string tk = gstr(m, "ticker");
      json tr = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>=? ORDER BY ts DESC LIMIT 1",
                            {tk, t_ref, t_ref - 12 * 3600});
      if (tr.empty()) {
        // The backfill holds the newest pages; pull the page ending at t_ref on demand (once).
        json seen = store.query("SELECT 1 FROM kv WHERE key=?", {"ls:" + tk + ":" + std::to_string(minutes_before)});
        if (seen.empty()) {
          try {
            for (const auto& t : client.get_trades(tk, t_ref - 12 * 3600, t_ref, 1))
              store.exec("INSERT OR IGNORE INTO hist_trades(trade_id,ticker,ts,yes_price,count,taker_side) VALUES(?,?,?,?,?,?)",
                         {t.trade_id, t.ticker, parse_iso(t.created_time), t.yes_price, t.count, t.taker_side});
          } catch (const std::exception& e) {
            LOG_WARN("trades %s: %s", tk.c_str(), e.what());
          }
          store.kv_set("ls:" + tk + ":" + std::to_string(minutes_before), "1");
          tr = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>=? ORDER BY ts DESC LIMIT 1",
                           {tk, t_ref, t_ref - 12 * 3600});
        }
      }
      if (tr.empty()) { ++skipped; continue; }
      double p = gnum(tr[0], "yes_price");
      bool win = gstr(m, "result") == "yes";
      for (auto& b : buckets)
        if (p >= b.lo && p < b.hi) { b.n++; b.wins += win; b.sum_p += p; }
      ++used;
    }
  }
  std::printf("longshot study: %d markets priced %d min before settlement (%d skipped: no trade)\n", used, minutes_before, skipped);
  std::printf("%-12s %5s %8s %8s %8s   %s\n", "bucket", "n", "mean_p", "win", "z", "NO@ask net return (taker fee)");
  for (const auto& b : buckets) {
    if (b.n == 0) continue;
    double mp = b.sum_p / b.n, wr = static_cast<double>(b.wins) / b.n;
    double se = std::sqrt(std::max(1e-9, mp * (1 - mp) / b.n));
    double z = (wr - mp) / se;
    double cost = 1.0 - mp + 0.01;  // NO at the ask, one tick through
    double fee = 0.07 * cost * (1 - cost);
    double ret = ((1 - wr) - cost - fee) / cost;
    std::printf("%4.2f-%4.2f   %5d %8.3f %8.3f %8.2f   %+.3f\n", b.lo, b.hi, b.n, mp, wr, z, ret);
  }
  std::printf("z = (win rate - mean price)/binomial se. Negative z in low buckets = longshots overpriced.\n");
  return 0;
}

// ---- tied80: SPORT-1 backtest ---------------------------------------------------------------

struct GoalTimeline {
  std::string espn_id;
  long long kickoff = 0;
  std::vector<double> home_goals, away_goals;  // minutes
  int final_home = 0, final_away = 0;
};

int tied80(const std::vector<std::string>& series, Store& store, const Config& cfg, KalshiClient& client) {
  Http http("curl/8.5.0", 20);
  std::map<std::string, json> espn_cache;  // slug|date -> scoreboard
  const auto& leagues = SportsInplay::default_leagues();
  std::vector<double> ret_market_no, ret_market_no_maker, brier_market, brier_model, brier_naive;
  std::map<std::string, std::vector<double>> ret_by_league;
  int matches = 0, tied = 0, unmatched = 0, no_price = 0, draws_after_tie = 0, misaligned = 0;
  std::vector<std::string> rows_out;
  for (const auto& st : series) {
    auto lit = leagues.find(st);
    if (lit == leagues.end()) { LOG_WARN("%s: no ESPN league mapping", st.c_str()); continue; }
    json ties = store.query(
        "SELECT ticker,event_ticker,result,rules_primary FROM hist_markets WHERE series_ticker=? AND ticker LIKE '%-TIE' "
        "AND result IN ('yes','no')", {st});
    for (const auto& m : ties) {
      ++matches;
      try {
      std::string ev = gstr(m, "event_ticker"), home, away;
      if (!SportsInplay::parse_teams(gstr(m, "rules_primary"), home, away)) { ++unmatched; continue; }
      std::string date = event_date(ev);
      std::string key = lit->second + "|" + date;
      if (!espn_cache.count(key)) {
        std::string d8 = date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2);
        json parsed = json::object();
        for (int attempt = 0; attempt < 3; ++attempt) {
          try {
            HttpResponse r = http.get("https://site.api.espn.com/apis/site/v2/sports/soccer/" + lit->second + "/scoreboard?dates=" + d8);
            if (r.ok()) { parsed = json::parse(r.body, nullptr, false); break; }
            LOG_WARN("espn %s %s: HTTP %ld", lit->second.c_str(), date.c_str(), r.status);
          } catch (const std::exception& e) {
            LOG_WARN("espn %s %s: %s (attempt %d)", lit->second.c_str(), date.c_str(), e.what(), attempt + 1);
          }
        }
        espn_cache[key] = parsed.is_object() ? parsed : json::object();
      }
      const json& sb = espn_cache[key];
      const json events = sb.value("events", json::array());  // keep alive: best_ev points into it
      GoalTimeline g;
      double best = 0;
      const json* best_ev = nullptr;
      for (const auto& x : events) {
        if (!x.is_object() || !x.contains("competitions") || !x["competitions"].is_array() || x["competitions"].empty()) continue;
        const json& comp = x["competitions"][0];
        if (!comp.contains("competitors") || !comp["competitors"].is_array()) continue;
        std::string hn, an;
        for (const auto& c : comp["competitors"]) {
          if (!c.is_object() || !c.contains("team") || !c["team"].is_object()) continue;
          if (c.value("homeAway", "") == "home") hn = c["team"].value("displayName", "");
          else an = c["team"].value("displayName", "");
        }
        double sc = std::min(SportsInplay::name_match(home, hn), SportsInplay::name_match(away, an));
        if (sc > best) { best = sc; best_ev = &x; }
      }
      if (!best_ev || best < 0.5) { ++unmatched; continue; }
      const json& x = *best_ev;
      const json& comp = x["competitions"][0];
      g.kickoff = parse_iso(x.value("date", ""));
      std::string home_id, away_id;
      for (const auto& c : comp["competitors"]) {
        if (!c.is_object() || !c.contains("team") || !c["team"].is_object()) continue;
        std::string score = c.contains("score") && c["score"].is_string() ? c["score"].get<std::string>() : "0";
        if (c.value("homeAway", "") == "home") { home_id = c["team"].value("id", ""); g.final_home = std::atoi(score.c_str()); }
        else { away_id = c["team"].value("id", ""); g.final_away = std::atoi(score.c_str()); }
      }
      bool clockless = false;
      const json& details = comp.contains("details") && comp["details"].is_array() ? comp["details"] : json::array();
      for (const auto& d : details) {
        if (!d.is_object()) continue;
        const json& sp = d.contains("scoringPlay") ? d["scoringPlay"] : json();
        if (!sp.is_boolean() || !sp.get<bool>()) continue;
        std::string clock = d.contains("clock") && d["clock"].is_object() ? d["clock"].value("displayValue", "") : "";
        if (clock.empty()) { clockless = true; break; }  // parse_clock would file it at 45': unusable
        double minute = SportsInplay::parse_clock(clock, 2, "");
        std::string tid = d.contains("team") && d["team"].is_object() ? d["team"].value("id", "") : "";
        const json& og = d.contains("ownGoal") ? d["ownGoal"] : json();
        bool own = og.is_boolean() && og.get<bool>();
        // ESPN attributes an own goal to the scoring player's team; it counts for the opponent.
        bool for_home = (tid == home_id) != own;
        (for_home ? g.home_goals : g.away_goals).push_back(minute);
      }
      if (clockless) { ++unmatched; continue; }
      if (static_cast<int>(g.home_goals.size()) != g.final_home || static_cast<int>(g.away_goals.size()) != g.final_away) {
        ++unmatched;  // the play-by-play does not add up to the final score: goals missing
        continue;
      }
      int h80 = 0, a80 = 0;
      for (double t : g.home_goals) if (t <= 80) ++h80;
      for (double t : g.away_goals) if (t <= 80) ++a80;
      if (h80 != a80) continue;
      ++tied;
      // Market price of the tie at the decision time: kickoff + 80' + halftime 15' + ~3' stoppage.
      // Only prints BEFORE that moment are usable; anything after it has seen minutes 81+.
      long long t80 = g.kickoff + (80 + 15 + 3) * 60;
      json tr = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>? ORDER BY ts DESC LIMIT 1",
                            {gstr(m, "ticker"), t80, t80 - 300});
      if (tr.empty()) { ++no_price; continue; }
      double p80 = gnum(tr[0], "yes_price");
      if (p80 < 0.10 || p80 > 0.90) { ++misaligned; continue; }  // level at 80' cannot trade at 1c: clock alignment failed
      // Pre-match prices for the three markets -> Poisson lambdas -> model draw prob at 80'.
      auto last_before = [&](const std::string& t) -> double {
        json q = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>=? ORDER BY ts DESC LIMIT 1",
                             {t, g.kickoff, g.kickoff - 6 * 3600});
        if (!q.empty()) return gnum(q[0], "yes_price");
        // The backfill keeps the most recent pages of the tape; pull the pre-kickoff page on demand.
        try {
          for (const auto& tr : client.get_trades(t, g.kickoff - 6 * 3600, g.kickoff, 1))
            store.exec("INSERT OR IGNORE INTO hist_trades(trade_id,ticker,ts,yes_price,count,taker_side) VALUES(?,?,?,?,?,?)",
                       {tr.trade_id, tr.ticker, parse_iso(tr.created_time), tr.yes_price, tr.count, tr.taker_side});
        } catch (const std::exception& e) {
          LOG_WARN("pre-match trades %s: %s", t.c_str(), e.what());
        }
        q = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>=? ORDER BY ts DESC LIMIT 1",
                        {t, g.kickoff, g.kickoff - 6 * 3600});
        return q.empty() ? -1 : gnum(q[0], "yes_price");
      };
      json sibs = store.query("SELECT ticker,title,yes_sub_title,rules_primary FROM hist_markets WHERE event_ticker=?", {ev});
      std::string home_t, away_t;
      for (const auto& sm : sibs) {
        Market mk;
        mk.ticker = gstr(sm, "ticker");
        mk.title = gstr(sm, "title");
        mk.yes_sub_title = gstr(sm, "yes_sub_title");
        mk.rules_primary = gstr(sm, "rules_primary");
        std::string side = SportsInplay::market_side(mk, home, away);
        if (side == "home") home_t = mk.ticker;
        else if (side == "away") away_t = mk.ticker;
      }
      double pd = last_before(gstr(m, "ticker")), ph = home_t.empty() ? -1 : last_before(home_t), pa = away_t.empty() ? -1 : last_before(away_t);
      double model = -1;
      if (pd > 0 && ph > 0 && pa > 0) {
        double s = pd + ph + pa;
        std::pair<double, double> lam = fit_lambdas(ph / s, pd / s, -0.13);
        double rem = 15.0 / 95.0 * 1.2;
        model = match_probs(lam.first * rem, lam.second * rem, 0).draw;
      }
      bool draw = gstr(m, "result") == "yes";
      if (draw) ++draws_after_tie;
      double y = draw ? 1.0 : 0.0;
      brier_market.push_back((p80 - y) * (p80 - y));
      if (model >= 0) brier_model.push_back((model - y) * (model - y));
      brier_naive.push_back((0.65 - y) * (0.65 - y));  // "tied at 80 -> 65% draw" rule of thumb
      double cost = 1.0 - p80 + 0.01;  // NO at the ask: one tick through the last print
      double fee = std::ceil(0.07 * cost * (1 - cost) * 10000) / 10000;  // hitting the ask is a taker trade
      double r = ((draw ? 0.0 : 1.0) - cost - fee) / cost;
      ret_market_no.push_back(r);
      ret_by_league[st].push_back(r);
      double cost_m = 1.0 - p80;  // resting at the print (maker): fills are not guaranteed
      double fee_m = std::ceil(0.0175 * cost_m * (1 - cost_m) * 10000) / 10000;
      ret_market_no_maker.push_back(((draw ? 0.0 : 1.0) - cost_m - fee_m) / cost_m);
      char buf[240];
      std::snprintf(buf, sizeof buf, "%-34s %s %d-%d at 80' final %d-%d  tie px80 %.2f model %.2f -> %s", ev.c_str(),
                    date.c_str(), h80, a80, g.final_home, g.final_away, p80, model, draw ? "DRAW" : "broken");
      rows_out.push_back(buf);
      } catch (const std::exception& e) {
        LOG_WARN("tied80 %s: %s", gstr(m, "event_ticker").c_str(), e.what());
        ++unmatched;
      }
    }
  }
  for (const auto& r : rows_out) std::printf("%s\n", r.c_str());
  BootCI rn = bootstrap_mean(ret_market_no);
  BootCI bm = bootstrap_mean(brier_market), bmo = bootstrap_mean(brier_model), bn = bootstrap_mean(brier_naive);
  std::printf("\nSPORT-1 backtest: %d settled matches, %d unmatched to ESPN, %d tied at 80', %d without an 80' print, %d dropped (clock misaligned)\n",
              matches, unmatched, tied, no_price, misaligned);
  int n = static_cast<int>(ret_market_no.size());
  if (n > 0) {
    double emp = static_cast<double>(draws_after_tie) / n;
    std::printf("tied at 80' with a price: n=%d; ended level %d (%.1f%%)\n", n, draws_after_tie, 100 * emp);
    std::printf("Brier: market@80 %.3f  poisson model %.3f (n=%d)  naive 0.65 %.3f\n", bm.mean, bmo.mean, bmo.n, bn.mean);
    BootCI rm = bootstrap_mean(ret_market_no_maker);
    std::printf("buy NO on tie at the 80' ask (taker fee 7%%): mean net return %+.3f per $, 90%% CI [%+.3f, %+.3f]\n", rn.mean, rn.lo, rn.hi);
    std::printf("  resting at the last print instead (maker fee, fill NOT guaranteed): %+.3f [%+.3f, %+.3f]\n", rm.mean, rm.lo, rm.hi);
    for (auto& [k, v] : ret_by_league) {
      BootCI b = bootstrap_mean(v);
      std::printf("  %-22s n=%3d  ret %+.3f [%+.3f, %+.3f]\n", k.c_str(), b.n, b.mean, b.lo, b.hi);
    }
    std::printf("(bootstrap CI; %d matches is a first look, the hypothesis asks for 120)\n", n);
  }
  return 0;
}

// ---- wx-settle: settled bracket vs NWS station daily max (IEM ASOS) -----------------------

int wx_settle(const std::vector<std::string>& series, Store& store) {
  static const std::map<std::string, std::string> iem = {
      {"KXHIGHNY", "NYC"}, {"KXHIGHCHI", "MDW"}, {"KXHIGHMIA", "MIA"}, {"KXHIGHLAX", "LAX"},
      {"KXHIGHHOU", "IAH"}, {"KXHIGHAUS", "AUS"}, {"KXHIGHPHIL", "PHL"}, {"KXDENHIGH", "DEN"}};
  static const std::map<std::string, std::string> tz = {
      {"NYC", "America/New_York"}, {"MDW", "America/Chicago"}, {"MIA", "America/New_York"}, {"LAX", "America/Los_Angeles"},
      {"IAH", "America/Chicago"}, {"AUS", "America/Chicago"}, {"PHL", "America/New_York"}, {"DEN", "America/Denver"}};
  Http http("curl/8.5.0", 60);
  int days = 0, agree = 0, within1 = 0;
  std::map<int, int> diff_hist;
  for (const auto& st : series) {
    auto it = iem.find(st);
    if (it == iem.end()) { LOG_WARN("%s: no IEM station mapping", st.c_str()); continue; }
    // Settled bracket per event: the market that resolved YES.
    json ms = store.query(
        "SELECT event_ticker,strike_type,floor_strike,cap_strike FROM hist_markets WHERE series_ticker=? AND result='yes' "
        "ORDER BY event_ticker", {st});
    if (ms.empty()) continue;
    std::string d0 = event_date(gstr(ms[0], "event_ticker")), d1 = event_date(gstr(ms[ms.size() - 1], "event_ticker"));
    // One IEM pull per station for the whole range (hourly + special obs).
    char url[512];
    std::snprintf(url, sizeof url,
                  "https://mesonet.agron.iastate.edu/cgi-bin/request/asos.py?station=%s&data=tmpf&year1=%s&month1=%s&day1=%s"
                  "&year2=%s&month2=%s&day2=%s&tz=%s&format=comma&latlon=no&missing=M&trace=T&direct=no&report_type=2&report_type=3",
                  it->second.c_str(), d0.substr(0, 4).c_str(), d0.substr(5, 2).c_str(), d0.substr(8, 2).c_str(),
                  d1.substr(0, 4).c_str(), d1.substr(5, 2).c_str(), std::to_string(std::atoi(d1.substr(8, 2).c_str()) + 1).c_str(),
                  url_encode(tz.at(it->second)).c_str());
    HttpResponse r = http.get(url);
    if (!r.ok()) { LOG_WARN("IEM %s HTTP %ld", it->second.c_str(), r.status); continue; }
    std::map<std::string, double> daymax;
    std::istringstream in(r.body);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || line.rfind("station", 0) == 0) continue;
      auto f = split(line, ',');
      if (f.size() < 3 || f[2] == "M") continue;
      std::string date = f[1].substr(0, 10);
      double t = std::atof(f[2].c_str());
      auto dm = daymax.find(date);
      if (dm == daymax.end() || t > dm->second) daymax[date] = t;
    }
    for (const auto& m : ms) {
      std::string date = event_date(gstr(m, "event_ticker"));
      auto dm = daymax.find(date);
      if (dm == daymax.end()) continue;
      double obs = std::round(dm->second);
      std::string stype = gstr(m, "strike_type");
      double lo = -999, hi = 999;
      if (stype == "greater") lo = gnum(m, "floor_strike") + 1;
      else if (stype == "less") hi = gnum(m, "cap_strike") - 1;
      else { lo = gnum(m, "floor_strike"); hi = gnum(m, "cap_strike"); }
      ++days;
      bool ok = obs >= lo && obs <= hi;
      if (ok) ++agree;
      double d = obs < lo ? obs - lo : (obs > hi ? obs - hi : 0);
      if (std::fabs(d) <= 1) ++within1;
      diff_hist[static_cast<int>(d)]++;
      std::printf("%-10s %s settled [%g, %g]  station max %.0f  %s\n", st.c_str(), date.c_str(), lo, hi, obs, ok ? "ok" : "MISMATCH");
    }
  }
  if (days) {
    std::printf("\nWX-1 settlement check: %d station-days; station daily max inside the settled bracket %d (%.0f%%), within 1F %d (%.0f%%)\n",
                days, agree, 100.0 * agree / days, within1, 100.0 * within1 / days);
    std::printf("obs - bracket (F): ");
    for (auto& [k, v] : diff_hist) std::printf("%+d:%d ", k, v);
    std::printf("\n");
  }
  return 0;
}


// ---- wx-intraday: same-day brackets from the climatology of the remaining rise vs the tape ----
//
// WX-1b. At local hour h on the event day the state is (running METAR max R_h, latest temp,
// one-hour trend). The remaining rise (final METAR max - R_h) is drawn from the empirical
// distribution of earlier years for the same station, month (+-1) and hour, then the settlement
// shift {0: .63, +1: .31, +2: .06} from wx-settle is applied. That model is scored against the
// last trade before h:00 local (Brier) and a simple taker trade rule, per hour of the day.

struct ObsDay { std::vector<std::pair<int, double>> obs; };  // (minute of local day, tmpf)

static int us_dst_offset(const std::string& date, int std_offset_hours) {
  // Second Sunday of March .. first Sunday of November (US rule since 2007).
  int Y = std::atoi(date.substr(0, 4).c_str()), M = std::atoi(date.substr(5, 2).c_str()), D = std::atoi(date.substr(8, 2).c_str());
  auto dow = [](int y, int m, int d) {  // 0 = Sunday
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
  };
  int mar_second_sunday = 8 + (7 - dow(Y, 3, 8)) % 7;
  int nov_first_sunday = 1 + (7 - dow(Y, 11, 1)) % 7;
  bool dst = (M > 3 && M < 11) || (M == 3 && D >= mar_second_sunday) || (M == 11 && D < nov_first_sunday);
  return (std_offset_hours + (dst ? 1 : 0)) * 3600;
}

static bool wx_bracket_hit(const std::string& stype, double lo, double hi, int t) {
  if (stype == "less") return t < hi;      // cap: T <= cap-1
  if (stype == "greater") return t > lo;   // floor: T >= floor+1
  return t >= lo && t <= hi;               // between, inclusive
}

int wx_intraday(const std::vector<std::string>& series, double edge, Store& store) {
  static const std::map<std::string, std::pair<std::string, int>> iem = {  // series -> (IEM id, standard UTC offset h)
      {"KXHIGHNY", {"NYC", -5}}, {"KXHIGHCHI", {"MDW", -6}}, {"KXHIGHMIA", {"MIA", -5}}, {"KXHIGHLAX", {"LAX", -8}},
      {"KXHIGHHOU", {"IAH", -6}}, {"KXHIGHAUS", {"AUS", -6}}, {"KXHIGHPHIL", {"PHL", -5}}, {"KXDENHIGH", {"DEN", -7}}};
  static const std::map<std::string, std::string> tzname = {
      {"NYC", "America/New_York"}, {"MDW", "America/Chicago"}, {"MIA", "America/New_York"}, {"LAX", "America/Los_Angeles"},
      {"IAH", "America/Chicago"}, {"AUS", "America/Chicago"}, {"PHL", "America/New_York"}, {"DEN", "America/Denver"}};
  static const std::pair<int, double> SHIFT[] = {{0, 0.63}, {1, 0.31}, {2, 0.06}};
  static const int HOURS[] = {9, 10, 11, 12, 13, 14, 15, 16};
  Http http("curl/8.5.0", 120);
  struct Row { double pm, pk, px_yes, px_no; int outcome; bool have_pk, have_yes, have_no; };  // pk: last print before; px_*: next print on that side
  std::map<int, std::vector<Row>> rows;               // hour -> rows
  std::map<int, int> shift_hist;
  int n_markets = 0;
  for (const auto& st : series) {
    auto it = iem.find(st);
    if (it == iem.end()) { LOG_WARN("%s: no IEM station mapping", st.c_str()); continue; }
    json ms = store.query(
        "SELECT ticker,event_ticker,strike_type,floor_strike,cap_strike,result FROM hist_markets "
        "WHERE series_ticker=? AND result!='' ORDER BY event_ticker", {st});
    if (ms.empty()) { LOG_WARN("%s: no settled markets in hist_markets (run research backfill)", st.c_str()); continue; }
    std::string d1 = event_date(gstr(ms[ms.size() - 1], "event_ticker"));
    int study_year = std::atoi(d1.substr(0, 4).c_str());
    // IEM's end date is exclusive: the calendar day after the last market (month/year roll over).
    std::string d_end = date_of(parse_iso(d1 + "T12:00:00Z") + 86400, 0);
    char url[512];
    std::snprintf(url, sizeof url,
                  "https://mesonet.agron.iastate.edu/cgi-bin/request/asos.py?station=%s&data=tmpf&year1=%d&month1=1&day1=1"
                  "&year2=%d&month2=%d&day2=%d&tz=%s&format=comma&latlon=no&missing=M&trace=T&direct=no&report_type=2&report_type=3",
                  it->second.first.c_str(), study_year - 4, std::atoi(d_end.substr(0, 4).c_str()),
                  std::atoi(d_end.substr(5, 2).c_str()), std::atoi(d_end.substr(8, 2).c_str()),
                  url_encode(tzname.at(it->second.first)).c_str());
    HttpResponse r = http.get(url);
    if (!r.ok()) { LOG_WARN("IEM %s HTTP %ld", it->second.first.c_str(), r.status); continue; }
    std::map<std::string, ObsDay> days;
    {
      std::istringstream in(r.body);
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("station", 0) == 0) continue;
        auto f = split(line, ',');
        if (f.size() < 3 || f[2] == "M" || f[1].size() < 16) continue;
        int minute = std::atoi(f[1].substr(11, 2).c_str()) * 60 + std::atoi(f[1].substr(14, 2).c_str());
        days[f[1].substr(0, 10)].obs.emplace_back(minute, std::atof(f[2].c_str()));
      }
      for (auto& [d, od] : days) std::sort(od.obs.begin(), od.obs.end());
    }
    // State at h:00: running max, latest temp, trend vs one hour earlier. false = no usable obs.
    auto state = [](const ObsDay& od, int h, double& rmax, double& tnow, double& trend) {
      int cut = h * 60;
      rmax = -999; tnow = -999; trend = 0;
      int last_m = -1; double prev = -999;
      for (const auto& [m, t] : od.obs) {
        if (m > cut) break;
        rmax = std::max(rmax, t);
        tnow = t; last_m = m;
        if (m <= cut - 60) prev = t;
      }
      if (last_m < 0 || last_m < cut - 90) return false;
      trend = prev > -999 ? tnow - prev : 0.0;
      return true;
    };
    auto tbucket = [](double x) { return x <= -1 ? -1 : (x >= 2 ? 1 : 0); };
    // Rise tables from the years before the study year: (month, hour, trend bucket [2 = pooled]) -> counts.
    std::map<std::tuple<int, int, int>, std::map<int, int>> tables;
    for (const auto& [d, od] : days) {
      int y = std::atoi(d.substr(0, 4).c_str()), mo = std::atoi(d.substr(5, 2).c_str());
      if (y >= study_year || od.obs.size() < 12) continue;
      double final_max = -999;
      for (const auto& [m, t] : od.obs) final_max = std::max(final_max, t);
      for (int h : HOURS) {
        double rmax, tnow, trend;
        if (!state(od, h, rmax, tnow, trend)) continue;
        int rise = static_cast<int>(std::lround(final_max - rmax));
        for (int mm : {mo - 1, mo, mo + 1}) {
          int mw = (mm + 11) % 12 + 1;
          tables[{mw, h, tbucket(trend)}][rise]++;
          tables[{mw, h, 2}][rise]++;
        }
      }
    }
    auto dist = [&](int mo, int h, double trend) -> const std::map<int, int>* {
      auto a = tables.find({mo, h, tbucket(trend)});
      int n = 0;
      if (a != tables.end()) for (auto& [k, v] : a->second) n += v;
      if (a != tables.end() && n >= 60) return &a->second;
      auto b = tables.find({mo, h, 2});
      return b == tables.end() ? nullptr : &b->second;
    };
    int out_of_study = 0;
    for (const auto& m : ms) {
      std::string date = event_date(gstr(m, "event_ticker"));
      // The rise tables exclude the study year only; a market from an earlier year would be
      // scored in-sample against its own observations.
      if (std::atoi(date.substr(0, 4).c_str()) != study_year) { ++out_of_study; continue; }
      auto dit = days.find(date);
      if (dit == days.end() || dit->second.obs.size() < 12) continue;
      ++n_markets;
      const ObsDay& od = dit->second;
      std::string stype = gstr(m, "strike_type");
      double lo = gnum(m, "floor_strike"), hi = gnum(m, "cap_strike");
      int outcome = gstr(m, "result") == "yes" ? 1 : 0;
      double final_max = -999;
      for (const auto& [mm, t] : od.obs) final_max = std::max(final_max, t);
      if (outcome == 1) {
        int k = 0;
        for (; k < 3; ++k) if (wx_bracket_hit(stype, lo, hi, static_cast<int>(std::lround(final_max)) + k)) break;
        shift_hist[k]++;
      }
      int off = us_dst_offset(date, it->second.second);
      int mo = std::atoi(date.substr(5, 2).c_str());
      long long midnight_utc = parse_iso(date + "T00:00:00Z") - off;
      std::string tk = gstr(m, "ticker");
      for (int h : HOURS) {
        double rmax, tnow, trend;
        if (!state(od, h, rmax, tnow, trend)) continue;
        const auto* dz = dist(mo, h, trend);
        if (!dz) continue;
        int n = 0;
        for (auto& [k, v] : *dz) n += v;
        double pm = 0;
        for (auto& [rise, cnt] : *dz)
          for (const auto& [k, w] : SHIFT)
            if (wx_bracket_hit(stype, lo, hi, static_cast<int>(std::lround(rmax)) + rise + k)) pm += w * cnt / n;
        long long t_dec = midnight_utc + h * 3600;
        json tr = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts<=? AND ts>? ORDER BY ts DESC LIMIT 1",
                              {tk, t_dec, t_dec - 3 * 3600});
        // Execution: the next print AFTER the decision on the side we would take. A print with
        // taker_side=yes happened at the YES ask (what a YES buyer pays); taker_side=no at the YES bid
        // (1 - that is what a NO buyer pays). Using the last print before, or any next print, mixes
        // bid and ask and flatters a model that merely disagrees with a stale price.
        json ny = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts>? AND ts<=? AND taker_side='yes' ORDER BY ts ASC LIMIT 1",
                              {tk, t_dec, t_dec + 3 * 3600});
        json nn = store.query("SELECT yes_price FROM hist_trades WHERE ticker=? AND ts>? AND ts<=? AND taker_side='no' ORDER BY ts ASC LIMIT 1",
                              {tk, t_dec, t_dec + 3 * 3600});
        Row row{pm, tr.empty() ? 0.0 : gnum(tr[0], "yes_price"), ny.empty() ? 0.0 : gnum(ny[0], "yes_price"),
                nn.empty() ? 0.0 : 1.0 - gnum(nn[0], "yes_price"), outcome, !tr.empty(), !ny.empty(), !nn.empty()};
        rows[h].push_back(row);
      }
    }
    LOG_INFO("%s: %zu station-days of obs, %zu settled markets (%d outside the study year skipped)", st.c_str(), days.size(),
             ms.size(), out_of_study);
  }
  if (!n_markets) { std::printf("wx-intraday: nothing to score\n"); return 1; }
  std::printf("WX-1b intraday climatology vs the tape: %d settled markets\n", n_markets);
  std::printf("settlement shift over the METAR max among YES brackets: ");
  for (auto& [k, v] : shift_hist) std::printf("%s%d:%d ", k == 3 ? "other" : "+", k == 3 ? 0 : k, v);
  std::printf("\n\nhour   n(tape)  Brier model  Brier market  Brier 50/50   | taker rule |model-mkt|>=%.2f, filled at the next print on our side: n  ret/$ [90%% CI]\n", edge);
  for (int h : HOURS) {
    std::vector<double> bm, bk, bb, ret, ret_yes, ret_no, tbm, tbk;
    for (const Row& r : rows[h]) {
      if (!r.have_pk) continue;
      bm.push_back((r.pm - r.outcome) * (r.pm - r.outcome));
      bk.push_back((r.pk - r.outcome) * (r.pk - r.outcome));
      double blend = 0.5 * (r.pm + r.pk);
      bb.push_back((blend - r.outcome) * (blend - r.outcome));
      if (r.pk < 0.03 || r.pk > 0.97 || std::fabs(r.pm - r.pk) < edge) continue;
      bool buy_yes = r.pm > r.pk;
      if (buy_yes ? !r.have_yes : !r.have_no) continue;
      double px = buy_yes ? r.px_yes : r.px_no;  // what a taker on that side paid at the next print
      double fee = std::ceil(0.07 * px * (1 - px) * 10000) / 10000;
      tbm.push_back(bm.back());
      tbk.push_back(bk.back());
      double x = (buy_yes ? r.outcome : 1 - r.outcome) - px - fee;
      ret.push_back(x);
      (buy_yes ? ret_yes : ret_no).push_back(x);
    }
    if (bm.empty()) { std::printf("%4d   no tape\n", h); continue; }
    BootCI cm = bootstrap_mean(bm), ck = bootstrap_mean(bk), cb = bootstrap_mean(bb);
    BootCI cr = ret.empty() ? BootCI{} : bootstrap_mean(ret);
    BootCI cy = ret_yes.empty() ? BootCI{} : bootstrap_mean(ret_yes), cn = ret_no.empty() ? BootCI{} : bootstrap_mean(ret_no);
    BootCI tm = tbm.empty() ? BootCI{} : bootstrap_mean(tbm), tk = tbk.empty() ? BootCI{} : bootstrap_mean(tbk);
    std::printf("%4d   %6zu   %.4f       %.4f        %.4f        | %4zu  %+.3f [%+.3f, %+.3f]   yes %3zu %+.3f  no %3zu %+.3f   traded-rows Brier model %.3f mkt %.3f\n",
                h, bm.size(), cm.mean, ck.mean, cb.mean, ret.size(), cr.mean, cr.lo, cr.hi, ret_yes.size(), cy.mean,
                ret_no.size(), cn.mean, tm.mean, tk.mean);
  }
  std::printf("\nReading: a lower Brier is better. If the market column is lowest at every hour, the same-day\n"
              "model has no standing to trade against the tape; keep weather_nws same-day research-only.\n");
  return 0;
}


// ---- wp-score: ESPN win probability / lead-time model / market, scored against outcomes -------
//
// SPORT-4. Every ext_wp row (one per live game every 30 s) is scored against the settled result of
// the home market (hist_markets, filled by `research backfill <series> 0`). Rows are bucketed by
// the fraction of the game left so the comparison is like for like. A trading rule is also
// scored: post at the ESPN number +- edge and assume the fill only when the market mid later
// crosses it (no fill is assumed from the tape; this is the conservative reading).

int wp_score(const std::vector<std::string>& series, double edge, Store& store) {
  (void)series;
  json rows = store.query(
      "SELECT w.ts, w.event_ticker, w.league, w.period, w.clock, w.home_score, w.away_score, w.wp_home, w.wp_leadtime, "
      "w.mkt_home, w.mkt_away, w.home_ticker, m.result FROM ext_wp w JOIN hist_markets m ON m.ticker = w.home_ticker "
      "WHERE m.result IN ('yes','no') AND w.state='in' ORDER BY w.event_ticker, w.ts", {});
  if (rows.empty()) {
    std::printf("wp-score: no scored rows yet (need ext_wp rows for games whose home market is in hist_markets; "
                "run `research backfill KXNCAAFGAME,KXNFLGAME,... 0` after games settle)\n");
    return 1;
  }
  // Brier accumulators per league|period bucket (rows), and the trading rule per game|bucket:
  // at most ONE order per game and bucket, posted at the ESPN number minus the edge on the side
  // we take, and counted as filled only when a LATER mid of the same game crosses that price.
  // Maker fee on the fill. Rows 30 s apart are not independent; trades are one per game-bucket.
  struct Acc { std::vector<double> b_espn, b_lt, b_mkt, ret; int n = 0, orders = 0; };
  std::map<std::string, Acc> by_bucket;  // "league|Pn"
  std::set<std::string> games;
  std::vector<double> ret_all;
  // Group rows by game (rows arrive ordered by event_ticker, ts).
  size_t i = 0;
  while (i < rows.size()) {
    size_t j = i;
    std::string ev = gstr(rows[i], "event_ticker");
    while (j < rows.size() && gstr(rows[j], "event_ticker") == ev) ++j;
    games.insert(ev);
    int outcome = gstr(rows[i], "result") == "yes" ? 1 : 0;
    std::set<std::string> ordered;  // buckets already carrying an order in this game
    for (size_t k = i; k < j; ++k) {
      const json& r = rows[k];
      double wp = gnum(r, "wp_home"), lt = gnum(r, "wp_leadtime"), mk = gnum(r, "mkt_home");
      if (mk <= 0 || mk >= 1) continue;
      int period = static_cast<int>(gnum(r, "period"));
      std::string bucket = gstr(r, "league") + "|P" + std::to_string(std::max(1, std::min(period, 9)));
      Acc& a = by_bucket[bucket];
      ++a.n;
      if (wp >= 0) a.b_espn.push_back((wp - outcome) * (wp - outcome));
      if (lt >= 0) a.b_lt.push_back((lt - outcome) * (lt - outcome));
      a.b_mkt.push_back((mk - outcome) * (mk - outcome));
      if (wp < 0 || std::fabs(wp - mk) < edge || ordered.count(bucket)) continue;
      ordered.insert(bucket);
      ++a.orders;
      bool buy_home = wp > mk;
      // Our resting price for the side we take: the ESPN number less the edge (YES on home when
      // ESPN is higher, YES on away = NO on home when lower).
      double px = buy_home ? wp - edge : (1 - wp) - edge;
      px = std::max(0.01, std::min(0.99, std::floor(px * 100 + 1e-9) / 100));
      bool filled = false;
      for (size_t l = k + 1; l < j; ++l) {
        double mk2 = gnum(rows[l], "mkt_home");
        if (mk2 <= 0 || mk2 >= 1) continue;
        double side_mid = buy_home ? mk2 : 1 - mk2;
        if (side_mid <= px) { filled = true; break; }  // the market came down to our bid
      }
      if (!filled) continue;
      double fee = std::ceil(0.0175 * px * (1 - px) * 10000) / 10000;  // maker
      double pay = buy_home ? outcome : 1 - outcome;
      a.ret.push_back(pay - px - fee);
      ret_all.push_back(pay - px - fee);
    }
    i = j;
  }
  std::printf("SPORT-4 live win probability vs the market: %zu games, %zu rows\n\n", games.size(), rows.size());
  std::printf("%-26s %6s  %-9s %-9s %-9s | rule: orders filled  ret/$ [90%% CI]\n", "league|period", "rows", "ESPN", "lead/time", "market");
  for (auto& [k, a] : by_bucket) {
    BootCI be = a.b_espn.empty() ? BootCI{} : bootstrap_mean(a.b_espn), bl = a.b_lt.empty() ? BootCI{} : bootstrap_mean(a.b_lt),
           bm = bootstrap_mean(a.b_mkt), rr = a.ret.empty() ? BootCI{} : bootstrap_mean(a.ret);
    std::printf("%-26s %6d  %.4f    %.4f    %.4f    | %4d %4zu  %+.3f [%+.3f, %+.3f]\n", k.c_str(), a.n, be.mean, bl.mean, bm.mean,
                a.orders, a.ret.size(), rr.mean, rr.lo, rr.hi);
  }
  if (!ret_all.empty()) {
    BootCI all = bootstrap_mean(ret_all);
    std::printf("\nall filled orders: n=%d  mean net %+.3f per contract [%+.3f, %+.3f] (maker fee; fill = a later mid crossed the price)\n",
                all.n, all.mean, all.lo, all.hi);
  }
  std::printf("\nBrier rows are 30-second snapshots, so they are not independent: judge by games (%zu), not rows.\n"
              "Orders: one per game and period bucket at ESPN -/+ %.2f, filled only if the mid later crossed it.\n",
              games.size(), edge);
  return 0;
}

}  // namespace

int run_research(const std::vector<std::string>& args, const Config& cfg, Store& store, KalshiClient& client) {
  store.exec("SELECT 1");
  {
    // schema
    std::istringstream in(HIST_SCHEMA);
    std::string stmt, line;
    while (std::getline(in, line)) {
      stmt += line + "\n";
      if (line.find(';') != std::string::npos) { store.exec(stmt); stmt.clear(); }
    }
  }
  if (args.empty()) {
    std::cerr << "research backfill <series,...> [trade_pages]\n"
                 "research longshot <series,...> [minutes_before_settlement]\n"
                 "research tied80 <series,...>\n"
                 "research wx-settle <series,...>\n"
                 "research wx-intraday <series,...> [edge=0.08]\n"
                 "research wp-score [series,...] [edge=0.06]   (SPORT-4: ext_wp rows vs settled home markets)\n";
    return 2;
  }
  std::string cmd = args[0];
  std::vector<std::string> series = args.size() > 1 ? split(args[1], ',') : std::vector<std::string>{};
  if (cmd == "backfill") return backfill(series, args.size() > 2 ? std::atoi(args[2].c_str()) : 5, store, client);
  if (cmd == "longshot") return longshot(series, args.size() > 2 ? std::atoi(args[2].c_str()) : 135, store, client);
  if (cmd == "tied80") return tied80(series, store, cfg, client);
  if (cmd == "wx-settle") return wx_settle(series, store);
  if (cmd == "wx-intraday") return wx_intraday(series, args.size() > 2 ? std::atof(args[2].c_str()) : 0.08, store);
  if (cmd == "wp-score") {
    // `research wp-score [series,...] [edge]`: the edge is whichever trailing argument is numeric.
    double e = 0.06;
    for (size_t k = 1; k < args.size(); ++k) {
      char* end = nullptr;
      double v = std::strtod(args[k].c_str(), &end);
      if (end && *end == '\0' && !args[k].empty()) e = v;
    }
    return wp_score(series, e, store);
  }
  std::cerr << "unknown research command " << cmd << "\n";
  return 2;
}

}  // namespace desk
