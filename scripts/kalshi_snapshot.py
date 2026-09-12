#!/usr/bin/env python3
"""Write data/snapshots/kalshi_perp_snapshot.json from the exchange, not from the recorder.

EVERY STALENESS BUG IN THIS HARNESS SO FAR CAME FROM THE SAME PLACE: a decision priced against a
file that was written minutes or hours earlier. The live flatten failed because a snapshot bid of
0.4001 had become 0.3990. The safety check reported a position that had already been closed. The
recorder is an excellent research tape and a poor source of truth for an order that is about to
be sent.

So this reads the account, the positions and the markets from the API when the signing key is
reachable, and falls back to prod.sqlite only when it is not -- saying clearly which it used, so
a snapshot built from the tape can never be mistaken for one built from the exchange.
"""
import argparse, datetime as dt, json, os, sqlite3, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
OUT = os.path.join(ROOT, 'data', 'snapshots', 'kalshi_perp_snapshot.json')

NUM = ('bid', 'ask', 'last', 'mark', 'liquidation_mark', 'reference', 'leverage', 'tick',
       'contract_size', 'volume_24h', 'open_interest', 'funding_rate', 'position',
       'entry_price', 'unrealized_pnl', 'margin_used', 'fees', 'equity', 'available',
       'settled_funds', 'position_value', 'maintenance_margin', 'initial_margin')


def num(d):
    """The perp API returns numbers as strings. Coerce once, here, rather than at every call site."""
    out = {}
    for k, v in (d or {}).items():
        if k in NUM and isinstance(v, str):
            try:
                out[k] = float(v)
                continue
            except ValueError:
                pass
        out[k] = v
    return out


def from_api():
    """Mirror src/perps.cpp parse_perp_balance exactly, rather than guessing field names.

    /margin/balance does NOT report equity at the top level. It returns settled_funds plus a
    subaccount_balances array, and equity is the sum of account_equity across those subaccounts.
    Retail members see per-subaccount equity; a self-clearing member sees zeros there, in which
    case settled funds plus position value is the honest fallback. Guessing at 'equity',
    'portfolio_value' and 'total_value' produced a snapshot with equity None, which the kill
    switches caught but two downstream tools crashed on.
    """
    from kalshi_client import Kalshi
    k = Kalshi()
    bal = k.perp_balance()

    def f(d, key):
        v = d.get(key)
        try:
            return float(v)
        except (TypeError, ValueError):
            return 0.0

    acct = {'ts': int(time.time()), 'settled_funds': f(bal, 'settled_funds'),
            'position_value': 0.0, 'equity': 0.0, 'maintenance_margin': 0.0,
            'initial_margin': 0.0, 'resting_orders_margin': 0.0, 'available': 0.0}
    subs = bal.get('subaccount_balances') or []
    for sub in subs:
        acct['position_value'] += f(sub, 'position_value')
        acct['equity'] += f(sub, 'account_equity')
        acct['maintenance_margin'] += f(sub, 'maintenance_margin')
        acct['initial_margin'] += f(sub, 'initial_margin')
        acct['resting_orders_margin'] += f(sub, 'resting_orders_margin')
        acct['available'] += f(sub, 'available_balance')
    if acct['equity'] <= 0:
        acct['equity'] = acct['settled_funds'] + acct['position_value']
        acct['equity_source'] = 'settled_funds + position_value (subaccount equity reported zero)'
    else:
        acct['equity_source'] = 'sum of subaccount account_equity'
    if acct['available'] <= 0:
        acct['available'] = acct['equity'] - acct['initial_margin']
    acct['subaccounts'] = len(subs)

    pr = k.perp_positions()
    positions = []
    for p in (pr.get('positions') or pr.get('market_positions') or []):
        p = num(p)
        tk = p.get('market_ticker') or p.get('ticker')
        n = float(p.get('position') or 0)
        if not tk or abs(n) < 1e-12:
            continue
        positions.append({'ticker': tk, 'subaccount': p.get('subaccount', 0), 'position': n,
                          'entry_price': p.get('entry_price'),
                          'unrealized_pnl': p.get('unrealized_pnl'),
                          'margin_used': p.get('margin_used'), 'fees': p.get('fees'),
                          'is_portfolio': p.get('is_portfolio'),
                          'updated_ts': int(time.time())})

    # Field names read from src/perps.cpp parse_perp_market, not guessed. The list endpoint
    # calls them tick_size, price, leverage_estimate, and wraps the three marks in objects of
    # {price, ts_ms}. Storing them under the recorder's shorter names keeps every consumer working
    # against one vocabulary.
    def tp(m, key):
        """settlement_mark_price and friends arrive as {price, ts_ms}, not as bare numbers."""
        v = m.get(key)
        if isinstance(v, dict):
            try:
                return float(v.get('price'))
            except (TypeError, ValueError):
                return None
        try:
            return float(v)
        except (TypeError, ValueError):
            return None

    def side(v):
        """Kalshi sends INT64_MAX for 'no order on this side', which parses as 9.2e14.

        Recorded straight through it made the basis read 6.8e16 bps on a dozen instruments. A
        price that large is not a price; absent is absent.
        """
        try:
            v = float(v)
        except (TypeError, ValueError):
            return None
        return v if (v == v and 0 < v < 1e9) else None

    mr = k.perp_markets(status='active')
    markets = []
    for m in (mr.get('markets') or []):
        tk = m.get('ticker')
        if not tk:
            continue
        sched = m.get('schedule') if isinstance(m.get('schedule'), dict) else {}
        bid, ask = side(m.get('bid')), side(m.get('ask'))
        mark = tp(m, 'settlement_mark_price')
        if mark is None and bid is not None and ask is not None:
            mark = (bid + ask) / 2.0
        markets.append({'ticker': tk, 'title': m.get('title'), 'status': m.get('status'),
                        'asset_class': m.get('asset_class'),
                        'tick': float(m.get('tick_size') or 0.0001),
                        'contract_size': m.get('contract_size'),
                        'bid': bid, 'ask': ask,
                        'last': tp(m, 'price'),
                        'mark': mark,
                        'liquidation_mark': tp(m, 'liquidation_mark_price'),
                        'reference': tp(m, 'reference_price'),
                        'leverage': float(m.get('leverage_estimate') or 0) or None,
                        'volume_24h': m.get('volume_24h'),
                        'open_interest': m.get('open_interest'),
                        'is_open': bool(sched.get('is_open', True)),
                        'fractional': bool(m.get('fractional_trading_enabled', False)),
                        'funding_rate': m.get('funding_rate'),
                        'next_funding': m.get('next_funding'),
                        'updated_ts': int(time.time())})

    bad = [m['ticker'] for m in markets
           if m['bid'] is None or m['ask'] is None or m['mark'] is None or not m['leverage']]
    if bad:
        print('markets missing a price or leverage (excluded from sizing): %s'
              % ', '.join(sorted(bad)[:8]))

    return acct, positions, markets


