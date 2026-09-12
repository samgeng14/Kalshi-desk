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
    from kalshi_client import Kalshi
    k = Kalshi()
    bal = num(k.perp_balance())
    acct = {'ts': int(time.time()),
            'equity': bal.get('equity'),
            'available': bal.get('available_balance', bal.get('available')),
            'settled_funds': bal.get('settled_funds'),
            'position_value': bal.get('position_value'),
            'maintenance_margin': bal.get('maintenance_margin'),
            'initial_margin': bal.get('initial_margin')}
    if acct['equity'] is None:
        # Field names vary; keep the raw payload so nothing is silently lost.
        acct['raw'] = bal
        for key in ('portfolio_value', 'total_value', 'balance'):
            if bal.get(key) is not None:
                acct['equity'] = float(bal[key])
                break

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

    mr = k.perp_markets(status='active')
    markets = []
    for m in (mr.get('markets') or []):
        m = num(m)
        tk = m.get('ticker')
        if not tk:
            continue
        markets.append({'ticker': tk, 'title': m.get('title'), 'status': m.get('status'),
                        'asset_class': m.get('asset_class'), 'tick': m.get('tick'),
                        'contract_size': m.get('contract_size'), 'bid': m.get('bid'),
                        'ask': m.get('ask'), 'last': m.get('last'), 'mark': m.get('mark'),
                        'reference': m.get('reference'), 'leverage': m.get('leverage'),
                        'volume_24h': m.get('volume_24h'),
                        'open_interest': m.get('open_interest'),
                        'is_open': m.get('is_open', True),
                        'funding_rate': m.get('funding_rate'),
                        'next_funding': m.get('next_funding'),
                        'updated_ts': int(time.time())})
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

    snap = {'captured_ts': int(time.time()), 'source': source,
            'account': acct, 'markets': markets, 'positions': positions}
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, 'w') as f:
        json.dump(snap, f, indent=1)

    print('snapshot source: %s   %s'
          % (source.upper(), dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')))
    print('equity $%s   markets %d   open positions %d'
          % (acct.get('equity'), len(markets), len(positions)))
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
