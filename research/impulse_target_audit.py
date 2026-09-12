#!/usr/bin/env python3
"""Does this target use only information that existed after we could have entered?

The first deliverable of the impulse lane is not a model. It is a target that cannot lie.

A directional study computes, for each candidate trade, "what happened afterwards". Everything
turns on what *afterwards* means. Three timestamps have to be kept apart:

    t_signal    the external observation that made us interested
    t_decision  when we could have decided, having seen it
    t_entry     when an order could actually have been filled

and the requirement is t_signal <= t_decision <= t_entry, with every component of the realized
value drawn from t >= t_entry. That sounds obvious and is violated constantly, because the
violations hide inside arithmetic rather than in an obviously wrong line:

  - scoring against the mid at signal time instead of the price paid
  - crediting half the spread as it was BEFORE the fill
  - benchmarking against a quote captured when the signal fired
  - using the pre-fill spread as a feature of the exit
  - any of the above three additions deep

So this module does not ask the analyst to be careful. It tracks provenance through the
arithmetic. Every number carries the earliest timestamp that contributed to it; operators
propagate that timestamp; and a realized value whose earliest contributor predates t_entry is
rejected with the path that produced it.

The adversarial test at the bottom is the point of the whole file: data where the pre-entry spread
perfectly predicts the score and the future price is pure noise. A study without this audit reports
a beautiful edge. With it, the target is refused before any number is printed.

Usage:
  from impulse_target_audit import Stamped, Clock, TargetAudit
  python3 research/impulse_target_audit.py --self-test
"""
import argparse
import functools
import math
import random


class CausalityError(Exception):
    """A realized value drew on information from before the entry it claims to follow."""


@functools.total_ordering
class Stamped:
    """A number that remembers the earliest observation time behind it.

    Arithmetic propagates the EARLIEST contributing timestamp, because a value is only as causal
    as the oldest thing that went into it. Comparisons and float() are deliberately untracked: a
    branch on a pre-entry value is a modelling decision, not a leak into the realized number, and
    the feature side of the study is allowed to use everything up to t_decision.
    """
    __slots__ = ('v', 'ts', 'why')

    def __init__(self, v, ts, why=''):
        self.v = float(v)
        self.ts = None if ts is None else int(ts)
        self.why = why

    @staticmethod
    def _pair(a, b):
        av, ats, awhy = (a.v, a.ts, a.why) if isinstance(a, Stamped) else (float(a), None, '')
        bv, bts, bwhy = (b.v, b.ts, b.why) if isinstance(b, Stamped) else (float(b), None, '')
        if ats is None:
            ts, why = bts, bwhy
        elif bts is None:
            ts, why = ats, awhy
        elif ats <= bts:
            ts, why = ats, awhy
        else:
            ts, why = bts, bwhy
        return av, bv, ts, why

    def __add__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(a + b, ts, why)
    __radd__ = __add__

    def __sub__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(a - b, ts, why)

    def __rsub__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(b - a, ts, why)

    def __mul__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(a * b, ts, why)
    __rmul__ = __mul__

    def __truediv__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(a / b, ts, why)

    def __rtruediv__(self, o):
        a, b, ts, why = self._pair(self, o); return Stamped(b / a, ts, why)

    def __neg__(self):
        return Stamped(-self.v, self.ts, self.why)

    def __abs__(self):
        return Stamped(abs(self.v), self.ts, self.why)

    def __float__(self):
        return self.v

    def __eq__(self, o):
        return self.v == (o.v if isinstance(o, Stamped) else float(o))

    def __lt__(self, o):
        return self.v < (o.v if isinstance(o, Stamped) else float(o))

    def __repr__(self):
        return f'Stamped({self.v:g} @ {self.ts} {self.why})'


class Clock:
    """The three timestamps of one candidate trade, in microseconds."""
    __slots__ = ('signal_us', 'decision_us', 'entry_us', 'event_id')

    def __init__(self, signal_us, decision_us, entry_us, event_id=None):
        self.signal_us = int(signal_us)
        self.decision_us = int(decision_us)
        self.entry_us = int(entry_us)
        self.event_id = event_id
        if not (self.signal_us <= self.decision_us <= self.entry_us):
            raise CausalityError(
                f'the clock itself is out of order: signal {self.signal_us} <= decision '
                f'{self.decision_us} <= entry {self.entry_us} does not hold')

    @property
    def latency_us(self):
        return self.entry_us - self.signal_us


