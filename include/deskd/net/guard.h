#pragma once
// The send guard: every connection deskd opens to a venue asks it first.
//
// deskd starts non-live. A production venue host (Kalshi's *.kalshi.com, Polymarket's
// *.polymarket.us and *.polymarket.com) is refused unless deskd was started with --live AND
// holds the production account's lock (app/lock.h: a non-live deskd holds the demo one, so the
// two conditions are independent); Kalshi's demo hosts (*.demo.kalshi.co) and loopback (the fake
// exchange) are allowed in either mode. Anything else is refused: the list is an allow-list, so
// a typo or a look-alike domain ("kalshi.com.example.net") never counts as either.
//
// The rule since week 1 (review, 2026-10-06; built 2026-10-07): net/conn.h's ConnFactory is the
// only way to open a venue socket (REST, WebSocket, every reconnect). It asks this guard on every
// connect, before DNS, with no redirects ever followed; refuses plain HTTP to anything but
// loopback; sets SO_NOSIGPIPE on every socket before OpenSSL gets it (the old desk died of
// SIGPIPE on a TLS reconnect, and Deskd::start's SIG_IGN covers deskd but not every embedder's
// handler); and reads in slices of 250 ms or less, so a quiet feed still comes back to beat()
// (app/ops.h). deskd-test-net shows a non-live factory, and a spare pool on it, pointed at
// production make no socket at all. The old desk's HTTP and WebSocket clients stay out of
// deskd's link (deskd_core links none of the desk library).
#include <string>
#include <vector>

namespace deskd {

class AccountLock;

enum class HostClass { PRODUCTION, DEMO, LOOPBACK, UNKNOWN };

// host without scheme or port, any case, an optional trailing dot
HostClass classify_host(const std::string& host);

class SendGuard {
 public:
  SendGuard(bool live, const AccountLock* lock) : live_(live), lock_(lock) {}
  // A guard for public read-only data (ESPN's scoreboards): exactly these hosts, any case, and
  // loopback (tests). Every venue host is refused, demo included, so a data factory can never
  // reach an account.
  static SendGuard data_hosts(std::vector<std::string> hosts);
  // deskctl's direct KILL (deskctl.cpp): it runs while a hung deskd still holds the account lock,
  // so it cannot take it. A production host is allowed only with --live AND a lock file whose
  // holder line names "deskd LIVE" (the caller reads it); demo and loopback as always.
  static SendGuard direct_kill(bool live, bool holder_is_deskd_live);

  // "" when a connection to host may be opened, else why not
  std::string refuse(const std::string& host) const;
  bool live() const { return live_; }

 private:
  bool live_;
  const AccountLock* lock_;
  bool data_ = false;
  bool direct_ = false, holder_live_ = false;
  std::vector<std::string> hosts_;   // data_ only, lower case
};

}  // namespace deskd
