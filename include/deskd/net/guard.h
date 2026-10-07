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
// Week 0 opens no venue socket, so nothing calls refuse() yet. Week 1's rule (review,
// 2026-10-06): the net/ connection factory is the only way to open a venue socket (REST,
// WebSocket, every reconnect); it takes this guard, asks it on every connect with redirects off,
// refuses plain HTTP to anything but loopback, and a test shows a non-live deskd configured with
// env prod and a production URL opens no connection. It also sets SO_NOSIGPIPE on every socket
// before OpenSSL gets it: the old desk died of SIGPIPE on a TLS reconnect, and Deskd::start's
// SIG_IGN covers deskd but not every embedder's handler. Its reads time out at 250 ms or less,
// so a quiet feed still comes back to beat() (app/ops.h). The old desk's HTTP and WebSocket
// clients stay out of deskd's link (deskd_core links none of the desk library).
#include <string>

namespace deskd {

class AccountLock;

enum class HostClass { PRODUCTION, DEMO, LOOPBACK, UNKNOWN };

// host without scheme or port, any case, an optional trailing dot
HostClass classify_host(const std::string& host);

class SendGuard {
 public:
  SendGuard(bool live, const AccountLock* lock) : live_(live), lock_(lock) {}

  // "" when a connection to host may be opened, else why not
  std::string refuse(const std::string& host) const;
  bool live() const { return live_; }

 private:
  bool live_;
  const AccountLock* lock_;
};

}  // namespace deskd
