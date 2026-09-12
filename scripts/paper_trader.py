#!/usr/bin/env python3
"""Phase 5 -- live paper trader.

It runs the SAME fill machinery as the execution simulator, on a live feed instead of a file.
That is the whole design. A paper trader with its own fill logic is the standard way to end up
with a backtest and a paper result that disagree for a reason nobody can locate, and then to
trust whichever one is more flattering. Here `Order`, `walk_book` and the two queue bounds are
imported from `execution_sim`, so a difference between backtest and paper can only come from the
market, never from the code.

What paper trading adds over the simulator is the things a replay cannot have:

  * Real latency. The simulator places an order at the timestamp of the event that triggered it.
    Here the decision is made after the frame arrives, the order is timestamped when it would
    have been sent, and the gap is recorded on every order.
  * Real intraday variation. A backtest over three hours is three hours of one session; paper
    runs across sessions, weekends and news.
  * Real failure. Sockets drop, sequences gap, the clock drifts. Each of those is a thing that
    happens to a live strategy and cannot happen to a file.

It is still not trading. No order is sent, nothing is authenticated, and no key is read. Paper
results are an upper bound on live results, because the orders here never move the book.

GATE: this refuses to run a strategy that has not produced a MEASURED positive net result. The
scoreboard's `PROMISING` status is not enough -- promising means untested. Run with
`--strategy baseline` to paper-trade the do-nothing benchmark, which is always allowed and is the
thing every strategy has to beat.

  python3 scripts/paper_trader.py --strategy baseline --markets KXBTCPERP --minutes 60
  python3 scripts/paper_trader.py --list
"""
import argparse, csv, json, os, signal, sys, time
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from execution_sim import Sim, Order, walk_book, MAKER_BPS, TAKER_BPS, MARKOUTS_US, fmt
from feature_engine import num, check_epoch

SCOREBOARD = os.path.join(ROOT, 'reports', 'model_scoreboard.csv')
JOURNAL = os.path.join(ROOT, 'reports', 'paper_journal.jsonl')
SUMMARY = os.path.join(ROOT, 'reports', 'paper_summary.md')


def measured_candidates():
    """Strategies with a measured positive net result. Not the same as 'promising'.

    `PROMISING` on the scoreboard means a screen liked it and nothing has priced it. A strategy
    reaches paper only once something measured what it costs to trade and the answer was still
    positive -- which is exactly the gate the earlier minute-bar work would have failed.
    """
    if not os.path.exists(SCOREBOARD):
        return [], 'no scoreboard'
    out = []
    with open(SCOREBOARD) as f:
        for r in csv.DictReader(f):
            try:
                net = float(r.get('net_bps') or '')
            except (TypeError, ValueError):
                continue
            trades = (r.get('trades') or '').strip()
            if net > 0 and trades not in ('', '0') and \
                    (r.get('status') or '') in ('PROMISING', 'LIVE_CANDIDATE'):
                out.append(r)
    return out, f'{len(out)} measured positive'


class Baseline:
    """The do-nothing benchmark: quote nothing, trade nothing, hold nothing.

    It exists because it is the thing every strategy must beat, and because running it end to end
    proves the harness works without any strategy being involved. Its P&L is exactly zero and any
    deviation from zero in the journal is a bug in the harness, not a discovery.
    """
    name = 'baseline'
    needs_gate = False

    def on_book(self, market, book, now_us):
        return []


class JoinTheTouch:
    """Rest at the touch on both sides, cancel on timeout. A market maker with no opinion.

    Not a proposal. It is the null model for the FAIR-MAKER thesis: if quoting with no signal at
    all is profitable on some market, the spread is paying for itself and the signal is a bonus.
    The maker viability screen says 6 of 20 contracts have a median quoted gap above the 10 bps
    maker/maker round trip, and that spread is widest exactly where flow is thinnest
    (correlation -0.64 between log notional and gap), so the expected answer is no.
    """
    name = 'join_the_touch'
    needs_gate = True

    def __init__(self, notional=25.0, timeout_s=30.0, min_spread_bps=12.0):
        self.notional, self.timeout_us = notional, int(timeout_s * 1e6)
        self.min_spread_bps = min_spread_bps

    def on_book(self, market, book, now_us):
        if not book.usable():
            return []
        sp = book.spread_bps
        if sp is None or sp < self.min_spread_bps:
            return []
        mid = book.mid
        size = self.notional / mid
        out = []
        for side, px in (('buy', book.best_bid), ('sell', book.best_ask)):
            depth = (book.bids if side == 'buy' else book.asks).get(px, 0.0)
            if depth <= 0 or size > 0.25 * depth:
                continue
            out.append((side, px, size, depth))
        return out


