#include "deskd/bus/msg.h"

namespace deskd {

const char* venue_code(Venue v) { return v == Venue::K ? "K" : "P"; }

const char* run_code(RunState s) {
  switch (s) {
    case RunState::RUN: return "RUN";
    case RunState::STOP: return "STOP";
    case RunState::KILLED: return "KILLED";
    case RunState::HALTED: return "HALTED";
  }
  return "STOP";
}

Lane lane_of(const GwMsg& m) {
  struct V {
    Lane operator()(const PlaceIntent&) const { return Lane::NORMAL; }
    Lane operator()(const AmendIntent&) const { return Lane::NORMAL; }
    Lane operator()(const Start&) const { return Lane::NORMAL; }
    Lane operator()(const Stop&) const { return Lane::CONTROL; }
    Lane operator()(const CancelIntent&) const { return Lane::PRIORITY; }
    Lane operator()(const SendResult&) const { return Lane::PRIORITY; }
    Lane operator()(const OrderEvent&) const { return Lane::PRIORITY; }
    Lane operator()(const Probe&) const { return Lane::PRIORITY; }
    Lane operator()(const ManualWrite&) const { return Lane::PRIORITY; }
    Lane operator()(const PrivateMsg&) const { return Lane::PRIORITY; }
    Lane operator()(const LaneSet&) const { return Lane::PRIORITY; }
  };
  return std::visit(V{}, m);
}

}  // namespace deskd
