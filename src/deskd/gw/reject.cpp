#include "deskd/gw/reject.h"

#include <algorithm>
#include <cctype>

namespace deskd {

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
}  // namespace

const char* reject_class_name(RejectClass c) {
  switch (c) {
    case RejectClass::SOFT_MARKET: return "soft_market";
    case RejectClass::SOFT_COLLATERAL: return "soft_collateral";
    case RejectClass::SOFT_SELF_TRADE: return "soft_self_trade";
    case RejectClass::SOFT_GROUP: return "soft_group";
    case RejectClass::DUPLICATE: return "duplicate";
    case RejectClass::GONE: return "gone";
    case RejectClass::RETRYABLE: return "retryable";
    case RejectClass::HARD_INVALID: return "hard_invalid";
    case RejectClass::AUTH: return "auth";
    case RejectClass::UNKNOWN: return "unknown";
  }
  return "unknown";
}

bool is_post_only_cross(const std::string& text) {
  const std::string b = lower(text);
  auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
  if (has("post only cross") || has("post_only_cross") || has("post-only cross")) return true;
  return (has("post-only") || has("post only")) && has("would cross");
}

RejectClass classify_refusal(WireOp op, int status, const std::string& text) {
  const std::string b = lower(text);
  auto has = [&](const char* s) { return b.find(s) != std::string::npos; };
  if (status == 429) return RejectClass::RETRYABLE;
  if (status == 401 || status == 403) return RejectClass::AUTH;
  if ((op == WireOp::CANCEL || op == WireOp::BATCH_CANCEL || op == WireOp::GROUP) && status == 404) return RejectClass::GONE;
  if (status == 409 && (has("order_already_exists") || has("already exists") || has("duplicate")))
    return RejectClass::DUPLICATE;
  if (is_post_only_cross(b)) return RejectClass::SOFT_MARKET;
  if (has("self trade") || has("self-trade") || has("self_trade") || has("selftrade"))
    return RejectClass::SOFT_SELF_TRADE;
  if (has("order group triggered") || has("order_group_triggered") || has("order group is triggered"))
    return RejectClass::SOFT_GROUP;
  if (has("market closed") || has("market_closed") || has("market is closed") || has("closed to new orders") ||
      has("market not open") || has("market_not_open") || has("market is not open") || has("trading is paused") ||
      has("trading_paused") || has("trading halted") || has("exchange closed") || has("exchange_closed"))
    return RejectClass::SOFT_MARKET;
  if (has("insufficient_balance") || has("insufficient balance") || has("insufficient funds") ||
      has("insufficient_funds") || has("insufficient collateral") || has("insufficient buying power"))
    return RejectClass::SOFT_COLLATERAL;
  if (status == 400 || status == 404 || status == 422 || has("invalid")) return RejectClass::HARD_INVALID;
  return RejectClass::UNKNOWN;
}

}  // namespace deskd
