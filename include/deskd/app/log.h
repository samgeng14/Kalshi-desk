#pragma once
// deskd's log lines, in the old desk's format (include/desk/log.h) so one grep reads both, but
// with the thread's worker name: "2026-10-06T14:02:11.204Z [warn] gw: ...". deskd keeps its
// own copy because the old code goes on 2026-11-22.
//
// Logging never blocks the thread that logs. Once start_logger() has run (Deskd::start does), a
// line goes into a bounded queue (4,096 lines) and a "log" thread makes the blocking write(2) to
// stderr. gw logs every refused order and send-cx every KILL; with a write(2) of their own, a
// stderr pipe nobody drained would have stopped both (review, 2026-10-06). A full queue drops
// the line and counts it (log_dropped(), in /ops/status), and the logger writes one line saying
// how many went once it can write again. Without start_logger() (deskctl, the tests' helpers) a
// line is written at once, as before.
#include <cstdint>
#include <string>

namespace deskd {

// Two clocks. The wall clock only for times that leave the process (ts_ms, srv_ms, since_ms,
// rearm_ms, the persisted kill time, log lines); the steady clock for every interval, deadline
// and timeout inside it. A wall clock stepped back X seconds stopped the 1 s hb to every page and
// the supervisor's heartbeat for X seconds, and a step forward shortened the 60 s KILL lockout.
std::int64_t now_ms();                  // wall clock, ms since the Unix epoch
std::int64_t mono_ms();                 // steady clock, ms since an arbitrary start; never steps
std::string iso_ms(std::int64_t ms);    // "2026-10-06T14:02:11.204Z"

void set_thread_label(const std::string& name);   // the name log lines carry for this thread
void log_line(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

void start_logger();                    // idempotent, process-wide
bool flush_logger(int wait_ms);         // true once every queued line is written
std::uint64_t log_dropped();            // lines dropped on a full queue since the process began

}  // namespace deskd

#define DLOG_INFO(...) ::deskd::log_line("info", __VA_ARGS__)
#define DLOG_WARN(...) ::deskd::log_line("warn", __VA_ARGS__)
#define DLOG_ERR(...) ::deskd::log_line("error", __VA_ARGS__)