class Series:
    """Timestamped observations, read only through accessors that stamp what they return."""

    def __init__(self, name, points):
        self.name = name
        self.points = sorted(points)                    # [(ts_us, value)]
        self._ts = [t for t, _ in self.points]

    def at_or_before(self, ts_us):
        """The most recent observation at or before ts_us, stamped with ITS OWN time."""
        import bisect
        i = bisect.bisect_right(self._ts, int(ts_us)) - 1
        if i < 0:
            return None
        t, v = self.points[i]
        return Stamped(v, t, f'{self.name}@{t}')

    def first_at_or_after(self, ts_us):
        """The first observation at or after ts_us. This is what an execution looks like: you get
        the book as it is when your order arrives, not as it was when you decided."""
        import bisect
        i = bisect.bisect_left(self._ts, int(ts_us))
        if i >= len(self.points):
            return None
        t, v = self.points[i]
        return Stamped(v, t, f'{self.name}@{t}')


class TargetAudit:
    """Checks one sample's realized components, and keeps the tally."""

    def __init__(self):
        self.checked = 0
        self.rejected = 0
        self.reasons = []

    def check(self, clock, **components):
        """Every keyword is a component of the realized value. Raises on the first violation.

        Components may be Stamped or plain numbers. A plain number is accepted: it carries no
        observation behind it (a fee schedule, a constant). Anything drawn from data must be
        Stamped, which is what the Series accessors return.
        """
        self.checked += 1
        for name, value in components.items():
            if not isinstance(value, Stamped):
                continue
            if value.ts is None:
                continue
            if value.ts < clock.entry_us:
                self.rejected += 1
                why = (f'{name} drew on {value.why or "an observation"} at {value.ts}, which is '
                       f'{(clock.entry_us - value.ts)/1000:.1f} ms BEFORE entry at {clock.entry_us}')
                self.reasons.append(why)
                raise CausalityError(why)
        return True

    def summary(self):
        return {'checked': self.checked, 'rejected': self.rejected, 'reasons': self.reasons[:10]}


def markout_bps(side, entry_price, exit_price):
    """The primary target. `side` is +1 for a buy, -1 for a sell.

    Both prices must be Stamped so the audit can see where they came from. The arithmetic below
    propagates the earlier of the two, which is the entry - so a caller who passes a pre-entry
    "entry price" is caught by the check, not by review.
    """
    return side * (exit_price - entry_price) / entry_price * 10000.0


# ---- the adversarial test the whole file exists for ---------------------------------------------

