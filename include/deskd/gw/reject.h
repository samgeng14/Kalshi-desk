#pragma once
// What a venue refusal means for deskd: ported from include/desk/reject.h (the old desk's rule
// since 2026-09-11), with plan section 3's changes.
//
// The old rule was "any 4xx except 429 trips the kill switch", until one post-only order that met
// a moved book ("post only cross") halted the systematic trader for a night and tripped the global
// kill (2026-09-11 02:32 UTC). Only a refusal that says the desk itself is wrong may stop it: a
// malformed or impossible order, a signing or permission failure, or a refusal nobody has
// classified yet (hard until someone reads it, so nothing unrecognised loosens the rule).
//
// Changed for deskd (plan section 3, "Soft, never Kill"):
//   - a self-trade refusal is soft (the old desk had it hard): Manual's click cancelling our own
//     quote, or Systematic meeting a Manual order, is the netting rule at work, not a bug
//   - "order group triggered" is soft: the group did its job
//   - new classes the old desk never needed: DUPLICATE (409, the client_order_id is already on
//     the venue: the earlier send landed, so reconcile by cid; never re-send), GONE (a cancel's
//     404: the order already filled, expired or was cancelled)
// Which phrases mean what is matched on the venue's code, message and details together, exact
// phrases only (a bare "post_only" also appears in validation errors that say the desk is wrong).
// The demo checks (plan section 8) record each shape; the fake's code strings other than
// invalid_order are its own (tests/fake_exchange/engine.h).
#include <string>

namespace deskd {

enum class RejectClass {
  SOFT_MARKET,       // the book or market moved: post-only would cross, market closed or paused
  SOFT_COLLATERAL,   // not enough cash on that shard at this moment
  SOFT_SELF_TRADE,   // self-trade prevention refused or cancelled it
  SOFT_GROUP,        // its order group is triggered
  DUPLICATE,         // the client_order_id already exists on the venue
  GONE,              // a cancel found nothing to cancel
  RETRYABLE,         // 429: refused before processing; back off
  HARD_INVALID,      // the order itself is wrong: a desk bug
  AUTH,              // signing, key or permission
  UNKNOWN            // a refusal nobody has classified: hard until someone reads it
};

const char* reject_class_name(RejectClass c);

enum class WireOp { CREATE, CANCEL, BATCH_CANCEL, CANCEL_ALL, OTHER };

// `text` is the venue's error code, message and details (any case). HTTP status first.
RejectClass classify_refusal(WireOp op, int status, const std::string& text);

bool is_post_only_cross(const std::string& text);
// The classes that may trip KILL or halt Systematic.
inline bool reject_trips_kill(RejectClass c) {
  return c == RejectClass::HARD_INVALID || c == RejectClass::AUTH || c == RejectClass::UNKNOWN;
}
inline bool reject_is_soft(RejectClass c) {
  return c == RejectClass::SOFT_MARKET || c == RejectClass::SOFT_COLLATERAL || c == RejectClass::SOFT_SELF_TRADE ||
         c == RejectClass::SOFT_GROUP || c == RejectClass::GONE;
}

}  // namespace deskd
