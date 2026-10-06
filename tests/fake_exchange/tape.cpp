#include "tape.h"

#include <zlib.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fakex {

namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01 for a proleptic Gregorian date.
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
int weekday(std::int64_t days) { return static_cast<int>((days % 7 + 11) % 7); }  // 0 = Sunday
// the n-th Sunday (1-based) of a month, as a day of the month
int nth_sunday(int y, unsigned m, int n) {
  const int first = weekday(days_from_civil(y, m, 1));
  return 1 + (7 - first) % 7 + 7 * (n - 1);
}
// New York local time to UTC: daylight time from 02:00 on the second Sunday of March to 02:00 on
// the first Sunday of November (the US rule since 2007).
std::int64_t new_york_to_utc_ms(int y, unsigned mo, unsigned d, int hh, int mm) {
  auto key = [](int month, int day, int minute) { return (month * 32 + day) * 1440 + minute; };
  const int local_min = key(static_cast<int>(mo), static_cast<int>(d), hh * 60 + mm);
  const int dst_start = key(3, nth_sunday(y, 3, 2), 120);
  const int dst_end = key(11, nth_sunday(y, 11, 1), 120);
  const int offset_h = local_min >= dst_start && local_min < dst_end ? 4 : 5;
  return ((days_from_civil(y, mo, d) * 24 + hh + offset_h) * 60 + mm) * 60000LL;
}

std::string series_of(const std::string& ticker) {
  const auto dash = ticker.find('-');
  return dash == std::string::npos ? ticker : ticker.substr(0, dash);
}

// A number the venue or the archive wrote: exact fixed point when it is one, else rounded (the
// archive's REAL columns come back through Python's float repr).
std::optional<std::int64_t> number(const std::string& s, int dp, bool allow_negative) {
  if (s.empty()) return std::nullopt;
  if (auto v = parse_fixed(s, dp, allow_negative)) return v;
  char* end = nullptr;
  const double d = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || !std::isfinite(d)) return std::nullopt;
  if (d < 0 && !allow_negative) return std::nullopt;
  return std::llround(d * std::pow(10.0, dp));
}
std::optional<std::int64_t> number(const json& j, int dp, bool allow_negative) {
  if (j.is_string()) return number(j.get<std::string>(), dp, allow_negative);
  if (j.is_number()) return std::llround(j.get<double>() * std::pow(10.0, dp));
  return std::nullopt;
}

bool levels_from(const json& arr, std::vector<std::pair<Px, Qty>>& out) {
  if (arr.is_null()) return true;
  if (!arr.is_array()) return false;
  for (const auto& lv : arr) {
    if (!lv.is_array() || lv.size() < 2) return false;
    auto px = number(lv[0], 4, false);
    auto q = number(lv[1], 2, false);
    if (!px || !q) return false;
    out.emplace_back(*px, *q);
  }
  return true;
}

// Streams records out of a (possibly gzip) CSV, quoted fields spanning lines included.
class CsvStream {
 public:
  explicit CsvStream(const std::string& path) : g_(gzopen(path.c_str(), "rb")) {}
  ~CsvStream() {
    if (g_) gzclose(g_);
  }
  bool ok() const { return g_ != nullptr; }
  bool next(std::vector<std::string>& rec) {
    rec.clear();
    std::string field;
    bool quoted = false, any = false;
    for (;;) {
      int c = get();
      if (c < 0) {
        if (!any && field.empty() && rec.empty()) return false;
        rec.push_back(std::move(field));
        return true;
      }
      any = true;
      if (quoted) {
        if (c == '"') {
          if (peek() == '"') {
            get();
            field.push_back('"');
          } else {
            quoted = false;
          }
        } else {
          field.push_back(static_cast<char>(c));
        }
      } else if (c == '"') {
        quoted = true;
      } else if (c == ',') {
        rec.push_back(std::move(field));
        field.clear();
      } else if (c == '\n') {
        rec.push_back(std::move(field));
        return true;
      } else if (c != '\r') {
        field.push_back(static_cast<char>(c));
      }
    }
  }

 private:
  int get() {
    if (pos_ >= len_ && !fill()) return -1;
    return static_cast<unsigned char>(buf_[pos_++]);
  }
  int peek() {
    if (pos_ >= len_ && !fill()) return -1;
    return static_cast<unsigned char>(buf_[pos_]);
  }
  bool fill() {
    const int n = gzread(g_, buf_, sizeof buf_);
    if (n <= 0) return false;
    len_ = static_cast<std::size_t>(n);
    pos_ = 0;
    return true;
  }
  gzFile g_;
  char buf_[1 << 16];
  std::size_t pos_ = 0, len_ = 0;
};

}  // namespace

