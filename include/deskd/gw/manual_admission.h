#pragma once
// Pure gateway-thread admission; no network calls and no mutation. The caller must reserve
// the result and register/journal the cid in the same gateway turn, before enqueueing a write.
#include "deskd/bus/msg.h"
#include "deskd/gw/kalshi_wire.h"
#include "deskd/md/kalshi_md.h"

namespace deskd {
struct ManualContext {
  std::int64_t now_ms = 0;
  const kalshi::MarketRow* market = nullptr;
  const MdBook* book = nullptr;
  bool killed = false, read_only = true, account_ready = false;
  bool public_ready = false, private_ready = false, clock_trusted = false;
  bool publisher_healthy = false, ledger_healthy = false;
  bool manual_sender_healthy = false, cancel_sender_healthy = false;
  std::string owner_why;   // which execution owner is not ready, and why ("" = not known)
  bool trading_active = false, shard_active = false;
  bool duplicate_cid = false, unknown_order = false, foreign_order = false;
  std::string unknown_cid;   // the order whose outcome is unknown (unknown_order)
  // Current spendable cash AFTER all existing exchange and local commitments; never subtract
  // the exchange's working reservations again from its already-available REST balance.
  kalshi::Micros free = 0, shard_free = 0;
  Qty position = 0; // signed YES position on this market AND shard
  Qty reducing_committed = 0; // same-side remaining/pending qty already claiming this inventory
};
struct ManualAdmission {
  std::string code, message;
  PlaceIntent intent; // reduce_only size clipped; POST expiry resolved, ttl_s cleared
  kalshi::CreateOrder wire;
  kalshi::Micros collateral = 0, fee_check = 0;
  // Fees are checked once, not displayed as working collateral (contract 1.5).
  kalshi::Micros required() const { return collateral + fee_check; }
  bool ok() const { return code.empty(); }
};
ManualAdmission admit_manual(const PlaceIntent& intent, const ManualContext& context);
} // namespace deskd
