#pragma once
// Recording that preserves what was actually known, and replay that refuses to know more.
//
// Every table on this desk stamps rows with one timestamp: the moment the desk wrote them. That
// conflates four different times, and a backtest built on the conflation quietly cheats:
//
//   valid_ts      the time the value is ABOUT (the hour an observation measures, the hour a
//                 forecast is for, the minute of a match)
//   init_ts       the model cycle a forecast came from. NOT when it could be read: NOAA's 12Z
//                 run is published well after 12Z, and treating the cycle label as availability
//                 hands a backtest an hour or more of the future.
//   published_ts  when the provider made it available. 0 means unknown, and unknown is not the
//                 same as zero lag.
//   received_ts   when this desk actually had it in hand. This is the only time a live decision
//                 could ever have used, so it is the default basis for replay.
#include <string>
#include <vector>

#include "desk/store.h"
#include "desk/types.h"

namespace desk {

struct Observation {
  std::string source;   // "nws.observations", "openmeteo.ensemble", "espn.scoreboard", "kalshi.book"
  std::string subject;  // station id, event ticker, market ticker
  std::string kind;     // "observation" | "forecast" | "state" | "book" | "quote"
  long long valid_ts = 0;
  long long init_ts = 0;
  long long published_ts = 0;
  long long received_ts = 0;
  std::string payload;  // raw, as received
};

// Which clock a study is allowed to read by. Received is what a live desk had. Published is the
// strictly more optimistic "we would have fetched it the instant it existed", which is a
// different claim and has to be declared rather than assumed.
enum class Arrival { Received, Published };
const char* arrival_label(Arrival a);

// What a study is entitled to conclude. A prediction-only study may compare a model against
// prices; it may not claim a trade. An execution study additionally needs a quote that was
// executable at the decision time, with size.
enum class StudyKind { PredictionOnly, Execution };
const char* study_label(StudyKind k);

void record_observation(Store& store, const Observation& o);
// Several at once, in one transaction (nothing when empty).
void record_observations(Store& store, const std::vector<Observation>& os);

// Serves only what had arrived by `as_of`. There is no way to ask it for anything later: that is
// the entire value of the type. A row whose chosen arrival clock is unknown (published_ts 0 under
// Arrival::Published) is withheld rather than assumed instant.
class ReplayCursor {
 public:
  ReplayCursor(Store& store, long long as_of, Arrival basis = Arrival::Received);

  long long as_of() const { return as_of_; }
  Arrival basis() const { return basis_; }
  // Moves the simulated clock forward. Refuses to move backwards, because a study that rewinds
  // has already seen what comes next.
  void advance_to(long long t);
  // Most recent rows for a source/subject that had arrived, newest first.
  std::vector<Observation> latest(const std::string& source, const std::string& subject, int limit = 1);
  // Rows withheld because they had not arrived yet. Counting them is how a replay proves it was
  // offered the future and declined it.
  long long withheld() const { return withheld_; }

 private:
  Store& store_;
  long long as_of_;
  Arrival basis_;
  long long withheld_ = 0;
};

}  // namespace desk
