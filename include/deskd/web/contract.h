#pragma once
// JSON in the shapes of ui/desk/js/contract.js (version 1.4.0), the single statement of what
// deskd serves the screen. "If the backend and this file disagree, this file wins": the tests
// run contract.js's own validator over what these builders produce (tests/deskd/contract_check.cjs).
//
// 1.4 (desk/frontend-v3, merged 2026-10-07): hb.date and snapshot.date (today's ET date); GET
// /api/universe and POST /api/watch follow with the web worker's guards. A 1.2 hb has no date, which 1.4's
// validator refuses, and the page's store drops an invalid event: the 1.4 page served by a 1.2
// deskd sat at SRV DOWN for good (review, 2026-10-06).
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "deskd/bus/msg.h"
#include "deskd/md/feed.h"
#include "deskd/web/streams.h"

namespace deskd {

constexpr const char* kContractVersion = "1.4.0";

// {"ok":false,"err":{"code":code,"msg":msg}}
nlohmann::json err_body(const std::string& code, const std::string& msg);
// the HTTP status the contract gives an error code (FORBIDDEN 403, BAD_STATE 409 ...); 500 if unknown
int err_http(const std::string& code);

nlohmann::json run_json(const RunView& r);
nlohmann::json preflight_json(const std::vector<PreflightRow>& rows);
nlohmann::json feed_json(const FeedView& f);
nlohmann::json feeds_json(const Feeds& f);
nlohmann::json subs_json(const Subs& s);

// The ET calendar date ("2026-10-07") of a wall-clock time, and the day after it: THE
// UNIVERSE's T and T1. US Eastern by the 2007 rule (EDT from 02:00 on the second Sunday of March
// to 02:00 on the first Sunday of November), worked out here rather than through TZ and
// localtime_r, which read process-wide state another thread may change.
std::string et_date(std::int64_t wall_ms);
std::string et_date_after(const std::string& date);   // "2026-10-31" -> "2026-11-01"

// hb.data: {epoch, sid, srv_ms, date, feeds}
nlohmann::json hb_json(const std::string& epoch, const std::string& sid, std::int64_t srv_ms, const nlohmann::json& feeds);

// What the header shows before any venue is connected: no balance, no positions, no P&L.
// Money the desk does not know is "0.0000" with the venue read-only (ro), which the page shows
// as RO and refuses orders on, rather than a number that looks like a balance.
struct HdrInputs {
  bool live = false;
  RunView run;
  std::vector<PreflightRow> preflight;
  Money daily_stop = 0;
};
nlohmann::json hdr_json(const HdrInputs& in);

nlohmann::json alert_json(const std::string& id, const std::string& lvl, const std::string& src,
                          const std::string& msg, std::int64_t ts_ms);

// GET /api/snapshot with no markets, books, orders, fills, positions or lanes yet (date: the
// ET date of srv_ms)
nlohmann::json snapshot_json(const std::string& epoch, const std::string& sid, std::uint64_t seq,
                             std::int64_t srv_ms, const nlohmann::json& hdr, const nlohmann::json& alerts,
                             const nlohmann::json& feeds, const Subs& subs);

}  // namespace deskd
