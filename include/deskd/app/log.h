#pragma once
// deskd's log lines, in the old desk's format (include/desk/log.h) so one grep reads both, but
// with the thread's worker name: "2026-10-06T14:02:11.204Z [warn] gw: ...". deskd keeps its
// own copy because the old code goes on 2026-11-22.
#include <cstdint>
#include <string>

namespace deskd {

std::int64_t now_ms();                  // wall clock, ms since the Unix epoch
std::string iso_ms(std::int64_t ms);    // "2026-10-06T14:02:11.204Z"

void set_thread_label(const std::string& name);   // the name log lines carry for this thread
void log_line(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace deskd

#define DLOG_INFO(...) ::deskd::log_line("info", __VA_ARGS__)
#define DLOG_WARN(...) ::deskd::log_line("warn", __VA_ARGS__)
#define DLOG_ERR(...) ::deskd::log_line("error", __VA_ARGS__)