def from_recorder():
    db = sqlite3.connect('file:%s?mode=ro' % os.path.join(ROOT, 'data', 'prod.sqlite'), uri=True)
    cols = lambda t: [r[1] for r in db.execute('PRAGMA table_info(%s)' % t)]
    acct = dict(zip(cols('perp_account'),
                    db.execute('select * from perp_account order by ts desc limit 1').fetchone()))
    markets = [dict(zip(cols('perp_markets'), r))
               for r in db.execute('select * from perp_markets')]
    positions = [dict(zip(cols('perp_positions'), r))
                 for r in db.execute('select * from perp_positions')
                 if abs(float(dict(zip(cols('perp_positions'), r))['position'])) > 1e-12]
    return acct, positions, markets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--recorder', action='store_true', help='force the tape, skip the API')
    args = ap.parse_args()

    source = 'recorder'
    if not args.recorder:
        try:
            acct, positions, markets = from_api()
            source = 'exchange'
        except (SystemExit, RuntimeError, ImportError, KeyError, TypeError, ValueError) as e:
            print('API unavailable (%s); falling back to the recorder tape' % str(e)[:120])
            acct, positions, markets = from_recorder()
    else:
        acct, positions, markets = from_recorder()

    eq = acct.get('equity')
    if eq is None or float(eq) <= 0:
        raise SystemExit('refusing to write a snapshot with unusable equity (%r). Everything '
                         'downstream prices against this field; a null here is how three tools '
                         'crash at once.' % eq)
    snap = {'captured_ts': int(time.time()), 'source': source,
            'account': acct, 'markets': markets, 'positions': positions}
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(snap, f, indent=1)

    print('snapshot source: %s   %s'
          % (source.upper(), dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')))
    print('equity $%.4f   available $%.4f   markets %d   open positions %d'
          % (float(acct['equity']), float(acct.get('available') or 0),
             len(markets), len(positions)))
    if acct.get('equity_source'):
        print('equity from: %s' % acct['equity_source'])
    for p in positions:
        print('  %-13s %+g @ %s' % (p['ticker'], p['position'], p.get('entry_price')))
    if not positions:
        print('  (flat)')
    if source == 'recorder':
        print('\nThis snapshot came from the tape, not the exchange. It can lag by minutes.')
        print('Do not send orders priced from it without a live quote refresh.')
    print('\nwrote %s' % args.out)


if __name__ == '__main__':
    sys.exit(main())
