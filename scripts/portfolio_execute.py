#!/usr/bin/env python3
"""Maker-first execution for a weekly book. Patient by construction, armed only deliberately.

THE SIGNAL HORIZON IS A WEEK, SO THERE IS NO REASON TO PAY THE SPREAD IN THE FIRST SECOND.
Twenty-four of the twenty-nine basis points in a Kalshi round trip are exchange fee, and the
maker fee is less than half the taker fee. On a weekly signal the correct default is to rest at
the touch, reprice as the book moves, and only cross if the remaining edge still justifies it.

CROSSING IS A DECISION, NOT A TIMEOUT. When the maker window expires the leg is re-scored: the
benefit that remains must still exceed the all-in taker cost plus the safety buffer. If it does
not, the leg is skipped and logged. A rebalance that does not happen is a valid outcome and the
ledger records it as such -- silently crossing at the end of a timer is how a strategy's measured
costs drift away from its modelled ones.

NOTHING HERE SENDS UNLESS THREE THINGS HOLD: the right machine, the arm token, and an explicit
--live flag. Default is a dry run that exercises every code path and writes every journal row.
"""
import argparse, datetime as dt, hashlib, json, os, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import portfolio_safety as S                                          # noqa: E402


def coid_for(decision_id, ticker, attempt):
    """Deterministic client order id.

    Derived from the decision and the leg, never from a clock read at send time: if a response is
    lost and the process restarts, the retry must carry the SAME id so the exchange can reject it
    as a duplicate instead of filling it twice.
    """
    raw = '%s|%s|%d' % (decision_id, ticker, attempt)
    return 'v1-' + hashlib.sha256(raw.encode()).hexdigest()[:24]


def plan_from_decision(decision, snap, cfg):
    """Legs to move, as orders, honouring the no-trade test the decision already ran."""
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
        delta = want - have
        if delta == 0:
            continue
        if sym not in keep:
            plan.append({'ticker': tk, 'symbol': sym, 'skip': True,
                         'reason': 'no-trade test: benefit did not clear cost x required ratio',
                         'delta': delta})
            continue
        m = markets[tk]
        tick = float(m.get('tick') or 0.0001)
        side = 'bid' if delta > 0 else 'ask'
        # Maker joins the near side; taker crosses to the far side. Prices are DOLLARS snapped to
        # the market tick, never cents -- a cent grid would concede up to 22 bps on a $2.25
        # contract, which is most of a round trip.
        snap = lambda x: round(round(x / tick) * tick, 6)
        maker_px = snap(float(m['bid']) if side == 'bid' else float(m['ask']))
        taker_px = snap(float(m['ask']) if side == 'bid' else float(m['bid']))
        # Shrinking an existing position is reduce-only: it can never flip through zero.
        reduce_only = (have != 0 and abs(want) < abs(have) and have * want >= 0)
        plan.append({'ticker': tk, 'symbol': sym, 'skip': False, 'side': side,
                     'count': abs(delta), 'maker_price': maker_px, 'taker_price': taker_px,
                     'tick': tick, 'have': have, 'want': want, 'delta': delta,
                     'reduce_only': reduce_only, 'is_exit': False})
    # Positions held that the book no longer wants at all.
    booked = {r['kalshi'] for r in decision.get('integer_book', []) if r['kalshi']}
    for tk, n in positions.items():
        if tk in booked or abs(n) < 1e-9 or tk not in markets:
            continue
        m = markets[tk]
        tick = float(m.get('tick') or 0.0001)
        snap = lambda x: round(round(x / tick) * tick, 6)
        side = 'ask' if n > 0 else 'bid'
        plan.append({'ticker': tk, 'symbol': '(not in book)', 'skip': False, 'side': side,
                     'count': int(abs(round(n))),
                     'maker_price': snap(float(m['ask']) if n > 0 else float(m['bid'])),
                     'taker_price': snap(float(m['bid']) if n > 0 else float(m['ask'])),
                     'tick': tick, 'have': int(round(n)), 'want': 0,
                     'delta': -int(round(n)), 'reduce_only': True, 'is_exit': True})
    return plan


def still_worth_crossing(leg, decision, cfg):
    """Re-score a leg at the moment of the maker window expiring.

    The taker cost is the full one-way for that coin; the buffer is the frozen safety_buffer_bps.
    This is the same arithmetic as the no-trade test, re-run with the taker cost instead of the
    modelled one, because the question at this point is different: not "is this move worth making"
    but "is it still worth making at the worse price".
    """
    ow = cfg['cost_assumptions']['oneway_bps']
    dflt = cfg['cost_assumptions']['default_oneway_bps']
    buf = float(cfg['execution']['safety_buffer_bps'])
    if leg.get('is_exit'):
        # Closing a position the book no longer wants is risk reduction, not an alpha trade.
        # Gating it on expected benefit would leave stray risk on the book indefinitely, which is
        # exactly what the kill switch complains about. It always crosses.
        return True, 'exit to flat: risk reduction is not benefit-gated'
    row = next((r for r in decision.get('no_trade_test', []) if r['symbol'] == leg['symbol']),
               None)
    if row is None:
        return False, 'no benefit estimate for this leg'
    benefit = float(row['expected_benefit_bps'])
    cost = abs(float(row['delta_w'])) * float(ow.get(leg['symbol'], dflt)) + buf
    return benefit > cost, 'benefit %.3f bps vs taker cost + buffer %.3f bps' % (benefit, cost)


