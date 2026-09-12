#pragma once
// What an exchange refusal means for the desk.
//
// The rule used to be "any 4xx except 429 trips the kill switch". That treats a post-only order
// that met a moved book - a race the exchange settles by refusing the order, harmlessly - exactly
// like a malformed order. On 2026-09-11 02:32 UTC one such refusal ("post only cross") halted the
// systematic trader for the rest of the night and tripped the global kill.
//
// Only a refusal that says the desk itself is wrong may stop it: a malformed or impossible order,
// a signing or permission failure, or a 4xx nobody has classified yet (treated as hard until
// someone reads it, so this never loosens the old rule for anything unrecognised).
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace desk {

enum class RejectClass {
  SoftMarketState,   // the book or the market moved: post-only would cross, market closed, already gone
  SoftCollateral,    // not enough cash on that exchange shard at this moment
  Retryable,         // 429 or a server-side failure: back off and try later
  HardInvalidOrder,  // the order itself is wrong (size, price, shape): a desk bug
  Auth,              // signing, key or permission failure
  Unknown            // a 4xx nobody has classified: hard until someone reads it
};

inline const char* reject_class_name(RejectClass c) {
  switch (c) {
    case RejectClass::SoftMarketState: return "soft_market_state";
    case RejectClass::SoftCollateral: return "soft_collateral";
    case RejectClass::Retryable: return "retryable";
    case RejectClass::HardInvalidOrder: return "hard_invalid_order";
    case RejectClass::Auth: return "auth";
    case RejectClass::Unknown: return "unknown";
  }
  return "unknown";
}

// Kalshi's refusal of a post-only order that would have crossed the book: code "invalid_order",
// details "post only cross". Exact phrases only. A bare "post_only" also appears in request
// validation errors (and in anything that echoes the request), which say the desk is wrong.
inline bool is_post_only_cross(const std::string& text) {
  std::string b = text;
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
  if (has("post only cross") || has("post_only_cross") || has("post-only cross")) return true;
  // The executor's own pre-flight wording: "a post-only yes bid at 0.61 would cross the 0.61 offer".
  return (has("post-only") || has("post only")) && has("would cross");
}

// `status` is the HTTP status, `body` the exchange's error body. Deliberately narrow: an exact list
// of market-state and collateral phrases is soft; everything else keeps the old rule (a 4xx stops
// the desk). Self-trade refusals stay hard: the desk crossing its own order is a desk fault.
inline RejectClass classify_rejection(long status, const std::string& body) {
  std::string b = body;
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
  if (status == 429) return RejectClass::Retryable;
  if (status >= 500) return RejectClass::Retryable;
  if (status == 401 || status == 403) return RejectClass::Auth;
  if (is_post_only_cross(b)) return RejectClass::SoftMarketState;
  if (has("market closed") || has("market_closed") || has("market is closed") || has("closed to new orders") ||
      has("market not open") || has("market_not_open") || has("market is not open") || has("trading is paused") ||
      has("trading_paused") || has("trading halted") || has("exchange closed") || has("exchange_closed"))
    return RejectClass::SoftMarketState;
  if (has("insufficient_balance") || has("insufficient balance") || has("insufficient funds") ||
      has("insufficient_funds") || has("insufficient collateral") || has("insufficient buying power"))
    return RejectClass::SoftCollateral;
  if (status == 400 || status == 422 || has("invalid")) return RejectClass::HardInvalidOrder;
  return RejectClass::Unknown;
}

inline RejectClass reject_class_from_name(const std::string& n) {
  for (RejectClass c : {RejectClass::SoftMarketState, RejectClass::SoftCollateral, RejectClass::Retryable,
                        RejectClass::HardInvalidOrder, RejectClass::Auth, RejectClass::Unknown})
    if (n == reject_class_name(c)) return c;
  return RejectClass::Unknown;
}

// The only classes that may trip the global kill switch or halt an algorithm.
inline bool reject_trips_kill(RejectClass c) {
  return c == RejectClass::HardInvalidOrder || c == RejectClass::Auth || c == RejectClass::Unknown;
}
inline bool reject_is_soft(RejectClass c) {
  return c == RejectClass::SoftMarketState || c == RejectClass::SoftCollateral;
}

// A refusal the desk raises itself before anything is sent, for a reason the exchange would give
// as a soft one (a post-only price the book has already moved through). Never an exchange
// rejection, never a reason to stop the desk.
struct SoftRejection : std::runtime_error {
  RejectClass cls;
  SoftRejection(RejectClass c, const std::string& what) : std::runtime_error(what), cls(c) {}
};

}  // namespace desk
