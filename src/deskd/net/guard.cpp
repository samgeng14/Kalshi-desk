#include "deskd/net/guard.h"

#include <cctype>

#include "deskd/app/lock.h"

namespace deskd {

namespace {
bool ends_with_domain(const std::string& h, const std::string& domain) {
  if (h == domain) return true;
  return h.size() > domain.size() && h.compare(h.size() - domain.size(), domain.size(), domain) == 0 &&
         h[h.size() - domain.size() - 1] == '.';
}
}  // namespace

HostClass classify_host(const std::string& host) {
  std::string h;
  h.reserve(host.size());
  for (char c : host) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (!h.empty() && h.back() == '.') h.pop_back();
  if (h.empty()) return HostClass::UNKNOWN;
  for (char c : h)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'))
      return HostClass::UNKNOWN;
  if (h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]") return HostClass::LOOPBACK;
  // demo first: demo.kalshi.co sits under kalshi.co, and any other kalshi.co host counts as
  // production (an unrecognised Kalshi host needs --live, it is never assumed to be demo)
  if (ends_with_domain(h, "demo.kalshi.co") || h == "demo-api.kalshi.co") return HostClass::DEMO;
  if (ends_with_domain(h, "kalshi.com") || ends_with_domain(h, "kalshi.co")) return HostClass::PRODUCTION;
  if (ends_with_domain(h, "polymarket.us") || ends_with_domain(h, "polymarket.com")) return HostClass::PRODUCTION;
  return HostClass::UNKNOWN;
}

SendGuard SendGuard::data_hosts(std::vector<std::string> hosts) {
  SendGuard g(false, nullptr);
  g.data_ = true;
  for (auto& h : hosts) {
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    g.hosts_.push_back(std::move(h));
  }
  return g;
}

std::string SendGuard::refuse(const std::string& host) const {
  if (data_) {
    const auto cls = classify_host(host);
    if (cls == HostClass::LOOPBACK) return "";
    if (cls != HostClass::UNKNOWN) return host + " is a venue host; this connection is for public data only";
    std::string h;
    for (char c : host) h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (!h.empty() && h.back() == '.') h.pop_back();
    for (const auto& allowed : hosts_) if (h == allowed) return "";
    return host + " is not an allowed data host";
  }
  switch (classify_host(host)) {
    case HostClass::LOOPBACK:
    case HostClass::DEMO:
      return "";
    case HostClass::PRODUCTION:
      if (!live_) return host + " is a production host and deskd was not started with --live";
      if (!lock_ || !lock_->held() || lock_->env() != "prod")
        return host + " is a production host and the production account's lock is not held";
      return "";
    case HostClass::UNKNOWN:
      break;
  }
  return host + " is not a known venue host";
}

}  // namespace deskd
