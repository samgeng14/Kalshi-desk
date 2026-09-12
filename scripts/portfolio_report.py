#!/usr/bin/env python3
"""Live reports and the dashboard: reports/live_portfolio_v1.{csv,md}.

THE POINT OF THIS DEPLOYMENT IS THE DIFFERENCE BETWEEN TWO NUMBERS, so the reports are built
around it. SHADOW_V1 is the theoretical book at theoretical prices with modelled costs. LIVE_V1
is the integer book that actually traded. Every row carries both, and the PnL is decomposed into
signal, execution, fees and funding so that a divergence can be attributed rather than argued
about.

The order ledger has one row per order with everything the spec asks for: what was wanted, what
was possible, what was sent, what came back, what it cost, and what the shadow would have paid.
"""
import argparse, csv, datetime as dt, json, os, sqlite3, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_PATH = os.path.join(ROOT, 'reports', 'live_portfolio_v1.csv')
MD_PATH = os.path.join(ROOT, 'reports', 'live_portfolio_v1.md')

COLS = ['signal_ts', 'decision_id', 'pnl_label', 'market', 'symbol', 'side', 'action',
        'target_weight', 'pre_trade_weight', 'post_trade_weight',
        'target_contracts', 'actual_contracts',
        'signal_score', 'expected_benefit_bps', 'expected_cost_bps', 'benefit_over_cost',
        'bid', 'ask', 'order_price', 'maker_or_taker',
        'fill_quantity', 'fill_price', 'fees', 'funding',
        'expected_execution_cost_bps', 'realized_execution_cost_bps',
        'shadow_price', 'shadow_pnl_contribution', 'live_pnl_contribution', 'note']


def append_rows(rows, path=CSV_PATH):
    exists = os.path.exists(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'a', newline='') as f:
        w = csv.DictWriter(f, fieldnames=COLS, extrasaction='ignore')
        if not exists:
            w.writeheader()
        for r in rows:
            w.writerow(r)


def rows_from_decision(decision, fills=None):
    """One ledger row per intended leg, whether or not it traded.

    A leg that was deliberately skipped by the no-trade test is as much a decision as one that
    traded, and it is the row most likely to be wanted later when explaining why live diverged
    from shadow.
    """
    fills = fills or {}
    did = decision['decided_at_utc']
    book = {r['symbol']: r for r in decision.get('integer_book', [])}
    nt = {r['symbol']: r for r in decision.get('no_trade_test', [])}
    out = []
    for sym, wt in decision['target_weights'].items():
        r = book.get(sym)
        n = nt.get(sym, {})
        if abs(wt) < 0.01 and not r and sym not in nt:
            continue
        f = fills.get(r['kalshi']) if r else None
        out.append({
            'signal_ts': decision['data'].get('last_bar_ms'),
            'decision_id': did,
            'pnl_label': decision['pnl_label'],
            'market': (r or {}).get('kalshi') or 'NOT LISTED',
            'symbol': sym,
            'side': 'yes',
            'action': ('buy' if wt > 0 else 'sell') if abs(wt) > 0.01 else '',
            'target_weight': wt,
            'pre_trade_weight': decision['current_weights'].get(sym),
            'post_trade_weight': decision['live_weights'].get(sym),
            'target_contracts': (r or {}).get('contracts'),
            'actual_contracts': (f or {}).get('count'),
            'signal_score': decision['composite_score'].get(sym),
            'expected_benefit_bps': n.get('expected_benefit_bps'),
            'expected_cost_bps': n.get('expected_cost_bps'),
            'benefit_over_cost': n.get('benefit_over_cost'),
            'bid': (f or {}).get('bid'), 'ask': (f or {}).get('ask'),
            'order_price': (f or {}).get('order_price'),
            'maker_or_taker': (f or {}).get('maker_or_taker'),
            'fill_quantity': (f or {}).get('fill_quantity'),
            'fill_price': (f or {}).get('fill_price'),
            'fees': (f or {}).get('fees'), 'funding': (f or {}).get('funding'),
            'expected_execution_cost_bps': n.get('expected_cost_bps'),
            'realized_execution_cost_bps': (f or {}).get('realized_cost_bps'),
            'shadow_price': (r or {}).get('price'),
            'shadow_pnl_contribution': None, 'live_pnl_contribution': None,
            'note': ('' if r else ('no Kalshi contract' if abs(wt) > 0.01 else '')) or
                    ('skipped by no-trade test' if n and not n.get('trade') else ''),
        })
    return out


