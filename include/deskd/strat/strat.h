#pragma once
// strat (plan section 5): quotes, lanes, models and the LIP share estimate, one maker engine for
// every lane. Week 0 has the owner thread only; the quoter arrives in week 3.
namespace deskd {

class Worker;

void run_strat(Worker& self);

}  // namespace deskd
