# Final gate: do sibling books constrain a withheld contract, or merely surround it?

2026-09-12. `research/loo_feasible.py`. No entropy objective, no Normal assumption, no midpoint.
Every sibling's book is an interval constraint, and we ask for the tightest range any coherent
distribution could assign the withheld contract. A signal exists only when the target's executable
price falls outside that range after fees.

## The bound, which is the real result

On a verified exhaustive partition each contract is one terminal state, so the LP collapses:

    p_min = 1 - sum of sibling ASKS        p_max = 1 - sum of sibling BIDS

    feasible width = sum of the siblings' spreads

The closed form is checked against brute-force enumeration in the self-test, not asserted. What it
says matters more than this study: **the width grows linearly in the number of brackets, and no
estimator can beat it.** Maximum entropy would return a point inside this interval; it cannot
narrow it. So the ceiling on the whole sibling-surface idea is set by Kalshi's spreads and its
ladder length, not by the sophistication of the fit. That is why the max-entropy build was worth
gating rather than starting.

## Result

| | weather | crypto |
|---|---:|---:|
| partition events | 18 | 114 |
| withheld-target evaluations | 28,766 | 59,204 |
| feasible width, p10 / median / p90 | 0.010 / 0.010 / 0.060 | 0.080 / 0.200 / **0.610** |
| target price lies INSIDE the interval | 94.9% | **100.0%** |
| outside-interval edge > 1c | 2.246% | **0** |
| outside-interval edge > 2c | 0.807% | 0 |
| outside-interval edge > 3c | 0.149% | 0 |
| outside-interval edge > 5c | **0.000%** | 0 |

**Crypto is killed outright.** Not one of 59,204 evaluations put the executable price outside what
the siblings permit. The ladders are 80-188 brackets long, so the feasible width is a fifth of a
dollar at the median and sixty-one cents at the ninetieth percentile. The siblings say nothing.

## The artifact that would have faked a discovery

The first run produced 163 crypto signals, every one claiming a guaranteed edge over five cents.
All 163 were an absent offer. A recorded `yes_ask` of 0 means **there is no offer**, not that the
contract's probability is at most zero, and 8.1% of crypto bracket-quotes are in that state. Read
as a ceiling, a missing quote becomes a hard constraint and manufactures a tight interval out of an
empty book. With the guard in place - an absent side constrains nothing - the count goes to zero.
The self-test now pins this directly.

## Weather, the only survivor, and why it is still not evidence

44 trades from 11 distinct events, mean **+1.59c**, 95% CI [+0.06, +3.67]. It survives a
two-snapshot persistence requirement (41 trades, +1.57c, [+0.02, +3.66]) and it is not one event in
disguise (largest event is 14% of trades). The shuffle control returns -27.96c, so the harness can
tell a null from a signal.

But it fails the requirement that matters:

| guaranteed edge | trades | realized |
|---|---:|---:|
| 1-2c | 30 | +4.83c |
| 2-3c | 12 | **-8.05c** |
| 3-5c | 2 | +10.78c |

Realized economics do not increase with guaranteed edge; the middle bucket is the worst one. The
interval is clear of zero by six hundredths of a cent on 11 independent clusters, and nothing ever
exceeded a five-cent guaranteed edge. At 94.9% inside it sits one tenth of a percentage point from
the pre-registered kill line.

## Verdict

Crypto: closed. Weather: not killed by the letter of the rule, but it does not pass the
monotonicity condition and rests on eleven event clusters over six days of quotes. It is a pulse,
not a result, and the honest reading is that the lane is parked rather than vindicated.

Max entropy is **not** authorized by this. The feasible-set bound applies to it too: it would pick
a point inside an interval that is 1 cent wide where the market already agrees and 20 cents wide
where it does not. The only thing that would change the weather answer is more weather events, and
that is a matter of waiting rather than modelling.

Nothing here traded and nothing was wired to MANUAL or AUTO.
