#include "desk/record.h"

#include <stdexcept>

#include "desk/log.h"
#include "desk/timeutil.h"

namespace desk {

const char* arrival_label(Arrival a) { return a == Arrival::Published ? "published" : "received"; }
const char* study_label(StudyKind k) {
  return k == StudyKind::Execution ? "execution" : "prediction_only";
}

void record_observation(Store& store, const Observation& o) {
  store.exec(
      "INSERT INTO observations(source,subject,kind,valid_ts,init_ts,published_ts,received_ts,payload) "
      "VALUES(?,?,?,?,?,?,?,?)",
      {o.source, o.subject, o.kind, o.valid_ts, o.init_ts, o.published_ts,
       o.received_ts ? o.received_ts : now_s(), o.payload});
}

ReplayCursor::ReplayCursor(Store& store, long long as_of, Arrival basis)
    : store_(store), as_of_(as_of), basis_(basis) {
  if (as_of <= 0) throw std::invalid_argument("replay: as_of must be a real time");
}

void ReplayCursor::advance_to(long long t) {
  if (t < as_of_)
    throw std::invalid_argument("replay: the clock cannot go backwards; a study that rewinds has "
                                "already seen what comes next");
  as_of_ = t;
}

std::vector<Observation> ReplayCursor::latest(const std::string& source, const std::string& subject,
                                              int limit) {
  // Under Arrival::Published a row with no known publication time is withheld: unknown lag is not
  // zero lag, and assuming it is, is how a backtest gives itself an hour of the future.
  const char* clock = basis_ == Arrival::Published ? "published_ts" : "received_ts";
  std::string sql = std::string(
      "SELECT source,subject,kind,valid_ts,init_ts,published_ts,received_ts,payload,") + clock +
      " AS arrival FROM observations WHERE source=? AND subject=? ORDER BY arrival DESC, id DESC";
  json rows = store_.query(sql, {source, subject});
  std::vector<Observation> out;
  for (const auto& r : rows) {
    long long arrival = r.value("arrival", 0LL);
    if (arrival <= 0 || arrival > as_of_) { ++withheld_; continue; }
    if (static_cast<int>(out.size()) >= limit) continue;   // keep counting what we refused
    Observation o;
    o.source = r.value("source", "");
    o.subject = r.value("subject", "");
    o.kind = r.value("kind", "");
    o.valid_ts = r.value("valid_ts", 0LL);
    o.init_ts = r.value("init_ts", 0LL);
    o.published_ts = r.value("published_ts", 0LL);
    o.received_ts = r.value("received_ts", 0LL);
    o.payload = r.value("payload", "");
    out.push_back(std::move(o));
  }
  return out;
}

}  // namespace desk
