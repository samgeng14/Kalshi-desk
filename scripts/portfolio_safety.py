#!/usr/bin/env python3
"""Reconciliation, kill switches, loss guards and emergency flatten.

THIS FILE EXISTS TO BE RUN BEFORE THE FIRST ORDER, NOT AFTER THE FIRST PROBLEM. The spec requires
emergency flatten to exist and be tested before anything goes live, so `--test-flatten` exercises
the entire path -- position read, order construction, pre-trade screen, journal write -- with the
arm disabled, and prints exactly what would have been sent.

RECONCILIATION TREATS A LOST RESPONSE AS UNKNOWN, NEVER AS FAILED. If an intent was written and
no acknowledgement came back, the exchange is the authority: we query orders and fills for that
client_order_id before deciding anything. Re-sending because an HTTP read timed out is how an
account ends up with double the intended position.
"""
import argparse, datetime as dt, json, os, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))


class Halt(Exception):
    pass


# ---------------------------------------------------------------- kill switches

def kill_switches(decision, acct, positions, cfg, max_stale_hours=30.0, expected_hash=None):
    """Every condition that must be false before new risk is opened. Returns a list of reasons."""
    bad = []
    if not decision:
        # No decision yet is a distinct state from a bad one. Reporting "hash changed" here would
        # be a false alarm, and a safety harness that cries wolf on a clean start teaches you to
        # ignore it.
        bad.append('no decision file yet — run portfolio_live.py first')
    else:
        if expected_hash and decision.get('config_hash') != expected_hash:
            bad.append('config hash changed since the freeze')
        st = decision.get('data', {}).get('stale_hours')
        if st is None:
            bad.append('decision file records no data age')
        elif st > max_stale_hours:
            bad.append('signal data stale: %.1f hours' % st)
    if not acct or acct.get('equity') is None:
        bad.append('account state unavailable')
    snap_age = None
    if acct.get('ts'):
        snap_age = (dt.datetime.now(dt.timezone.utc).timestamp() - float(acct['ts'])) / 3600.0
        if snap_age > max_stale_hours:
            bad.append('account snapshot %.1f hours old' % snap_age)
    declared = {p['ticker']: float(p['position']) for p in positions}
    booked = {r['kalshi']: r['contracts'] for r in decision.get('integer_book', []) if r['kalshi']}
    for tk, n in declared.items():
        if tk not in booked and abs(n) > 0:
            bad.append('position in %s (%g) is not part of the current book' % (tk, n))
    eq = float(acct.get('equity') or 0)
    if eq <= 0:
        bad.append('non-positive equity')
    return bad


def loss_guards(equity_now, equity_start, cfg):
    """Drawdown against the equity the canary started with."""
    lim = cfg['canary_risk_limits']
    if not equity_start:
        return 'UNKNOWN', 0.0
    dd = (equity_start - equity_now) / equity_start * 100.0
    if dd >= lim['drawdown_halt_new_risk_pct']:
        return 'HALT', dd
    if dd >= lim['drawdown_warning_pct']:
        return 'WARN', dd
    return 'OK', dd


# ---------------------------------------------------------------- pre-trade screen

