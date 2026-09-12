#!/usr/bin/env python3
"""Maker-first execution for a weekly book. Patient, bounded, and cancel-safe.

WHAT THE FIRST LIVE RUN GOT WRONG, because the rewrite is shaped by it:

  1. Each reprice posted a NEW good-till-cancelled order and never cancelled the previous one.
     Two attempts left two live orders for the same 3 contracts; six would have left six. The
     target was 3 short and the live intent was heading for 18.
  2. `filled` was initialised to zero and never read back from the exchange, so every repost was
     for the full size regardless of what had already traded.
  3. The "reprice" reposted at the SAME price, because the price was computed once before the
     loop. It was not repricing. It was duplicating.
  4. The pre-trade screen ran once, before the loop, against a position snapshot that never
     updated. It could not have caught any of the above.
  5. Legs ran sequentially with a 15-minute window each, so a six-leg book took up to 75 minutes
     and spent most of that half-built -- shorts on, longs not yet -- carrying exactly the
     directional exposure the PC1 neutralisation exists to remove.

THE SHAPE THAT FIXES ALL FIVE. One window for the whole book, not one per leg. Every leg's maker
order goes out together, so the book stays roughly neutral throughout. Each cycle: read positions
and resting orders from the exchange, cancel what is stale, recompute what is still needed,
re-screen against the live position, and repost only that. At the end, cancel everything and cross
only what still justifies it.

THE INVARIANTS THAT MAKE IT SAFE. There is never more than one live order per ticker. Remaining
size always comes from the exchange, never from a local counter. A per-ticker send budget caps
total contracts at twice the distance from flat to target. And a `finally` cancels every order
this process opened -- including on Ctrl-C, which is how the first live run ended.
"""
import argparse, datetime as dt, hashlib, json, os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import portfolio_safety as S                                          # noqa: E402


def coid_for(decision_id, ticker, attempt):
    """Deterministic client order id: same decision, same leg, same cycle -> same id.

    Never derived from a clock read at send time. If a response is lost and the process restarts,
    the retry carries the same id and the exchange rejects it as a duplicate rather than filling
    it twice.
    """
    raw = '%s|%s|%d' % (decision_id, ticker, attempt)
    return 'v1-' + hashlib.sha256(raw.encode()).hexdigest()[:24]


def live_positions(client):
    """Contracts held, from the exchange. The only source of truth for what is still needed."""
    r = client.perp_positions()
    out = {}
    for p in (r.get('positions') or r.get('market_positions') or []):
        tk = p.get('market_ticker') or p.get('ticker')
        try:
            n = float(p.get('position') or 0)
        except (TypeError, ValueError):
            continue
        if tk and abs(n) > 1e-12:
            out[tk] = n
    return out


def resting_orders(client, tickers=None):
    r = client.get('/margin/orders', status='resting', limit=200)
    out = []
    for o in (r.get('orders') or []):
        tk = o.get('ticker') or o.get('market_ticker')
        if tickers is None or tk in tickers:
            out.append(dict(o, ticker=tk))
    return out


def cancel_all(client, tickers, journal=None, why='', quiet=False):
    """Cancel every resting order on these tickers. Returns (cancelled, clean).

    `clean` is False if anything could not be cancelled or even listed. That distinction is the
    whole point: "nothing was resting" and "I could not tell you what was resting" look identical
    to a caller that only counts, and only one of them is safe to post into.
    """
    n, clean = 0, True
    try:
        for o in resting_orders(client, tickers):
            try:
                client.cancel_perp_order(o['order_id'], journal=journal, dry_run=False)
                n += 1
            except RuntimeError as e:
                clean = False
                print('  cancel FAILED %s: %s' % (o.get('order_id'), str(e)[:100]))
    except RuntimeError as e:
        clean = False
        print('  could not list resting orders: %s' % str(e)[:120])
    if n and not quiet:
        print('  cancelled %d resting order(s)%s' % (n, (' — ' + why) if why else ''))
    return n, clean


def fresh_quote(client, ticker, fallback):
    mk, stale = S.refresh_markets(client, [ticker], {ticker: dict(fallback)}, verbose=False)
    return mk[ticker], (ticker in stale)


def _send(client, **kw):
    """Place an order, or simulate one when no client exists (no key on this machine).

    A dry run without a key must still walk the same branches, or the loop stays untested while
    the planner looks fine -- the precise gap that let a duplicate-order bug reach production.
    """
    if client is None:
        return {'simulated': True, 'reason': 'no signing key in this environment',
                'order': {k: v for k, v in kw.items() if k != 'journal'}}
    return client.create_perp_order(**kw)