STRATEGIES = {s.name: s for s in (Baseline, JoinTheTouch)}


class PaperTrader:
    def __init__(self, strategy, markets, notional, timeout_s):
        self.strat = strategy
        self.sim = Sim(notional, 1e12, timeout_s, 0.25, markets)   # sampling driven by strategy
        self.markets = set(markets) if markets else None
        self.journal = []
        self.decisions = 0
        self.placed = 0
        self.latency_us = []
        self.started = time.time()
        self.errors = defaultdict(int)

    def on_event(self, src, ev, arrived_us):
        """Feed one live frame. `arrived_us` is when WE saw it, which is what decides latency."""
        try:
            self.sim.feed(src, ev)
        except Exception as e:                      # a live feed must not be killed by one frame
            self.errors[type(e).__name__] += 1
            return
        m = (ev.get('msg') or {})
        t = m.get('market_ticker')
        if not t or (self.markets and t not in self.markets):
            return
        bk = self.sim.eng.books.get(t)
        if bk is None or bk.stale:
            return
        now_us = check_epoch(ev.get('src_ts_us'), 'src') or arrived_us
        orders = self.strat.on_book(t, bk, now_us)
        if not orders:
            return
        self.decisions += 1
        # A decision made now would be SENT now, not at the timestamp of the frame that caused
        # it. Recording the difference is the only way paper latency is ever visible.
        send_us = int(time.time() * 1e6)
        lat = send_us - (arrived_us or send_us)
        self.latency_us.append(lat)
        for side, px, size, depth in orders:
            if any(o.side == side and abs(o.price - px) < 1e-9 for o in self.sim.open[t]):
                continue                            # already quoting there
            o = Order(self.strat.name, t, side, px, size, now_us, bk.mid, depth,
                      self.sim.timeout_us, bk.spread_bps)
            self.sim.open[t].append(o)
            self.placed += 1
            self.journal.append({'ts_us': now_us, 'sent_us': send_us, 'latency_us': lat,
                                 'market': t, 'side': side, 'price': px, 'size': size,
                                 'queue_ahead': depth, 'mid': bk.mid,
                                 'spread_bps': bk.spread_bps, 'event': 'place'})

    def finish(self):
        for t, orders in self.sim.open.items():
            for o in orders:
                if o.fill_opt_us is not None:
                    self.sim.filled.append(o)
                else:
                    self.sim.expired[(t, o.mode)] += 1
        self.sim.open.clear()
        rows = []
        for o in self.sim.filled:
            for b in ('pess', 'opt'):
                r = self.sim.measure(o, b)
                if r:
                    rows.append(r)
                    if b == 'pess':
                        self.journal.append({'ts_us': r['fill_us'], 'market': r['market'],
                                             'side': r['side'], 'event': 'fill',
                                             'cost_bps': r['cost_bps'], 'fee_bps': r['fee_bps'],
                                             'markout_60s': r.get('markout_60s')})
        return rows


def write_summary(pt, rows, path, args):
    from execution_sim import aggregate
    lines = aggregate(pt.sim, rows)
    pess = [L for L in lines if L['queue_bound'] == 'pess']
    mins = (time.time() - pt.started) / 60.0
    out = []
    W = out.append
    W(f'# Paper trading -- `{pt.strat.name}`\n')
    W(f'Ran {mins:.1f} minutes on {", ".join(sorted(args.markets or ["all"]))}.  ')
    W(f'Decisions {pt.decisions:,}, orders placed {pt.placed:,}, fills {len(pt.sim.filled):,}\n')

    if pt.latency_us:
        lat = sorted(pt.latency_us)
        n = len(lat)
        W('## Decision latency\n')
        W(f'From frame arrival to the moment an order would have been sent. The backtest has no '
          f'equivalent: it places orders at the timestamp of the frame that triggered them, '
          f'which is free.\n')
        W(f'- median {lat[n//2]/1000:.1f} ms, p95 {lat[int(.95*n)]/1000:.1f} ms, '
          f'max {lat[-1]/1000:.1f} ms\n')

    if not pess:
        W('## Result\n')
        W('No fills. With nothing filled there is no P&L to report and no result to compare '
          'against the backtest -- which is itself the finding if the strategy was supposed to '
          'be quoting.\n')
    else:
        W('## Result, pessimistic queue bound\n')
        W('| market | attempts | fills | fill% | entry bps | fee | 60s markout | net 60s |')
        W('|---|---:|---:|---:|---:|---:|---:|---:|')
        for L in sorted(pess, key=lambda x: x['market']):
            W(f"| {L['market']} | {L['attempts']} | {L['fills']} | "
              f"{fmt(L['fill_rate_pct'],1)} | {fmt(L['entry_cost_bps'])} | "
              f"{fmt(L['fee_bps'])} | {fmt(L['markout_60s_bps'])} | "
              f"**{fmt(L['net_60s_bps'])}** |")
        W('')
        tot = [L['net_60s_bps'] for L in pess if L['net_60s_bps'] is not None]
        if tot:
            W(f'Mean net 60s across markets: **{sum(tot)/len(tot):+.2f} bps**\n')

    if pt.errors:
        W('## Frames that raised\n')
        for k, v in sorted(pt.errors.items(), key=lambda kv: -kv[1]):
            W(f'- `{k}`: {v:,}')
        W('\nA live feed must survive a bad frame. These were counted and skipped rather than '
          'allowed to end the session.\n')

    W('## What paper trading still does not prove\n')
    W('- The orders here were never in the book, so nobody queued behind them and nobody stepped '
      'in front. Fill rates are an upper bound.')
    W('- No order was sent, so nothing was rejected, re-priced, partially filled across a '
      'reconnect, or subject to a margin check.')
    W('- Markouts are measured against the mid. The exit is a second spread and is not in these '
      'numbers.\n')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return lines


