#pragma once
// strat (plan section 5): quotes, lanes, models and the LIP share estimate, one maker engine for
// every lane. Today it runs the models (strat/fair.h) every 250 ms; the quoter arrives with the
// lanes.
#include <functional>

namespace deskd {

class Worker;

void run_strat(Worker& self, const std::function<void()>& step = {});

}  // namespace deskd