def self_test():
    rng = random.Random(11)

    # A clean sample: entry at the first book observation after the latency budget, exit later.
    bids = Series('bid', [(1_000_000 + i * 100_000, 100.0 + rng.gauss(0, 0.01)) for i in range(200)])
    asks = Series('ask', [(1_000_000 + i * 100_000, 100.02 + rng.gauss(0, 0.01)) for i in range(200)])
    audit = TargetAudit()
    clock = Clock(signal_us=2_000_000, decision_us=2_010_000, entry_us=2_300_000)
    entry = asks.first_at_or_after(clock.entry_us)
    exit_ = bids.first_at_or_after(clock.entry_us + 1_000_000)
    audit.check(clock, entry_price=entry, exit_price=exit_)
    m = markout_bps(+1, entry, exit_)
    assert isinstance(m, Stamped) and m.ts >= clock.entry_us

    # The clock itself is checked.
    try:
        Clock(signal_us=5, decision_us=4, entry_us=9)
        raise AssertionError('an out-of-order clock must be refused')
    except CausalityError:
        pass

    # A pre-entry mid used as the entry price: the classic.
    bad_entry = asks.at_or_before(clock.signal_us)
    try:
        audit.check(clock, entry_price=bad_entry, exit_price=exit_)
        raise AssertionError('a signal-time entry price must be refused')
    except CausalityError as e:
        assert 'BEFORE entry' in str(e)

    # THE ADVERSARIAL CASE. Spread before entry perfectly predicts the "score"; the future price
    # is pure noise. A study that credits half the pre-entry spread reports a beautiful edge.
    # The audit must refuse the target before any number is printed.
    def build_adversarial(n=400):
        samples = []
        for k in range(n):
            t_sig = 10_000_000 + k * 1_000_000
            t_ent = t_sig + 300_000
            # Priced around 1, the way a Kalshi perp is, so the spread is a real number of basis
            # points rather than a rounding error on a 100-dollar instrument.
            spread = 0.002 + 0.008 * rng.random()          # varies, and is known before entry
            mid = 1.0
            bid_pts = [(t_sig, mid - spread / 2), (t_ent, mid - spread / 2)]
            ask_pts = [(t_sig, mid + spread / 2), (t_ent, mid + spread / 2)]
            # The future is a coin flip: there is no edge here at all.
            future = mid + rng.gauss(0, 0.0002)
            bid_pts.append((t_ent + 1_000_000, future - spread / 2))
            ask_pts.append((t_ent + 1_000_000, future + spread / 2))
            samples.append((Clock(t_sig, t_sig, t_ent, event_id=k),
                            Series('bid', bid_pts), Series('ask', ask_pts), spread))
        return samples

    samples = build_adversarial()

    # The dishonest target: assume the order rested at the bid and was filled, so credit half the
    # spread as it stood BEFORE the fill, then add whatever the mid did afterwards. This is the
    # "post-time half-spread credit" leak, and it is seductive precisely because the second term is
    # honest. The first term is a pre-entry quantity, so the whole thing earns the average spread
    # on data with no edge in it whatsoever.
    dishonest = []
    caught = 0
    dishonest_audit = TargetAudit()
    for clock, bid, ask, _spread in samples:
        pre_bid = bid.at_or_before(clock.signal_us)
        pre_ask = ask.at_or_before(clock.signal_us)
        pre_mid = (pre_ask + pre_bid) / 2
        half_spread_credit = (pre_ask - pre_bid) / 2 / pre_mid * 10000.0
        post_mid = (bid.first_at_or_after(clock.entry_us + 1_000_000) +
                    ask.first_at_or_after(clock.entry_us + 1_000_000)) / 2
        drift = (post_mid - pre_mid) / pre_mid * 10000.0
        value = half_spread_credit + drift
        dishonest.append(float(value))
        try:
            dishonest_audit.check(clock, realized=value)
        except CausalityError:
            caught += 1
    mean_dishonest = sum(dishonest) / len(dishonest)
    assert mean_dishonest > 20, f'the adversarial construction should look profitable, got {mean_dishonest:.1f} bps'
    assert caught == len(samples), f'every dishonest sample must be refused, caught {caught}'
    # And the leak is the spread, not the drift: the same data with the credit removed is flat.
    drift_only = []
    for clock, bid, ask, _spread in samples:
        pre_mid = float((ask.at_or_before(clock.signal_us) + bid.at_or_before(clock.signal_us)) / 2)
        post_mid = float((bid.first_at_or_after(clock.entry_us + 1_000_000) +
                          ask.first_at_or_after(clock.entry_us + 1_000_000)) / 2)
        drift_only.append((post_mid - pre_mid) / pre_mid * 10000.0)
    assert abs(sum(drift_only) / len(drift_only)) < 5, 'the future was supposed to be noise'

    # The honest target on the same data: pay the ask, exit at the bid, no credit for anything.
    honest = []
    honest_audit = TargetAudit()
    for clock, bid, ask, _spread in samples:
        entry = ask.first_at_or_after(clock.entry_us)
        exit_ = bid.first_at_or_after(clock.entry_us + 1_000_000)
        honest_audit.check(clock, entry_price=entry, exit_price=exit_)
        honest.append(float(markout_bps(+1, entry, exit_)))
    assert honest_audit.rejected == 0, honest_audit.summary()
    mean_honest = sum(honest) / len(honest)
    # Paying the spread on a coin flip loses money, which is the truth the audit protects.
    assert mean_honest < 0, f'the honest target on noise should not be profitable, got {mean_honest:.1f}'

    # A hidden reference three additions deep is still caught: provenance travels through
    # arithmetic, so an analyst cannot launder a pre-entry number by combining it with others.
    clock, bid, ask, _ = samples[0]
    laundered = ((bid.at_or_before(clock.signal_us) * 0.0) + 1.0) * ask.first_at_or_after(clock.entry_us)
    try:
        TargetAudit().check(clock, realized=laundered)
        raise AssertionError('a laundered pre-entry reference must still be refused')
    except CausalityError:
        pass

    # A constant with no observation behind it is fine: a fee schedule is not a leak.
    TargetAudit().check(clock, fee=0.0007, entry_price=ask.first_at_or_after(clock.entry_us))

    print(f'self-test OK: the adversarial target reports {mean_dishonest:+.1f} bps and is refused on')
    print(f'  all {len(samples)} samples; the honest one on the same data reports {mean_honest:+.1f} bps')
    print('  and passes. Provenance survives arithmetic, so a pre-entry value cannot be laundered.')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test or True:
        self_test()


if __name__ == '__main__':
    main()