def execute(plan, decision, snap, cfg, client=None, journal=None, live=False, quick=False):
    markets = {m['ticker']: m for m in snap['markets']}
    positions = {p['ticker']: float(p['position']) for p in snap.get('positions', [])}
    ctx = {'markets': markets, 'positions': dict(positions),
           'targets': {r['kalshi']: int(r['contracts'])
                       for r in decision.get('integer_book', []) if r['kalshi']},
           'equity': float(snap['account']['equity']),
           'limits': cfg['canary_risk_limits']}
    ex = cfg['execution']
    window = 1 if quick else int(ex['maker_window_seconds'])
    interval = 1 if quick else int(ex['reprice_interval_seconds'])
    max_reprice = int(ex['max_reprices'])
    did = decision['decided_at_utc']
    log = []

    for leg in plan:
        if leg.get('skip'):
            log.append(dict(leg, outcome='skipped', maker_or_taker='none'))
            print('  %-13s SKIP   %s' % (leg['ticker'], leg['reason']))
            continue

        order = {'ticker': leg['ticker'], 'side': leg['side'], 'count': leg['count'],
                 'price': leg['maker_price'], 'reduce_only': leg.get('reduce_only', False)}
        fails = S.pre_trade_screen(order, ctx)
        if fails:
            log.append(dict(leg, outcome='rejected', reason='; '.join(fails)))
            print('  %-13s REJECT %s' % (leg['ticker'], '; '.join(fails)))
            continue

        filled = 0
        attempt = 0
        deadline = time.time() + window
        while attempt <= max_reprice and time.time() < deadline and filled < leg['count']:
            coid = coid_for(did, leg['ticker'], attempt)
            if client is not None:
                # post_only is what makes the maker phase a maker phase: a bare limit at the
                # touch can cross and quietly pay the taker fee.
                r = client.create_perp_order(ticker=leg['ticker'], side=leg['side'],
                                             count=leg['count'] - filled,
                                             price=order['price'], tick=leg['tick'],
                                             post_only=True,
                                             reduce_only=leg.get('reduce_only', False),
                                             tif='good_till_canceled',
                                             client_order_id=coid, journal=journal,
                                             dry_run=not live)
            else:
                r = {'simulated': True, 'reason': 'no client in this environment'}
            print('  %-13s MAKER  %d @ $%.4f  attempt %d  -> %s'
                  % (leg['ticker'], leg['count'] - filled, order['price'], attempt,
                     'sent' if live and not r.get('simulated') else 'not sent (%s)'
                     % r.get('reason', 'dry run')))
            attempt += 1
            if quick:
                break
            time.sleep(min(interval, max(0, deadline - time.time())))

        if filled < leg['count']:
            ok, why = still_worth_crossing(leg, decision, cfg)
            if ok:
                order['price'] = leg['taker_price']
                fails = S.pre_trade_screen(order, ctx)
                if fails:
                    log.append(dict(leg, outcome='rejected_at_cross', reason='; '.join(fails)))
                    print('  %-13s REJECT at cross: %s' % (leg['ticker'], '; '.join(fails)))
                    continue
                coid = coid_for(did, leg['ticker'], 99)
                if client is not None:
                    client.create_perp_order(ticker=leg['ticker'], side=leg['side'],
                                             count=leg['count'] - filled, price=order['price'],
                                             tick=leg['tick'], post_only=False,
                                             reduce_only=leg.get('reduce_only', False),
                                             tif='immediate_or_cancel',
                                             client_order_id=coid, journal=journal,
                                             dry_run=not live)
                log.append(dict(leg, outcome='crossed', maker_or_taker='taker', reason=why))
                print('  %-13s CROSS  %d @ $%.4f  (%s)'
                      % (leg['ticker'], leg['count'] - filled, order['price'], why))
            else:
                log.append(dict(leg, outcome='skipped_at_cross', maker_or_taker='none',
                                reason=why))
                print('  %-13s SKIP at cross: %s' % (leg['ticker'], why))
        else:
            log.append(dict(leg, outcome='filled', maker_or_taker='maker'))
    return log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decision', default=os.path.join(ROOT, 'reports', 'decisions', 'latest.json'))
    ap.add_argument('--snapshot', default=os.path.join(ROOT, 'data', 'snapshots',
                                                       'kalshi_perp_snapshot.json'))
    ap.add_argument('--live', action='store_true',
                    help='actually send. Also needs the right machine and KALSHI_ARM set.')
    ap.add_argument('--quick', action='store_true', help='collapse the maker window, for testing')
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    want_hash = open(os.path.join(ROOT, 'config', 'portfolio_v1.sha256')).read().strip()
    decision = json.load(open(args.decision))
    snap = json.load(open(args.snapshot))

    if decision.get('config_hash') != want_hash:
        raise SystemExit('decision was made under a different config hash. Refusing to execute.')

    blocks = S.kill_switches(decision, snap['account'], snap.get('positions', []), cfg,
                             expected_hash=want_hash)
    print('EXECUTE  %s   mode=%s   label=%s'
          % (dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
             decision['mode'], decision['pnl_label']))
    if blocks:
        print('\nBLOCKED — new risk will not be opened:')
        for b in blocks:
            print('  %s' % b)
        if args.live:
            raise SystemExit('refusing to trade with kill switches active')

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

    plan = plan_from_decision(decision, snap, cfg)
    print('\n%d leg(s) in the plan' % len(plan))
    log = execute(plan, decision, snap, cfg, client=client, journal=journal,
                  live=args.live, quick=args.quick)

    out = os.path.join(ROOT, 'reports', 'decisions',
                       'execution_%s.json' % decision['decided_at_utc'].replace(':', ''))
    with open(out, 'w') as f:
        json.dump({'decision_id': decision['decided_at_utc'], 'live': bool(args.live),
                   'log': log}, f, indent=1)
    print('\nwrote %s' % out)


if __name__ == '__main__':
    sys.exit(main())