def pre_trade_screen(order, ctx):
    """Every check the spec lists, run immediately before the order leaves. Any failure rejects.

    The checks are deliberately redundant with the planner. The planner is where a bug would be
    subtle; this is where it has to be caught, because everything after it is irreversible.
    """
    fails = []
    tk = order.get('ticker')
    m = ctx['markets'].get(tk)
    if not m:
        fails.append('unknown ticker %s' % tk)
        return fails
    if m.get('status') != 'active' or not m.get('is_open'):
        fails.append('%s is not open for trading' % tk)
    if order['side'] not in ('bid', 'ask'):
        fails.append("bad side %r (perps use 'bid'/'ask')" % order['side'])
    n = int(order.get('count') or 0)
    if n <= 0:
        fails.append('non-positive count')
    px = order.get('price')                       # DOLLARS, four decimals, not cents
    if px is None:
        fails.append('limit order without a price')
    else:
        bid, ask = float(m['bid']), float(m['ask'])
        lo, hi = min(bid, ask) * 0.90, max(bid, ask) * 1.10
        if not (lo <= float(px) <= hi):
            fails.append('price %.4f is outside +-10%% of the book [%.4f, %.4f]'
                         % (float(px), bid, ask))
        tick = float(m.get('tick') or 0)
        if tick:
            steps = float(px) / tick
            if abs(steps - round(steps)) > 1e-6:
                fails.append('price %.4f is not a multiple of the %g tick' % (float(px), tick))

    cur = ctx['positions'].get(tk, 0.0)
    tgt = ctx['targets'].get(tk, 0.0)
    signed = n if order['side'] == 'bid' else -n
    if order.get('reduce_only') and abs(cur + signed) > abs(cur) + 1e-9:
        fails.append('reduce_only order would increase the position')
    after = cur + signed
    if abs(after - tgt) > abs(cur - tgt) + 1e-9:
        fails.append('order moves %s away from target (%g -> %g, target %g)'
                     % (tk, cur, after, tgt))

    price = float(m['ask'] if order['side'] == 'bid' else m['bid'])
    notional_after = abs(after) * price
    eq = ctx['equity']
    lim = ctx['limits']
    if notional_after > eq * lim['single_name_notional_over_equity_max'] + 1e-9:
        fails.append('single-name notional $%.2f exceeds %.0f%% of equity'
                     % (notional_after, 100 * lim['single_name_notional_over_equity_max']))
    gross_after = sum(abs(ctx['positions'].get(k, 0.0) + (signed if k == tk else 0)) *
                      float(ctx['markets'][k]['mark'])
                      for k in set(list(ctx['positions']) + [tk]) if k in ctx['markets'])
    if gross_after > eq * lim['gross_over_equity_max'] + 1e-9:
        fails.append('gross $%.2f exceeds %.1fx equity'
                     % (gross_after, lim['gross_over_equity_max']))
    margin_after = sum(abs(ctx['positions'].get(k, 0.0) + (signed if k == tk else 0)) *
                       float(ctx['markets'][k]['mark']) /
                       float(ctx['markets'][k].get('leverage') or 1.0)
                       for k in set(list(ctx['positions']) + [tk]) if k in ctx['markets'])
    if margin_after > eq * lim['margin_utilization_max'] + 1e-9:
        fails.append('margin utilisation %.1f%% exceeds %.0f%%'
                     % (100 * margin_after / eq, 100 * lim['margin_utilization_max']))
    return fails


# ---------------------------------------------------------------- reconciliation

def reconcile(client, journal):
    """Resolve every live intent that has no terminal state, using the exchange as the authority."""
    out = []
    for row in journal.unresolved():
        coid = row['coid']
        state = 'unknown'
        try:
            orders = client.paged('/margin/orders', 'orders', limit=200)
            match = [o for o in orders if o.get('client_order_id') == coid]
            if match:
                state = match[0].get('status') or 'found'
                journal.settle(coid, state)
            else:
                fills = client.paged('/margin/fills', 'fills', limit=200)
                if any(f.get('client_order_id') == coid for f in fills):
                    state = 'filled'
                    journal.settle(coid, state)
                else:
                    state = 'absent_at_exchange'
                    journal.settle(coid, state)
        except RuntimeError as e:
            state = 'query_failed: %s' % str(e)[:120]
        out.append({'client_order_id': coid, 'resolved_state': state,
                    'intent': row['body'], 'acked': bool(row['acked'])})
    return out


# ---------------------------------------------------------------- flatten