def replay_source(hours, markets):
    """Replay recorded hours as if live. Used when no socket is available, and for testing.

    It is explicitly NOT called paper trading in the report: replaying a file measures the same
    thing the execution simulator already measured. What it is for is proving the harness runs.
    """
    from execution_sim import iter_events
    for src, ev in iter_events(hours):
        yield src, ev, int(time.time() * 1e6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strategy', default='baseline', choices=sorted(STRATEGIES))
    ap.add_argument('--markets', nargs='*', default=['KXBTCPERP'])
    ap.add_argument('--notional', type=float, default=25.0)
    ap.add_argument('--timeout-s', type=float, default=30.0)
    ap.add_argument('--minutes', type=float, default=60.0)
    ap.add_argument('--replay', nargs='*', help='replay these recorded hours instead of a socket')
    ap.add_argument('--list', action='store_true', help='show which strategies the gate allows')
    ap.add_argument('--journal', default=JOURNAL)
    ap.add_argument('--md', default=SUMMARY)
    args = ap.parse_args()

    cands, why = measured_candidates()
    if args.list:
        print(f'gate: {why}')
        for name, cls in sorted(STRATEGIES.items()):
            allowed = (not cls.needs_gate) or bool(cands)
            print(f'  {name:16s} {"ALLOWED" if allowed else "LOCKED"}'
                  f'{"" if allowed else "  needs a measured positive net result"}')
        for r in cands:
            print(f'  measured: {r.get("market")} {r.get("model")} net {r.get("net_bps")} bps')
        return 0

    cls = STRATEGIES[args.strategy]
    if cls.needs_gate and not cands:
        print(f'LOCKED: `{args.strategy}` needs a measured positive net result and there are '
              f'none ({why}).')
        print('Nothing on the scoreboard has been priced through the execution simulator, so '
              'there is nothing to paper-trade yet.')
        print('Run `--strategy baseline` to exercise the harness, or run the execution simulator '
              'against a strategy first.')
        return 2

    strat = cls() if cls is not Baseline else Baseline()
    pt = PaperTrader(strat, args.markets, args.notional, args.timeout_s)

    stop = {'now': False}
    def _sig(*_):
        stop['now'] = True
        print('\nstopping, writing what has been collected so far', file=sys.stderr)
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    deadline = time.time() + args.minutes * 60
    if args.replay:
        for src, ev, arrived in replay_source(args.replay, args.markets):
            pt.on_event(src, ev, arrived)
            if stop['now']:
                break
    else:
        print('No live socket wired into this build yet, so there is nothing to paper-trade '
              'against. Pass --replay <hour> to exercise the harness on recorded data.',
              file=sys.stderr)
        print('Wiring it up means pointing perps_recorder.py at this process rather than a file; '
              'the fill machinery is already shared with the execution simulator.',
              file=sys.stderr)
        return 3

    rows = pt.finish()
    os.makedirs(os.path.dirname(args.journal), exist_ok=True)
    with open(args.journal, 'w') as f:
        for j in pt.journal:
            f.write(json.dumps(j) + '\n')
    write_summary(pt, rows, args.md, args)
    print(f'decisions {pt.decisions:,}  placed {pt.placed:,}  fills {len(pt.sim.filled):,}')
    print(f'wrote {os.path.relpath(args.md, ROOT)} and '
          f'{os.path.relpath(args.journal, ROOT)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