std::optional<std::int64_t> close_ms_from_ticker(const std::string& ticker) {
  const auto a = ticker.find('-');
  if (a == std::string::npos) return std::nullopt;
  const auto b = ticker.find('-', a + 1);
  const std::string s = ticker.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
  // YY MMM DD HH[MM]
  if (s.size() != 9 && s.size() != 11) return std::nullopt;
  static const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  unsigned mo = 0;
  for (unsigned i = 0; i < 12; ++i)
    if (s.compare(2, 3, months[i]) == 0) mo = i + 1;
  auto digits = [&](std::size_t pos, std::size_t n) {
    int v = 0;
    for (std::size_t i = pos; i < pos + n; ++i) {
      if (s[i] < '0' || s[i] > '9') return -1;
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  const int yy = digits(0, 2), dd = digits(5, 2), hh = digits(7, 2), mm = s.size() == 11 ? digits(9, 2) : 0;
  if (mo == 0 || yy < 0 || dd < 1 || dd > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59) return std::nullopt;
  return new_york_to_utc_ms(2000 + yy, mo, static_cast<unsigned>(dd), hh, mm);
}

MarketSpec default_spec(const std::string& ticker) {
  MarketSpec s;
  s.ticker = ticker;
  const std::string series = series_of(ticker);
  const bool fifteen = series.size() > 3 && series.compare(series.size() - 3, 3, "15M") == 0;
  const bool fx = series.find("USD") != std::string::npos || series.find("JPY") != std::string::npos ||
                  series.find("EUR") != std::string::npos;
  if (fifteen && !fx) {
    s.exchange_index = 2;
    s.ticks = tapered_deci_cent();
    s.price_level_structure = "tapered_deci_cent";
  }
  if (auto c = close_ms_from_ticker(ticker)) s.close_ms = *c;
  return s;
}

bool load_archive_csv(const std::string& path, Tape& out, std::string* err, const TapeFilter& f) {
  CsvStream in(path);
  if (!in.ok()) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::vector<std::string> rec;
  if (!in.next(rec)) {
    if (err) *err = path + " is empty";
    return false;
  }
  std::map<std::string, std::size_t> col;
  for (std::size_t i = 0; i < rec.size(); ++i) col[rec[i]] = i;
  for (const char* need : {"ts_received_ms", "ticker", "kind"})
    if (!col.count(need)) {
      if (err) *err = path + " has no " + need + " column: not a commodity_book_events archive";
      return false;
    }
  auto field = [&](const char* name) -> std::string {
    auto it = col.find(name);
    return it == col.end() || it->second >= rec.size() ? std::string() : rec[it->second];
  };
  std::int64_t last_seq = -1;
  const std::string prefix = f.series.empty() ? std::string() : f.series + "-";
  while (in.next(rec)) {
    if (rec.size() == 1 && rec[0].empty()) continue;  // a blank line
    ++out.rows;
    const std::string kind = field("kind");
    ++out.kinds[kind];
    const auto t = number(field("ts_received_ms"), 0, false);
    const auto seq = number(field("sequence"), 0, false);
    if (kind == "connect") last_seq = -1;
    if (seq) {
      if (last_seq >= 0 && *seq > last_seq + 1) ++out.seq_gaps;
      last_seq = *seq;
    }
    if (kind != "orderbook_snapshot" && kind != "orderbook_delta") continue;
    const std::string ticker = field("ticker");
    if (!t || ticker.empty()) {
      ++out.kinds["unreadable"];
      continue;
    }
    if (!prefix.empty() && ticker.compare(0, prefix.size(), prefix) != 0) continue;
    if (f.from_ms && *t < f.from_ms) continue;
    if (f.to_ms && *t > f.to_ms) break;
    TapeEvent e;
    e.t_ms = *t;
    e.ticker = ticker;
    e.seq = seq.value_or(0);
    const std::string raw = field("raw_json");
    const json j = raw.empty() ? json() : json::parse(raw, nullptr, false);
    const json* m = j.is_object() && j.contains("msg") && j["msg"].is_object() ? &j["msg"] : nullptr;
    if (kind == "orderbook_snapshot") {
      e.kind = TapeEvent::Kind::SNAPSHOT;
      if (!m) {  // a snapshot's levels exist nowhere but its frame
        ++out.kinds["snapshot_without_frame"];
        continue;
      }
      if (!levels_from(m->value("yes_dollars_fp", json()), e.yes) || !levels_from(m->value("no_dollars_fp", json()), e.no)) {
        ++out.kinds["unreadable"];
        continue;
      }
    } else {
      e.kind = TapeEvent::Kind::DELTA;
      std::optional<std::int64_t> px, dq;
      std::string side;
      if (m) {
        px = number(m->value("price_dollars", json()), 4, false);
        dq = number(m->value("delta_fp", json()), 2, true);
        side = m->value("side", std::string());
      } else {
        px = number(field("price"), 4, false);
        dq = number(field("delta"), 2, true);
        side = field("side");
      }
      if (!px || !dq || (side != "yes" && side != "no")) {
        ++out.kinds["unreadable"];
        continue;
      }
      e.no_side = side == "no";
      e.px = *px;
      e.qty = *dq;
    }
    out.events.push_back(std::move(e));
    if (f.max_events && out.events.size() >= f.max_events) break;
  }
  return true;
}

bool parse_scenario(const std::string& text, Tape& out, std::string* err) {
  std::istringstream in(text);
  std::string line;
  int n = 0;
  auto fail = [&](const std::string& why) {
    if (err) *err = "scenario line " + std::to_string(n) + ": " + why;
    return false;
  };
  while (std::getline(in, line)) {
    ++n;
    std::size_t a = line.find_first_not_of(" \t\r");
    if (a == std::string::npos || line[a] == '#') continue;
    const json j = json::parse(line, nullptr, false);
    if (!j.is_object() || !j.contains("t") || !j["t"].is_number_integer()) return fail("needs an object with integer t");
    TapeEvent e;
    e.t_ms = j["t"].get<std::int64_t>();
    auto ticker_of = [&](const json& o) { return o.is_object() ? o.value("ticker", std::string()) : std::string(); };
    if (j.contains("market")) {
      const json& o = j["market"];
      e.kind = TapeEvent::Kind::MARKET;
      e.ticker = ticker_of(o);
      if (e.ticker.empty()) return fail("market needs a ticker");
      auto spec = std::make_shared<MarketSpec>(default_spec(e.ticker));
      spec->exchange_index = o.value("exchange_index", spec->exchange_index);
      const std::string ticks = o.value("ticks", spec->price_level_structure);
      if (ticks == "tapered_deci_cent") {
        spec->ticks = tapered_deci_cent();
      } else if (ticks == "linear_cent") {
        spec->ticks = linear_cent();
      } else {
        return fail("ticks must be linear_cent or tapered_deci_cent");
      }
      spec->price_level_structure = ticks;
      const std::string fee = o.value("fee_type", std::string("quadratic"));
      if (fee == "quadratic") {
        spec->fee_type = FeeType::QUADRATIC;
      } else if (fee == "quadratic_with_maker_fees") {
        spec->fee_type = FeeType::QUADRATIC_WITH_MAKER_FEES;
      } else if (fee == "quadratic_with_combo_maker_fees") {
        spec->fee_type = FeeType::QUADRATIC_WITH_COMBO_MAKER_FEES;
      } else {
        return fail("unknown fee_type " + fee);
      }
      spec->fee_mult_milli = static_cast<int>(std::lround(o.value("fee_multiplier", 1.0) * 1000));
      spec->close_ms = o.contains("close_in_ms") ? e.t_ms + o["close_in_ms"].get<std::int64_t>() : 0;
      e.spec = std::move(spec);
    } else if (j.contains("snapshot")) {
      const json& o = j["snapshot"];
      e.kind = TapeEvent::Kind::SNAPSHOT;
      e.ticker = ticker_of(o);
      if (e.ticker.empty() || !levels_from(o.value("yes", json()), e.yes) || !levels_from(o.value("no", json()), e.no))
        return fail("snapshot needs a ticker and [[price, count], ...] levels");
    } else if (j.contains("delta")) {
      const json& o = j["delta"];
      e.kind = TapeEvent::Kind::DELTA;
      e.ticker = ticker_of(o);
      const std::string side = o.is_object() ? o.value("side", std::string()) : std::string();
      auto px = o.is_object() ? number(o.value("price", json()), 4, false) : std::nullopt;
      auto dq = o.is_object() ? number(o.value("delta", json()), 2, true) : std::nullopt;
      if (e.ticker.empty() || (side != "yes" && side != "no") || !px || !dq) return fail("delta needs ticker, side, price, delta");
      e.no_side = side == "no";
      e.px = *px;
      e.qty = *dq;
    } else if (j.contains("trade")) {
      const json& o = j["trade"];
      e.kind = TapeEvent::Kind::TRADE;
      e.ticker = ticker_of(o);
      const std::string side = o.is_object() ? o.value("taker_book_side", std::string()) : std::string();
      auto px = o.is_object() ? number(o.value("price", json()), 4, false) : std::nullopt;
      auto q = o.is_object() ? number(o.value("count", json()), 2, false) : std::nullopt;
      if (e.ticker.empty() || (side != "bid" && side != "ask") || !px || !q)
        return fail("trade needs ticker, taker_book_side (bid|ask), price, count");
      e.side = side == "bid" ? Side::BID : Side::ASK;
      e.px = *px;
      e.qty = *q;
    } else if (j.contains("pause") || j.contains("resume") || j.contains("close")) {
      const char* k = j.contains("pause") ? "pause" : j.contains("resume") ? "resume" : "close";
      e.kind = j.contains("pause") ? TapeEvent::Kind::PAUSE : j.contains("resume") ? TapeEvent::Kind::RESUME
                                                                                  : TapeEvent::Kind::CLOSE;
      e.ticker = ticker_of(j[k]);
    } else {
      return fail("unknown event");
    }
    static const char* names[] = {"snapshot", "delta", "trade", "market", "pause", "resume", "close"};
    ++out.rows;
    ++out.kinds[names[static_cast<int>(e.kind)]];
    out.events.push_back(std::move(e));
  }
  for (std::size_t i = 1; i < out.events.size(); ++i)
    if (out.events[i].t_ms < out.events[i - 1].t_ms) {
      if (err) *err = "scenario times must not go backwards";
      return false;
    }
  return true;
}

bool load_scenario(const std::string& path, Tape& out, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return parse_scenario(ss.str(), out, err);
}

}  // namespace fakex