def flatten_plan(positions, markets, aggressive=True):
    """Orders that take every position to zero. Marketable limits, reduce-only, never market.

    A market order on a thin perp book is how a flatten becomes the worst fill of the week. These
    are limits placed one tick THROUGH the touch, which fill immediately in any normal book and
    still refuse an absurd price in a broken one. reduce_only is what guarantees a flatten can
    only ever shrink a position, even if the position figure it was built from is stale.
    """
    plan = []
    for tk, n in positions.items():
        n = float(n)
        if abs(n) < 1e-9 or tk not in markets:
            continue
        m = markets[tk]
        tick = float(m.get('tick') or 0.0001)
        if n > 0:
            px = float(m['bid']) - (tick if aggressive else 0.0)
            side = 'ask'
        else:
            px = float(m['ask']) + (tick if aggressive else 0.0)
            side = 'bid'
        px = round(round(px / tick) * tick, 6)
        plan.append({'ticker': tk, 'side': side, 'count': int(abs(round(n))),
                     'price': px, 'tick': tick, 'reduce_only': True,
                     'tif': 'immediate_or_cancel', 'reason': 'emergency flatten'})
    return plan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', default=os.path.join(ROOT, 'data', 'snapshots',
                                                       'kalshi_perp_snapshot.json'))
    ap.add_argument('--decision', default=os.path.join(ROOT, 'reports', 'decisions', 'latest.json'))
    ap.add_argument('--test-flatten', action='store_true',
                    help='exercise the whole flatten path with the arm disabled')
    ap.add_argument('--equity-start', type=float, default=None)
    args = ap.parse_args()

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    want_hash = open(os.path.join(ROOT, 'config', 'portfolio_v1.sha256')).read().strip()
    snap = json.load(open(args.snapshot))
    markets = {m['ticker']: m for m in snap['markets']}
    acct = snap['account']
    positions = {p['ticker']: float(p['position']) for p in snap.get('positions', [])}
    decision = json.load(open(args.decision)) if os.path.exists(args.decision) else {}

    print('SAFETY CHECK   %s' % dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'))
    print('equity $%.4f   positions %d   markets %d'
          % (float(acct['equity']), len(positions), len(markets)))

    bad = kill_switches(decision, acct, snap.get('positions', []), cfg,
                        expected_hash=want_hash)
    print('\nKILL SWITCHES')
    if bad:
        for b in bad:
            print('  BLOCK  %s' % b)
    else:
        print('  clear — no condition blocks opening new risk')

    state, dd = loss_guards(float(acct['equity']), args.equity_start, cfg)
    print('\nLOSS GUARD   %s   drawdown %.2f%% (warn %.0f%%, halt %.0f%%)'
          % (state, dd, cfg['canary_risk_limits']['drawdown_warning_pct'],
             cfg['canary_risk_limits']['drawdown_halt_new_risk_pct']))

    plan = flatten_plan(positions, markets)
    print('\nEMERGENCY FLATTEN PLAN (%d order(s))' % len(plan))
    ctx = {'markets': markets, 'positions': positions,
           'targets': {tk: 0.0 for tk in positions},
           'equity': float(acct['equity']), 'limits': cfg['canary_risk_limits']}
    for o in plan:
        fails = pre_trade_screen(o, ctx)
        print('  %-13s %-4s %4d @ $%.4f  reduce_only  screen: %s'
              % (o['ticker'], o['side'], o['count'], o['price'],
                 'PASS' if not fails else 'REJECT ' + '; '.join(fails)))

    if args.test_flatten:
        print('\nTESTING THE FLATTEN PATH WITH THE ARM DISABLED')
        try:
            from kalshi_client import Kalshi, Journal
            k = Kalshi()
            j = Journal()
            host_ok, tok_ok = k.armed()
            print('  arm state: host_ok=%s token_ok=%s -> %s'
                  % (host_ok, tok_ok, 'ARMED' if host_ok and tok_ok else 'DISARMED'))
            for o in plan:
                r = k.create_perp_order(ticker=o['ticker'], side=o['side'], count=o['count'],
                                        price=o['price'], tick=o['tick'],
                                        reduce_only=True, tif=o['tif'],
                                        client_order_id='flattentest-%s' % o['ticker'],
                                        journal=j, dry_run=True)
                print('  %-13s -> %s' % (o['ticker'], json.dumps(r)[:120]))
            print('  journal rows written: %d unresolved live intents' % len(j.unresolved()))
            print('  PATH EXERCISED END TO END. Nothing was sent.')
        except SystemExit as e:
            print('  client unavailable here: %s' % e)
            print('  the signing key is outside this session by design; run this on the Mac.')
        except Exception as e:
            print('  FLATTEN PATH ERROR: %s' % str(e)[:200])
    return 0


if __name__ == '__main__':
    sys.exit(main())