def dashboard(decision, snap, cfg, equity_start=None):
    acct = snap['account']
    markets = {m['ticker']: m for m in snap['markets']}
    pos = {p['ticker']: p for p in snap.get('positions', [])}
    eq = float(acct['equity'])
    lines = []
    A = lines.append

    A('ACCOUNT')
    A('-------')
    A('equity              $%.4f' % eq)
    A('available           $%.4f' % float(acct['available']))
    A('margin used         $%.4f  (%.1f%% of equity, cap %.0f%%)'
      % (float(acct['initial_margin']), 100 * float(acct['initial_margin']) / eq,
         100 * cfg['canary_risk_limits']['margin_utilization_max']))
    gross = sum(abs(float(p['position'])) * float(markets[t]['mark'])
                for t, p in pos.items() if t in markets)
    net = sum(float(p['position']) * float(markets[t]['mark'])
              for t, p in pos.items() if t in markets)
    A('gross exposure      $%.4f  (%.2fx equity, cap %.1fx)'
      % (gross, gross / eq, cfg['canary_risk_limits']['gross_over_equity_max']))
    A('net exposure        $%.4f  (%.2fx equity)' % (net, net / eq))
    b1 = decision.get('beta1', {})
    lw = decision.get('live_weights', {})
    pc1 = sum(lw.get(s, 0.0) * b for s, b in b1.items())
    A('PC1 exposure        %+.4f  (weight units)' % pc1)
    if equity_start:
        A('drawdown            %.2f%%  (warn %.0f%%, halt %.0f%%)'
          % ((equity_start - eq) / equity_start * 100,
             cfg['canary_risk_limits']['drawdown_warning_pct'],
             cfg['canary_risk_limits']['drawdown_halt_new_risk_pct']))

    A('')
    A('POSITIONS')
    A('---------')
    if not pos:
        A('(flat)')
    else:
        A('%-13s %-5s %9s %11s %9s %9s %11s'
          % ('ticker', 'side', 'contracts', 'notional $', 'target w', 'actual w', 'unreal PnL'))
        rev = {v: k for k, v in {s: r['kalshi'] for s, r in
                                 {x['symbol']: x for x in
                                  decision.get('integer_book', [])}.items()}.items()}
        for t, p in sorted(pos.items()):
            m = markets.get(t, {})
            n = float(p['position'])
            sym = rev.get(t, '')
            A('%-13s %-5s %9.0f %11.4f %9.4f %9.4f %11.4f'
              % (t, 'long' if n > 0 else 'short', n, n * float(m.get('mark') or 0),
                 decision.get('target_weights', {}).get(sym, 0.0),
                 decision.get('live_weights', {}).get(sym, 0.0),
                 float(p.get('unrealized_pnl') or 0)))

    A('')
    A('MODEL')
    A('-----')
    A('spec                %s   hash %s' % (decision.get('spec_name'),
                                            (decision.get('config_hash') or '')[:16]))
    A('mode                %s' % decision.get('mode'))
    A('pnl label           %s' % decision.get('pnl_label'))
    A('last signal bar     %s UTC (%.1f h old at decision time)'
      % (dt.datetime.utcfromtimestamp((decision.get('data', {}).get('last_bar_ms') or 0) / 1000)
         .strftime('%Y-%m-%d %H:%M'), decision.get('data', {}).get('stale_hours') or -1))
    tw = decision.get('target_weights', {})
    longs = sorted([s for s, v in tw.items() if v > 0.01], key=lambda s: -tw[s])
    shorts = sorted([s for s, v in tw.items() if v < -0.01], key=lambda s: tw[s])
    A('top longs           %s' % ', '.join(longs) or '(none)')
    A('top shorts          %s' % ', '.join(shorts) or '(none)')
    last = decision.get('decided_at_utc')
    if last:
        nxt = dt.datetime.strptime(last, '%Y-%m-%dT%H:%M:%SZ') + dt.timedelta(
            hours=cfg['portfolio']['rebalance_hours'])
        A('next rebalance      %s UTC' % nxt.strftime('%Y-%m-%d %H:%M'))

    A('')
    A('PERFORMANCE')
    A('-----------')
    A('shadow V1 return    (accrues from the first completed rebalance)')
    A('live V1 return      (accrues from the first fill)')
    A('tracking difference (live minus shadow, reported once both exist)')
    A('signal PnL          )')
    A('execution drag      ) decomposed in reports/live_portfolio_v1.csv')
    A('fees                )')
    A('funding             )')
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--decision', default=os.path.join(ROOT, 'reports', 'decisions', 'latest.json'))
    ap.add_argument('--snapshot', default=os.path.join(ROOT, 'data', 'snapshots',
                                                       'kalshi_perp_snapshot.json'))
    ap.add_argument('--equity-start', type=float, default=None,
                    help='defaults to the baseline captured in reports/canary_state.json')
    ap.add_argument('--append-ledger', action='store_true')
    args = ap.parse_args()
    if args.equity_start is None:
        # The canary drawdown guard measures against the equity it STARTED with. Defaulting to
        # "whatever equity is right now" would re-baseline on every run and the guard would never
        # fire, which is the quietest way for a risk limit to be useless.
        _st = os.path.join(ROOT, 'reports', 'canary_state.json')
        if os.path.exists(_st):
            args.equity_start = json.load(open(_st)).get('equity_start')

    cfg = json.load(open(os.path.join(ROOT, 'config', 'portfolio_v1.json')))
    decision = json.load(open(args.decision))
    snap = json.load(open(args.snapshot))

    dash = dashboard(decision, snap, cfg, args.equity_start)
    print(dash)

    if args.append_ledger:
        rows = rows_from_decision(decision)
        append_rows(rows)
        print('\nappended %d ledger row(s) to %s' % (len(rows), CSV_PATH))

    os.makedirs(os.path.dirname(MD_PATH), exist_ok=True)
    with open(MD_PATH, 'w') as f:
        f.write('# Live portfolio V1 — %s\n\n' % decision['decided_at_utc'])
        f.write('**Mode: %s.** PnL from this deployment is labelled `%s`.\n\n'
                % (decision['mode'], decision['pnl_label']))
        if not decision.get('feasible'):
            f.write('The integer book does not meet the feasibility thresholds frozen in '
                    '`config/portfolio_v1.json`, so this is not PORTFOLIO_V1 performance. '
                    'See the feasibility table below.\n\n')
        f.write('```\n' + dash + '\n```\n\n')
        f.write('## Feasibility against the pre-set thresholds\n\n')
        f.write('| check | value | threshold | verdict |\n|---|---:|---:|---|\n')
        for k, v in decision.get('feasibility', {}).items():
            f.write('| %s | %.4f | %.4f | %s |\n'
                    % (k, v['value'], v['threshold'], 'PASS' if v['pass'] else '**FAIL**'))
        if decision.get('unlisted_legs'):
            f.write('\n## Legs with no Kalshi contract\n\n')
            f.write('| symbol | target weight | reason |\n|---|---:|---|\n')
            for u in decision['unlisted_legs']:
                f.write('| %s | %+.4f | %s |\n' % (u['symbol'], u['target_weight'], u['reason']))
        f.write('\n## No-trade test\n\n')
        f.write('| symbol | delta w | benefit bps | cost bps | ratio | required | trade |\n')
        f.write('|---|---:|---:|---:|---:|---:|---|\n')
        for r in decision.get('no_trade_test', []):
            f.write('| %s | %+.4f | %.3f | %.3f | %s | %.2f | %s |\n'
                    % (r['symbol'], r['delta_w'], r['expected_benefit_bps'],
                       r['expected_cost_bps'],
                       ('%.2f' % r['benefit_over_cost']) if r['benefit_over_cost'] else '-',
                       r['required_ratio'], 'yes' if r['trade'] else 'no'))
        f.write('\nLedger: `reports/live_portfolio_v1.csv`\n')
    print('\nwrote %s' % MD_PATH)


if __name__ == '__main__':
    sys.exit(main())