def plan_from_decision(decision, snap, cfg):
    """Target contracts per ticker, plus the exits the book no longer wants."""
    markets = {m['ticker']: m for m in snap['markets']}
    positions = {p['ticker']: float(p['position']) for p in snap.get('positions', [])}
    keep = set(decision.get('legs_to_trade', []))
    plan = []
    for row in decision.get('integer_book', []):
        tk, sym = row['kalshi'], row['symbol']
        if not tk or tk not in markets:
            continue
        want = int(row['contracts'])
        have = int(round(positions.get(tk, 0.0)))
        if want == have:
            continue
        if sym not in keep:
            plan.append({'ticker': tk, 'symbol': sym, 'skip': True, 'want': want,
                         'reason': 'no-trade test: benefit did not clear cost x required ratio'})
            continue
        plan.append({'ticker': tk, 'symbol': sym, 'skip': False, 'want': want,
                     'tick': float(markets[tk].get('tick') or 0.0001), 'is_exit': False})
    booked = {r['kalshi'] for r in decision.get('integer_book', []) if r['kalshi']}
    for tk, n in positions.items():
        if tk in booked or abs(n) < 1e-9 or tk not in markets:
            continue
        plan.append({'ticker': tk, 'symbol': '(not in book)', 'skip': False, 'want': 0,
                     'tick': float(markets[tk].get('tick') or 0.0001), 'is_exit': True})
    return plan


def still_worth_crossing(leg, decision, cfg):
    """Re-score at the moment the maker window expires: is the worse price still worth it?"""
    ow = cfg['cost_assumptions']['oneway_bps']
    dflt = cfg['cost_assumptions']['default_oneway_bps']
    buf = float(cfg['execution']['safety_buffer_bps'])
    if leg.get('is_exit'):
        return True, 'exit to flat: risk reduction is not benefit-gated'
    row = next((r for r in decision.get('no_trade_test', []) if r['symbol'] == leg['symbol']),
               None)
    if row is None:
        return False, 'no benefit estimate for this leg'
    benefit = float(row['expected_benefit_bps'])
    cost = abs(float(row['delta_w'])) * float(ow.get(leg['symbol'], dflt)) + buf
    return benefit > cost, 'benefit %.3f bps vs taker cost + buffer %.3f bps' % (benefit, cost)


def _screen_ctx(markets, pos, plan, snap, cfg):
    return {'markets': markets, 'positions': pos,
            'targets': {x['ticker']: x['want'] for x in plan},
            'equity': float(snap['account']['equity'] or 0),
            'limits': cfg['canary_risk_limits']}


