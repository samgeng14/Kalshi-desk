#pragma once
// The send guard: every connection deskd opens to a venue asks it first.
//
// deskd starts non-live. A production venue host (Kalshi's *.kalshi.com, Polymarket's
// *.polymarket.us and *.polymarket.com) is refused unless deskd was started with --live AND
// holds the account lock; Kalshi's demo hosts (*.demo.kalshi.co) and loopback (the fake
// exchange) are allowed in either mode. Anything else is refused: the list is an allow-list, so
// a typo or a look-alike domain ("kalshi.com.example.net") never counts as either.
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
