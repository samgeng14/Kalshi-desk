#include <cmath>
#include <limits>

#include "desk/fixed.h"
#include "desk/http.h"
#include "desk/log.h"
#include "desk/mathutil.h"
#include "desk/strategies.h"
#include "desk/timeutil.h"

namespace desk {

// Default station map. Kalshi settles each city on a named climate station (CLINYC, CLIMDW, ...)
// as reported by The Weather Company; the NWS point and ASOS station below are the usual
// matches. Override any of these in config: params.weather_nws.stations.
static const struct {
  const char* series; double lat, lon; const char* obs;
} DEFAULT_STATIONS[] = {
    {"KXHIGHNY", 40.7789, -73.9692, "KNYC"},   {"KXHIGHCHI", 41.7868, -87.7522, "KMDW"},
    {"KXHIGHTNYC", 40.7789, -73.9692, "KNYC"}, {"KXHIGHTCHI", 41.7868, -87.7522, "KMDW"},
    {"KXHIGHMIA", 25.7959, -80.2870, "KMIA"},  {"KXHIGHLAX", 33.9425, -118.4081, "KLAX"},
    {"KXHIGHHOU", 29.9902, -95.3368, "KIAH"},  {"KXHIGHAUS", 30.1975, -97.6664, "KAUS"},
    {"KXHIGHPHIL", 39.8729, -75.2437, "KPHL"}, {"KXDENHIGH", 39.8561, -104.6737, "KDEN"},
};

WeatherNws::WeatherNws(const json& params, const std::string& user_agent) : http_(user_agent, 8) {
  double sigma_f = params.value("sigma_f", 2.5);
  double bias_f = params.value("bias_f", 0.4);  // hourly-sampled forecasts understate the continuous max (see wx-settle)
  refresh_seconds_ = params.value("refresh_seconds", 600);
  ens_refresh_seconds_ = params.value("ens_refresh_seconds", 1800);
  ens_weight_ = params.value("ens_weight", 0.5);
  ens_sd_inflate_ = params.value("ens_sd_inflate", 1.6);
  ens_model_ = params.value("ens_model", std::string("ecmwf_ifs025"));
  same_day_trading_ = params.value("same_day_trading", false);
  day_ahead_trading_ = params.value("day_ahead_trading", false);
  resid_k_ = params.value("resid_k", 0.6);
  same_day_sd_floor_ = params.value("same_day_sd_floor", 2.0);
  for (const auto& d : DEFAULT_STATIONS) {
    Station st;
    st.series = d.series;
    st.lat = d.lat;
    st.lon = d.lon;
    st.obs_station = d.obs;
    st.sigma_f = sigma_f;
    st.bias_f = bias_f;
    st.low = std::string(d.series).find("LOW") != std::string::npos;
    stations_[st.series] = st;
  }
  if (params.contains("stations") && params["stations"].is_object()) {
    for (auto& [series, v] : params["stations"].items()) {
      Station st = stations_.count(series) ? stations_[series] : Station{};
      st.series = series;
      st.lat = v.value("lat", st.lat);
      st.lon = v.value("lon", st.lon);
      st.obs_station = v.value("obs_station", st.obs_station);
      st.sigma_f = v.value("sigma_f", st.sigma_f);
      st.bias_f = v.value("bias_f", st.bias_f);
      st.low = v.value("low", series.find("LOW") != std::string::npos);
      stations_[series] = st;
    }
  }
}

bool WeatherNws::covers(const Market& m, const Series& s) const {
  if (s.category != "Climate and Weather") return false;
  if (!stations_.count(m.series_ticker)) return false;
  return m.strike_type == "greater" || m.strike_type == "less" || m.strike_type == "between";
}

static double c_to_f(double c) { return c * 9.0 / 5.0 + 32.0; }

void WeatherNws::refresh_station(Station& st, const std::string& date, long long now) {
  std::string key = st.series + "|" + date;
  DayForecast& d = fc_[key];
  if (now - d.fetched < refresh_seconds_) return;
  d.date = date;
  try {
    if (st.hourly_url.empty()) {
      char url[160];
      std::snprintf(url, sizeof url, "https://api.weather.gov/points/%.4f,%.4f", st.lat, st.lon);
      HttpResponse r = http_.get(url);
      if (!r.ok()) throw std::runtime_error("points HTTP " + std::to_string(r.status));
      json j = json::parse(r.body, nullptr, false);
      if (!j.is_object() || !j.contains("properties") || !j["properties"].is_object())
        throw std::runtime_error("points: no properties");
      st.hourly_url = j["properties"].value("forecastHourly", "");
      st.tz = j["properties"].value("timeZone", st.tz);
      if (st.hourly_url.empty()) throw std::runtime_error("no forecastHourly url");
    }
    // Local UTC offset for the event date: from the zone the points endpoint named, else the
    // offset a previous forecast period carried. Needed before any forecast is parsed because a
    // date that has already passed no longer appears in the hourly forecast at all.
    int off = d.offset_seconds;
    if (!st.tz.empty()) off = tz_offset_seconds(st.tz, parse_iso(date + "T12:00:00Z"));
    std::string today = date_of(now, off);
    bool past_day = today > date;   // ISO dates compare lexicographically
    bool same_day = today == date;
    std::map<long long, double> hourly;  // period start -> forecast temp (this date only)
    if (!past_day) {
      HttpResponse r = http_.get(st.hourly_url);
      if (!r.ok()) throw std::runtime_error("hourly HTTP " + std::to_string(r.status));
      json j = json::parse(r.body, nullptr, false);
      double mx = -999, mn = 999, mx_rem = -999, mn_rem = 999;
      bool any = false;
      const json* periods = nullptr;
      if (j.is_object() && j.contains("properties") && j["properties"].is_object() &&
          j["properties"].contains("periods") && j["properties"]["periods"].is_array())
        periods = &j["properties"]["periods"];
      if (!periods) throw std::runtime_error("hourly: no periods");
      for (const auto& p : *periods) {
        if (!p.is_object()) continue;
        std::string start = p.value("startTime", "");
        if (start.compare(0, 10, date) != 0) continue;
        int o = 0;
        long long t = parse_iso(start, &o);
        if (t == 0) continue;
        off = o;
        if (!p.contains("temperature") || !p["temperature"].is_number()) continue;
        double temp = p["temperature"].get<double>();
        if (p.value("temperatureUnit", "F") == "C") temp = c_to_f(temp);
        hourly[t] = temp;
        mx = std::max(mx, temp);
        mn = std::min(mn, temp);
        if (t + 3600 > now) {  // period still ahead (or in progress)
          mx_rem = std::max(mx_rem, temp);
          mn_rem = std::min(mn_rem, temp);
        }
        any = true;
      }
      if (!any) throw std::runtime_error("date not in hourly forecast");
      d.offset_seconds = off;
      today = date_of(now, off);
      same_day = today == date;
      d.fc_day_max = mx;
      d.fc_day_min = mn;
      // Same local day: the remaining-hours forecast is what the rest of the day can still do.
      d.fc_max = same_day && mx_rem > -999 ? mx_rem : mx;
      d.fc_min = same_day && mn_rem < 999 ? mn_rem : mn;
    } else if (d.fc_day_max > -999) {
      // The whole-day forecast from before midnight stays on file for the log line only.
      d.fc_max = d.fc_day_max;
      d.fc_min = d.fc_day_min;
    }
    d.offset_seconds = off;
    // Same local day, or a date that has passed while its markets are still open (they settle on
    // the next morning's climate report): pull every observation of that local day. Everything
    // since local midnight (limit=60 lost the early-afternoon max by evening on 2026-09-05: KMDW
    // showed 82.4 at 13:27 and 80.6 at 21:04). Running extremes never move backwards either.
    if (same_day || past_day) {
      char tzbuf[16];
      std::snprintf(tzbuf, sizeof tzbuf, "%c%02d:%02d", off < 0 ? '-' : '+', std::abs(off) / 3600,
                    (std::abs(off) % 3600) / 60);
      std::string url = "https://api.weather.gov/stations/" + st.obs_station + "/observations?start=" +
                        url_encode(date + "T00:00:00" + tzbuf) + "&end=" + url_encode(date + "T23:59:59" + tzbuf) +
                        "&limit=500";
      HttpResponse ro = http_.get(url);
      if (ro.ok()) {
        json jo = json::parse(ro.body, nullptr, false);
        double omx = -999, omn = 999, tnow = -999;
        long long tnow_ts = 0;
        for (const auto& f : jo.value("features", json::array())) {
          if (!f.is_object() || !f.contains("properties") || !f["properties"].is_object()) continue;
          const json& pr = f["properties"];
          std::string ts = pr.value("timestamp", "");
          long long t = parse_iso(ts);
          if (t == 0 || date_of(t, off) != date) continue;
          if (!pr.contains("temperature") || !pr["temperature"].is_object()) continue;
          const json& temp = pr["temperature"];
          if (!temp.contains("value") || !temp["value"].is_number()) continue;
          double tf = c_to_f(temp["value"].get<double>());
          omx = std::max(omx, tf);
          omn = std::min(omn, tf);
          if (t > tnow_ts) { tnow_ts = t; tnow = tf; }
        }
        if (tnow_ts > 0) {
          d.obs_max = d.have_obs ? std::max(d.obs_max, omx) : omx;
          d.obs_min = d.have_obs ? std::min(d.obs_min, omn) : omn;
          d.have_obs = true;
          if (past_day) d.day_complete = true;
          if (tnow_ts >= d.t_now_ts) { d.t_now = tnow; d.t_now_ts = tnow_ts; }
          // Residual of the latest observation against the forecast for its hour.
          auto hit = hourly.upper_bound(d.t_now_ts);
          if (hit != hourly.begin()) {
            --hit;
            if (d.t_now_ts - hit->first < 3600 * 2) {
              d.fc_at_now = hit->second;
              d.resid = d.t_now - d.fc_at_now;
              d.have_resid = true;
            }
          }
        } else if (past_day) {
          LOG_WARN("nws obs %s %s: no observations for the day", st.obs_station.c_str(), date.c_str());
        }
      } else {
        LOG_WARN("nws obs %s HTTP %ld", st.obs_station.c_str(), ro.status);
      }
    }
    d.fetched = now;
    LOG_INFO("nws %s %s fc_max %.0f fc_min %.0f obs_max %.1f have_obs %d final %d t_now %.1f resid %+.1f",
             st.series.c_str(), date.c_str(), d.fc_max, d.fc_min, d.obs_max, d.have_obs ? 1 : 0,
             d.day_complete ? 1 : 0, d.t_now, d.have_resid ? d.resid : 0.0);
  } catch (const std::exception& e) {
    LOG_WARN("nws %s %s: %s", st.series.c_str(), date.c_str(), e.what());
    d.fetched = now - refresh_seconds_ + 60;  // retry in a minute
  }
}

// Open-Meteo ensemble: per member, the daily max/min at the station point for the next days.
void WeatherNws::refresh_ensemble(Station& st, long long now) {
  if (ens_model_.empty()) return;
  if (now - ens_ts_[st.series] < ens_refresh_seconds_) return;
  ens_ts_[st.series] = now;
  try {
    char url[400];
    std::snprintf(url, sizeof url,
                  "https://ensemble-api.open-meteo.com/v1/ensemble?latitude=%.4f&longitude=%.4f"
                  "&hourly=temperature_2m&models=%s&temperature_unit=fahrenheit&forecast_days=3&timezone=auto",
                  st.lat, st.lon, ens_model_.c_str());
    HttpResponse r = http_.get(url);
    if (!r.ok()) throw std::runtime_error("open-meteo HTTP " + std::to_string(r.status));
    json j = json::parse(r.body, nullptr, false);
    const json& h = j["hourly"];
    const json& times = h["time"];
    if (!times.is_array()) throw std::runtime_error("no hourly.time");
    // member columns: temperature_2m_member01 ... (and temperature_2m = control run)
    std::vector<std::string> cols;
    for (auto it = h.begin(); it != h.end(); ++it)
      if (it.key().rfind("temperature_2m", 0) == 0) cols.push_back(it.key());
    std::map<std::string, std::vector<double>> mx, mn;  // date -> per-member max/min
    for (const auto& col : cols) {
      const json& arr = h[col];
      std::map<std::string, double> dmax, dmin;
      for (size_t i = 0; i < times.size() && i < arr.size(); ++i) {
        if (arr[i].is_null()) continue;
        std::string d = times[i].get<std::string>().substr(0, 10);
        double v = arr[i].get<double>();
        auto a = dmax.find(d);
        if (a == dmax.end()) { dmax[d] = v; dmin[d] = v; }
        else { a->second = std::max(a->second, v); dmin[d] = std::min(dmin[d], v); }
      }
      for (auto& [d, v] : dmax) { mx[d].push_back(v); mn[d].push_back(dmin[d]); }
    }
    for (auto& [d, v] : mx) {
      if (v.size() < 5) continue;
      double mean = 0;
      for (double x : v) mean += x;
      mean /= v.size();
      double var = 0;
      for (double x : v) var += (x - mean) * (x - mean);
      var /= v.size();
      double mean_min = 0;
      for (double x : mn[d]) mean_min += x;
      mean_min /= mn[d].size();
      double var_min = 0;
      for (double x : mn[d]) var_min += (x - mean_min) * (x - mean_min);
      var_min /= mn[d].size();
      DayForecast& df = fc_[st.series + "|" + d];
      df.date = d;
      df.ens_mean_max = mean;
      df.ens_sd_max = std::sqrt(var);
      df.ens_mean_min = mean_min;
      df.ens_sd_min = std::sqrt(var_min);
      df.ens_members = static_cast<int>(v.size());
      df.ens_fetched = now;
    }
    LOG_INFO("ensemble %s: %zu members, %zu days", st.series.c_str(), cols.size(), mx.size());
  } catch (const std::exception& e) {
    LOG_WARN("ensemble %s: %s", st.series.c_str(), e.what());
    ens_ts_[st.series] = now - ens_refresh_seconds_ + 120;
  }
}

void WeatherNws::prepare(const std::vector<Market>& markets, Store& store) {
  long long now = now_s();
  std::map<std::string, bool> done;
  // Each refresh is two or three HTTP round trips inside the engine cycle; cap them per cycle so
  // a slow api.weather.gov cannot stall the desk for a minute (10 stations x 2 dates otherwise).
  int budget = 3;
  for (const auto& m : markets) {
    auto it = stations_.find(m.series_ticker);
    if (it == stations_.end()) continue;
    std::string date = event_date(m.event_ticker);
    if (date.empty()) continue;
    std::string key = m.series_ticker + "|" + date;
    if (done[key]) continue;
    done[key] = true;
    long long before = fc_.count(key) ? fc_[key].fetched : 0;
    bool due = now - before >= refresh_seconds_ || now - ens_ts_[it->second.series] >= ens_refresh_seconds_;
    if (due && budget-- <= 0) continue;
    refresh_ensemble(it->second, now);
    refresh_station(it->second, date, now);
    const DayForecast& d = fc_[key];
    if (d.fetched != before && d.fetched == now) {
      store.exec(
          "INSERT INTO ext_weather(ts,series,date,fc_max,fc_min,obs_max,obs_min,have_obs,ens_mean,ens_sd) "
          "VALUES(?,?,?,?,?,?,?,?,?,?)",
          {now, m.series_ticker, date, d.fc_max, d.fc_min, d.obs_max, d.obs_min,
           static_cast<long long>(d.have_obs ? 1 : 0), it->second.low ? d.ens_mean_min : d.ens_mean_max,
           it->second.low ? d.ens_sd_min : d.ens_sd_max});
    }
  }
}

// Settled value is an integer temperature. HIGH: T = max(observed_so_far, X), X ~ N(mu, sd).
// LOW:  T = min(observed_so_far, X).
double WeatherNws::bracket_prob(const Market& m, double mu, double sd, double observed, bool have_obs, bool low) {
  const double inf = std::numeric_limits<double>::infinity();
  if (!std::isfinite(mu) || !std::isfinite(sd)) return -1;
  if (sd < 1e-3) sd = 1e-3;  // a degenerate sd must not turn the cdf into NaN
  double a = -inf, b = inf;  // want P(a <= T < b) on the continuous scale
  if (m.strike_type == "less" && m.cap_strike) b = *m.cap_strike - 0.5;            // T <= cap-1
  else if (m.strike_type == "greater" && m.floor_strike) a = *m.floor_strike + 0.5;  // T >= floor+1
  else if (m.strike_type == "between" && m.floor_strike && m.cap_strike) {
    a = *m.floor_strike - 0.5;
    b = *m.cap_strike + 0.5;
  } else return -1;
  auto cdf = [&](double t) -> double {  // P(T < t)
    if (std::isinf(t)) return t > 0 ? 1.0 : 0.0;
    if (!low) {
      if (have_obs && observed >= t) return 0.0;
      return norm_cdf((t - mu) / sd);
    }
    // low: T = min(obs, X); P(T < t) = 1 if obs < t else P(X < t)
    if (have_obs && observed < t) return 1.0;
    return norm_cdf((t - mu) / sd);
  };
  double p = cdf(b) - cdf(a);
  return std::max(0.0, std::min(1.0, p));
}

std::optional<Fair> WeatherNws::fair(const Market& m, const Series&, Store&) {
  auto sit = stations_.find(m.series_ticker);
  if (sit == stations_.end()) return std::nullopt;
  const Station& st = sit->second;
  std::string date = event_date(m.event_ticker);
  auto fit = fc_.find(m.series_ticker + "|" + date);
  if (fit == fc_.end() || fit->second.fetched == 0) return std::nullopt;
  const DayForecast& d = fit->second;
  long long now = now_override_ ? now_override_ : now_s();
  std::string today = date_of(now, d.offset_seconds);
  bool same_day = today == date;
  bool past_day = today > date;  // the local day ended; the market waits for the climate report
  if (past_day && !d.day_complete) return std::nullopt;  // no full-day observations yet: no opinion
  if (!past_day && d.fc_max <= -999) return std::nullopt;
  double observed = st.low ? d.obs_min : d.obs_max;
  bool have_obs = (same_day || past_day) && d.have_obs;
  double mu, sd;
  if (st.low) {
    mu = d.fc_min - st.bias_f;
  } else {
    mu = d.fc_max + st.bias_f;
  }
  if (past_day) {
    // Every observation of the day is in: the settled value is the day's METAR extreme plus the
    // sensor shift below. Center the residual normal far beyond the extreme so it cannot move T.
    double obs_int = std::round(observed);
    mu = st.low ? obs_int + 50.0 : obs_int - 50.0;
    sd = 0.5;
    observed = obs_int;
  } else if (same_day) {
    int hour = hour_of(now, d.offset_seconds);
    // Highs peak mid-afternoon; lows near dawn. Uncertainty shrinks as the day's extreme passes.
    double hours_left = st.low ? std::max(0.0, 6.0 - hour) : std::max(0.0, 17.0 - hour);
    double scale = std::min(1.0, std::max(0.3, hours_left / 8.0));
    // 2026-09-05 lesson: when the day is already running warmer than the hourly forecast, the
    // remaining hours run warm too (AUS: fc 96, 1:30pm obs 95 vs fc ~92, settled 100). Carry a
    // fraction of today's residual into the remaining forecast; never center below what has
    // already been observed while there is heating left.
    if (d.have_resid && hours_left > 0) mu += resid_k_ * (st.low ? -std::max(0.0, -d.resid) : std::max(0.0, d.resid));
    if (!st.low && d.have_obs && hours_left > 1 && mu < d.obs_max + 0.5) mu = d.obs_max + 0.5;
    if (st.low && d.have_obs && hours_left > 1 && mu > d.obs_min - 0.5) mu = d.obs_min - 0.5;
    sd = std::max(hours_left > 1 ? same_day_sd_floor_ : (hours_left > 0 ? 1.2 : 0.8), st.sigma_f * scale);
  } else {
    // Day-ahead: the whole-day forecast extremes (fc_max/fc_min may hold a remaining-hours
    // value if the last refresh happened on the event date itself).
    mu = st.low ? d.fc_day_min - st.bias_f : d.fc_day_max + st.bias_f;
    long long t_event = parse_iso(date + "T12:00:00Z");
    double days_ahead = std::max(1.0, static_cast<double>(t_event - now) / 86400.0);
    sd = st.sigma_f * (1.0 + 0.2 * (days_ahead - 1.0));
    // Blend in the ensemble: mean toward the member mean, sigma from the (inflated) spread.
    bool have_ens = d.ens_members >= 5 && (st.low ? d.ens_mean_min < 999 : d.ens_mean_max > -999);
    if (have_ens) {
      double ens_mu = st.low ? d.ens_mean_min : d.ens_mean_max;
      double ens_sd = st.low ? d.ens_sd_min : d.ens_sd_max;
      double nws_mu = mu;
      // Grid-point vs station bias can be large (coastal stations on hot days: LAX showed
      // ens 90 vs NWS 79). Until a per-station bias is learned, a big disagreement means
      // keep the NWS mean and widen sigma instead of averaging in a wrong number.
      double gap = std::fabs(ens_mu - nws_mu);
      double w = gap > 4.0 ? 0.0 : ens_weight_;
      mu = (1.0 - w) * nws_mu + w * ens_mu;
      sd = std::max(1.5, std::min(sd, ens_sd_inflate_ * ens_sd + 0.5 * gap));
    }
  }
  // The settled value comes from continuous sensor readings; the hourly METAR running max
  // understates it. Measured on 210 settled station-days (research wx-settle, Aug-Sep 2026):
  // settle = hourly max + 0 (63%), +1F (31%), +2F (6%). Mixture over that shift.
  static const std::pair<int, double> SHIFT[] = {{0, 0.63}, {1, 0.31}, {2, 0.06}};
  auto prob = [&](double sdev) -> double {
    if (!have_obs) return bracket_prob(m, mu, sdev, observed, false, st.low);
    double acc = 0;
    for (const auto& [k, w] : SHIFT) acc += w * bracket_prob(m, mu, sdev, observed + (st.low ? -k : k), true, st.low);
    return acc;
  };
  double p = prob(sd);
  if (p < 0) return std::nullopt;
  double p1 = prob(sd * 1.3);
  double p2 = prob(sd * 0.7);
  Fair f;
  f.p = p;
  f.sigma = 0.5 * std::fabs(p1 - p2) + 0.02;
  char buf[200];
  if (past_day)
    std::snprintf(buf, sizeof buf, "%s %s day over: obs %s %.0f (+0/+1/+2 sensor shift), awaiting climate report",
                  st.obs_station.c_str(), date.c_str(), st.low ? "min" : "max", observed);
  else if (have_obs)
    std::snprintf(buf, sizeof buf, "%s %s fc %.0f obs %.1f now %.1f resid %+.1f mu %.1f sd %.1f (same day)",
                  st.obs_station.c_str(), date.c_str(), st.low ? d.fc_min : d.fc_max, observed, d.t_now,
                  d.have_resid ? d.resid : 0.0, mu, sd);
  else if (d.ens_members >= 5)
    std::snprintf(buf, sizeof buf, "%s %s nws %.0f ens %.1f+-%.1f (n=%d) mu %.1f sd %.1f", st.obs_station.c_str(),
                  date.c_str(), st.low ? d.fc_min : d.fc_max, st.low ? d.ens_mean_min : d.ens_mean_max,
                  st.low ? d.ens_sd_min : d.ens_sd_max, d.ens_members, mu, sd);
  else
    std::snprintf(buf, sizeof buf, "%s %s fc %.0f sd %.1f", st.obs_station.c_str(), date.c_str(),
                  st.low ? d.fc_min : d.fc_max, sd);
  f.note = buf;
  f.cluster = "wx|" + date;  // all cities on one date move with the same synoptic pattern
  if (!same_day && !past_day && !day_ahead_trading_) {
    // WX-1c (research wx-dayahead, 1,404 settled markets against the archived one-day-ahead
    // ECMWF run): the tape's Brier was 0.086 and this model's 0.124 at every decision point
    // from noon the day before to 6am on the day, and the trading rule lost 3-5 cents a
    // contract at every edge threshold. Sweeping bias and sigma in sample - which flatters the
    // model - only reached 0.122, and blending toward the market improved monotonically all
    // the way to a weight of 1.0. The model carries no information the market has not already
    // priced, so day-ahead numbers are logged and not traded.
    f.research_only = true;
    f.research_reason = "day-ahead weather: the tape beat this model at every decision point (WX-1c, 1404 markets); "
                        "set params.weather_nws.day_ahead_trading to override";
  } else if ((same_day || past_day) && !same_day_trading_) {
    // WX-1b (research wx-intraday, 2,448 settled markets): the tape beat the intraday model at
    // every hour of the day. Same-day numbers are logged for the Brier sample, never traded.
    // After the day ends the only open question is the sensor shift, and the tape sees the
    // preliminary climate data before this desk does: same gate.
    f.research_only = true;
    f.research_reason = past_day
        ? "weather day over: settlement waits on the climate report; sensor-shift odds only (WX-1b gate)"
        : "same-day weather: market beat the model at every hour (WX-1b); set params.weather_nws.same_day_trading after WX-1c";
  }
  return f;
}

std::string WeatherNws::describe() const {
  std::string s = "NWS hourly + same-day obs + ECMWF ensemble;";
  int n = 0;
  for (const auto& [k, d] : fc_) {
    if (d.fetched == 0) continue;
    if (++n > 6) { s += " ..."; break; }
    char buf[96];
    std::snprintf(buf, sizeof buf, " %s max %.0f%s", k.c_str(), d.fc_max, d.have_obs ? "*" : "");
    s += buf;
  }
  return s;
}

}  // namespace desk