def execute(plan, decision, snap, cfg, client, journal=None, live=False, quick=False):
    """One maker window for the whole book, then one cross decision per leg."""
    markets = {m['ticker']: m for m in snap['markets']}
    start_pos = {p['ticker']: float(p['position']) for p in snap.get('positions', [])}
    ex = cfg['execution']
    window = 6 if quick else int(ex['maker_window_seconds'])
    interval = 1 if quick else int(ex['reprice_interval_seconds'])
    max_cycles = int(ex['max_reprices'])
    did = decision['decided_at_utc']
    work = [l for l in plan if not l.get('skip')]
    tickers = [l['ticker'] for l in work]
    log = {l['ticker']: dict(l, sent=0, outcome='untouched') for l in plan}
    # Cumulative sends grow with every legitimate reprice, because each repost follows a cancel.
    # So the cap has to allow one post per cycle plus the cross; it is a runaway backstop, not a
    # position limit. The position limit is the invariant checked after every cycle below.
    budget = {l['ticker']: max(2, abs(l['want'] - int(round(start_pos.get(l['ticker'], 0))))
                               * (max_cycles + 2)) for l in work}

    for l in plan:
        if l.get('skip'):
            print('  %-13s SKIP   %s' % (l['ticker'], l['reason']))
            log[l['ticker']]['outcome'] = 'skipped'

    def positions_now():
        return live_positions(client) if (live and client is not None) else dict(start_pos)

    try:
        deadline = time.time() + window
        cycle = 0
        while cycle <= max_cycles and time.time() < deadline and work:
            pos = positions_now()
            # INVARIANT: no leg may ever hold more than it was asked to hold. If one does, a
            # duplicate filled somewhere and nothing more should be sent until a human looks.
            over = [(tk, pos.get(tk, 0), l['want']) for l in work
                    for tk in [l['ticker']]
                    if abs(pos.get(tk, 0)) > abs(l['want']) + 0.5]
            if over:
                for tk, have, want in over:
                    print('  %-13s OVERFILLED  holding %+g against a target of %+d'
                          % (tk, have, want))
                print('  halting: cancelling everything and leaving the book as it stands')
                break
            if live:
                _, clean = cancel_all(client, tickers, journal, quiet=True)
                if not clean:
                    print('  could not confirm the book is clear of resting orders; '
                          'skipping this cycle rather than posting on top of one')
                    cycle += 1
                    time.sleep(min(interval, max(0, deadline - time.time())))
                    continue
            still = []
            for l in work:
                tk = l['ticker']
                have = int(round(pos.get(tk, 0)))
                need = l['want'] - have
                if need == 0:
                    print('  %-13s DONE   at target (%+d)' % (tk, l['want']))
                    log[tk]['outcome'] = 'filled'
                    continue
                m, stale = (fresh_quote(client, tk, markets.get(tk, {}))
                            if client is not None else (markets.get(tk, {}), False))
                if stale or m.get('bid') is None:
                    print('  %-13s HOLD   no usable quote this cycle' % tk)
                    still.append(l)
                    continue
                markets[tk] = m
                side = 'bid' if need > 0 else 'ask'
                tick = float(m.get('tick') or l['tick'])
                px = float(m['bid']) if side == 'bid' else float(m['ask'])
                px = round(round(px / tick) * tick, 6)
                reduce_only = bool(l.get('is_exit')) or (
                    have != 0 and abs(l['want']) < abs(have) and l['want'] * have >= 0)
                order = {'ticker': tk, 'side': side, 'count': abs(need), 'price': px,
                         'reduce_only': reduce_only}
                fails = S.pre_trade_screen(order, _screen_ctx(markets, pos, plan, snap, cfg))
                if fails:
                    print('  %-13s REJECT %s' % (tk, '; '.join(fails)))
                    log[tk]['outcome'] = 'rejected'
                    continue
                if log[tk]['sent'] + abs(need) > budget[tk]:
                    print('  %-13s STOP   send budget exhausted (%d sent, cap %d)'
                          % (tk, log[tk]['sent'], budget[tk]))
                    log[tk]['outcome'] = 'budget_stop'
                    continue
                r = _send(client, ticker=tk, side=side, count=abs(need), price=px,
                          tick=tick, post_only=True, reduce_only=reduce_only,
                          tif='good_till_canceled',
                          client_order_id=coid_for(did, tk, cycle),
                          journal=journal, dry_run=not live)
                if live and not r.get('simulated'):
                    log[tk]['sent'] += abs(need)
                print('  %-13s MAKER  %+d @ $%.4f  cycle %d  -> %s'
                      % (tk, need, px, cycle,
                         'resting' if (live and not r.get('simulated'))
                         else 'not sent (%s)' % r.get('reason', 'dry run')))
                still.append(l)
            work = still
            if not work:
                break
            cycle += 1
            time.sleep(min(interval, max(0, deadline - time.time())))

        print('\n  maker window closed; cancelling and deciding on the remainder')
        if live:
            _, clean = cancel_all(client, tickers, journal, why='maker window closed')
            if not clean:
                print('  resting orders could not be confirmed cancelled; NOT crossing on top '
                      'of them')
                work = []
        pos = positions_now()
        for l in work:
            tk = l['ticker']
            have = int(round(pos.get(tk, 0)))
            need = l['want'] - have
            if need == 0:
                log[tk]['outcome'] = 'filled'
                print('  %-13s DONE   at target (%+d)' % (tk, l['want']))
                continue
            ok, why = still_worth_crossing(l, decision, cfg)
            if not ok:
                log[tk]['outcome'] = 'skipped_at_cross'
                log[tk]['reason'] = why
                print('  %-13s SKIP at cross: %s' % (tk, why))
                continue
            m, stale = (fresh_quote(client, tk, markets.get(tk, {}))
                        if client is not None else (markets.get(tk, {}), False))
            if stale or m.get('bid') is None:
                print('  %-13s SKIP at cross: no usable quote' % tk)
                log[tk]['outcome'] = 'no_quote'
                continue
            markets[tk] = m
            side = 'bid' if need > 0 else 'ask'
            tick = float(m.get('tick') or l['tick'])
            px = float(m['ask']) if side == 'bid' else float(m['bid'])
            px = round(round(px / tick) * tick, 6)
            order = {'ticker': tk, 'side': side, 'count': abs(need), 'price': px,
                     'reduce_only': bool(l.get('is_exit'))}
            fails = S.pre_trade_screen(order, _screen_ctx(markets, pos, plan, snap, cfg))
            if fails:
                print('  %-13s REJECT at cross: %s' % (tk, '; '.join(fails)))
                log[tk]['outcome'] = 'rejected_at_cross'
                continue
            if log[tk]['sent'] + abs(need) > budget[tk]:
                print('  %-13s STOP   send budget exhausted' % tk)
                log[tk]['outcome'] = 'budget_stop'
                continue
            _send(client, ticker=tk, side=side, count=abs(need), price=px, tick=tick,
                  post_only=False, reduce_only=order['reduce_only'],
                  tif='immediate_or_cancel', client_order_id=coid_for(did, tk, 99),
                  journal=journal, dry_run=not live)
            if live:
                log[tk]['sent'] += abs(need)
            log[tk]['outcome'] = 'crossed'
            print('  %-13s CROSS  %+d @ $%.4f  (%s)' % (tk, need, px, why))
    finally:
        # Ctrl-C ended the first live run and left two orders resting. Whatever happens above,
        # nothing this process opened is left on the book.
        if live and client is not None:
            print('\n  final sweep')
            n, clean = cancel_all(client, tickers, journal, why='process exiting')
            if not clean:
                print('  WARNING: could not confirm every order was cancelled. Check manually:')
                print('  python3 scripts/kalshi_client.py /margin/orders status=resting')

    return list(log.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decision', default=os.path.join(ROOT, 'reports', 'decisions', 'latest.json'))
    ap.add_argument('--snapshot', default=os.path.join(ROOT, 'data', 'snapshots',
                                                       'kalshi_perp_snapshot.json'))
    ap.add_argument('--live', action='store_true')
    ap.add_argument('--quick', action='store_true', help='short window, for testing')
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    want_hash = open(os.path.join(ROOT, 'config', 'portfolio_v1.sha256')).read().strip()
    decision = json.load(open(args.decision))
    snap = json.load(open(args.snapshot))
    if decision.get('config_hash') != want_hash:
        raise SystemExit('decision was made under a different config hash. Refusing to execute.')

    blocks, warns = S.kill_switches(decision, snap['account'], snap.get('positions', []), cfg,
                                    expected_hash=want_hash)
    print('EXECUTE  %s   mode=%s   label=%s'
          % (dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
             decision['mode'], decision['pnl_label']))

    client = journal = None
    try:
        from kalshi_client import Kalshi, Journal
        client, journal = Kalshi(), Journal()
        host_ok, tok_ok = client.armed()
        print('arm state: host_ok=%s token_ok=%s live_flag=%s -> %s'
              % (host_ok, tok_ok, args.live,
                 'WILL SEND' if (host_ok and tok_ok and args.live) else 'WILL NOT SEND'))
    except SystemExit as e:
        print('no signing key reachable here (%s) — dry run only' % e)
        if args.live:
            raise SystemExit('cannot trade without the signing key')

    if client is not None and journal is not None:
        pending = journal.unresolved()
        if pending:
            print('\nRECONCILING %d unresolved intent(s) before planning' % len(pending))
            for r in S.reconcile(client, journal):
                print('  %s -> %s' % (r['client_order_id'][:20], r['resolved_state']))

    plan = plan_from_decision(decision, snap, cfg)
    blocks = blocks + S.promote_unhandled_strays(warns, plan)
    for w in warns:
        closed = any(l.get('is_exit') and l['ticker'] == w['ticker'] for l in plan)
        print('  %s  %s' % ('NOTE ' if closed else 'BLOCK',
                            w['text'] + (' — this plan closes it' if closed else '')))
    if blocks:
        print('\nBLOCKED — new risk will not be opened:')
        for b in blocks:
            print('  %s' % b)
        if args.live:
            raise SystemExit('refusing to trade with kill switches active')

    print('\n%d leg(s) in the plan; ONE maker window of %ds for the whole book'
          % (len(plan), int(cfg['execution']['maker_window_seconds'])))
    log = execute(plan, decision, snap, cfg, client=client, journal=journal,
                  live=args.live, quick=args.quick)

    out = os.path.join(ROOT, 'reports', 'decisions',
                       'execution_%s.json' % decision['decided_at_utc'].replace(':', ''))
    with open(out, 'w') as f:
        json.dump({'decision_id': decision['decided_at_utc'], 'live': bool(args.live),
                   'log': log}, f, indent=1)
    print('\nwrote %s' % out)

    if args.live and client is not None:
        pos = live_positions(client)
        print('\nfinal positions, read from the exchange:')
        for tk, n in sorted(pos.items()):
            print('  %-13s %+g' % (tk, n))
        if not pos:
            print('  (flat)')
        print('resting orders left on the book: %d' % len(resting_orders(client)))


if __name__ == '__main__':
    sys.exit(main())
