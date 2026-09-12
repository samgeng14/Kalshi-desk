#pragma once
// trade_decisions: the answer to "what probability and what book caused this trade?"
//
// One row per thing that happened to a decision: offered (routed MANUAL or AUTO this cycle; the
// board shows it), research (a sampled record of a research-routed candidate), approved (you
// clicked it), revalidated (the backend re-priced it just before sending), auto (the automatic
// lane took it, live or paper, at its own price and size), rejected (a gate, the risk checks or the
// exchange refused it), unknown (sent, but its delivery is not known: a 5xx or a timeout),
// submitted (the executor sent an order on it). Each row carries the quote,
// the book and the edge breakdown as they were at that moment. Proposals carry the decision_id,
// and orders inherit it, so every order leads back to its snapshot.
#include <string>

#include "desk/store.h"
#include "desk/types.h"

namespace desk {

// Record a screen row (decision_to_row output) under `event`. Returns the row id.
long long record_decision(Store& store, const json& row, const std::string& event, long long ts,
                          long long proposal_id = 0, const std::string& note = "", double count = 0,
                          double price = -1, const std::string& style = "");
// The most recent snapshot recorded for a decision id, or null.
json last_decision_snapshot(Store& store, const std::string& decision_id);
// Called by the executor just before an order goes out. With a decision id on the proposal the
// decision's latest snapshot is copied as "submitted" (with the proposal, price, size and style);
// without one (a hand ticket, an exit, a close) a snapshot is built from the proposal and the
// market as the executor read it, and its new decision id is written onto the proposal.
void record_order_snapshot(Store& store, Proposal& p, const Market& m, const std::string& kind, long long ts);
// order id -> its proposal -> every snapshot of its decision.
json decision_trace(Store& store, const std::string& order_id);

}  // namespace desk
