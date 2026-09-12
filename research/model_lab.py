#!/usr/bin/env python3
"""Read-only research models. Never submits orders or changes a live trading setting.

Models are fitted on independent event/day groups, not millions of correlated quotes.
The report is a prospective research artifact, not an automatically deployed strategy.
"""
import argparse
import bisect
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import time

VERSION = 'model-lab-v1'


def timestamp(value):
    if not value:
        return 0
    try:
        return int(dt.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp())
    except (ValueError, AttributeError):
        return 0


def number(x):
    return isinstance(x, (int, float)) and math.isfinite(x)


def bucket(p):
    return min(9, max(0, int(p * 10)))


def fit_bins(rows):
    # Unit of weight is an event, regardless of the number of brackets/observations it has.
    sizes = {}
    for r in rows:
        sizes[r['group']] = sizes.get(r['group'], 0) + 1
    bins = [{'weight': 0., 'wins': 0., 'prior_sum': 0., 'groups': set()} for _ in range(10)]
    for r in rows:
        b = bins[bucket(r['p'])]
        w = 1 / sizes[r['group']]
        b['weight'] += w
        b['wins'] += w * r['y']
        b['prior_sum'] += w * r['p']
        b['groups'].add(r['group'])
    out = []
    for i, b in enumerate(bins):
        # Shrink toward the input probability, not toward an invented 50% win rate.
        center = b['prior_sum'] / b['weight'] if b['weight'] else (i + .5) / 10
        alpha, beta = 20 * center + b['wins'], 20 * (1 - center) + b['weight'] - b['wins']
        out.append({'lower': i / 10, 'upper': (i + 1) / 10, 'p': alpha / (alpha + beta),
                    'event_weight': b['weight'], 'groups': len(b['groups']),
                    'sigma': math.sqrt(alpha * beta / ((alpha + beta)**2 * (alpha + beta + 1)))})
    return out


def predict(bins, p):
    b = bins[bucket(p)]
    # Empty buckets keep the supplied probability, rather than extrapolating a bucket midpoint.
    return b['p'] if b['event_weight'] else p


def evaluate(rows, predict_fn):
    groups = {}
    for r in rows:
        p = predict_fn(r)
        scores = groups.setdefault(r['group'], [])
        scores.append(((p - r['y'])**2, (r['baseline'] - r['y'])**2))
    scores = [(sum(x[0] for x in v) / len(v), sum(x[1] for x in v) / len(v)) for v in groups.values()]
    if not scores:
        return {'groups': 0, 'model_brier': None, 'baseline_brier': None, 'difference': None}
    mean = lambda xs: sum(xs) / len(xs)
    model, baseline = mean([x[0] for x in scores]), mean([x[1] for x in scores])
    return {'groups': len(scores), 'model_brier': model, 'baseline_brier': baseline, 'difference': model - baseline}


def train_report(name, objective, rows, minimum_groups=100, minimum_days=20):
    # Every day is entirely train OR test. No game/city-day appears on both sides.
    days = sorted({r['day'] for r in rows})
    split = max(1, int(len(days) * .7))
    train_days = set(days[:split]) if len(days) >= 2 else set(days)
    train = [r for r in rows if r['day'] in train_days]
    test = [r for r in rows if r['day'] not in train_days]
    fitted = fit_bins(train)
    metrics = evaluate(test, lambda r: predict(fitted, r['p']))
    enough = len({r['group'] for r in train}) >= minimum_groups and len(train_days) >= minimum_days and metrics['groups'] >= 30
    return {'name': name, 'objective': objective, 'status': 'shadow_validation' if enough else 'insufficient_independent_data',
            'live_enabled': False, 'rows': len(rows), 'groups': len({r['group'] for r in rows}), 'days': len(days),
            'train_groups': len({r['group'] for r in train}), 'train_days': sorted(train_days),
            'holdout_days': [d for d in days if d not in train_days], 'holdout': metrics,
            'minimum_train_groups': minimum_groups, 'minimum_train_days': minimum_days,
            'validation_parameters': fitted, 'prospective_parameters': fit_bins(rows),
            'notice': 'Retrospective day-grouped prediction test. A lower Brier score does not establish fills or profitable trading. No automatic promotion.'}


def weather_rows(c):
    # One forecast per contract/day, 6-30h before expected settlement. All brackets share one
    # event weight and one day split. No selection on whether its side won or appeared cheap.
    rows, seen = [], set()
    for r in c.execute("""SELECT f.ts,f.ticker,f.p,f.yes_bid,f.yes_ask,m.event_ticker,m.expected_expiration_time,
                              m.expiration_time,m.result
                         FROM model_fairs f JOIN markets m ON m.ticker=f.ticker
                         WHERE f.strategy='weather_nws' AND m.result IN ('yes','no') ORDER BY f.ts,f.id"""):
        end = timestamp(r['expected_expiration_time']) or timestamp(r['expiration_time'])
        if not end or not 6 * 3600 <= end - r['ts'] <= 30 * 3600:
            continue
        key = r['ticker']
        if key in seen or not all(number(r[k]) for k in ('p','yes_bid','yes_ask')):
            continue
        if not 0 < r['p'] < 1 or not 0 < r['yes_bid'] < r['yes_ask'] < 1:
            continue
        seen.add(key)
        rows.append({'group': r['event_ticker'], 'day': dt.datetime.fromtimestamp(end, dt.timezone.utc).date().isoformat(),
                     'p': r['p'], 'baseline': (r['yes_bid'] + r['yes_ask']) / 2, 'y': int(r['result'] == 'yes')})
    return rows


def top(book, side):
    own = book.get('yes_bids' if side == 'yes' else 'no_bids', [])
    opposite = book.get('no_bids' if side == 'yes' else 'yes_bids', [])
    if not own or not opposite:
        return None
    try:
        bid, bid_size = max(own, key=lambda x: x[0]); no_bid, ask_size = max(opposite, key=lambda x: x[0])
        ask = 1 - no_bid
        if not all(number(v) for v in (bid, bid_size, ask, ask_size)) or not 0 < bid < ask < 1:
            return None
        return bid, ask, bid_size, ask_size
    except (ValueError, TypeError, IndexError):
        return None


def barrier_label(path, start, side, entry, horizon=1200, max_gap=90):
    """Quote opportunity label, not a fill claim. Gaps/empty depth censor, never count as losses."""
    first = path[start][0]
    stop, target = .30 * entry, min(.99, 2 * entry)
    previous = first
    for ts, book in path[start+1:]:
        if ts > first + horizon + max_gap or ts - previous > max_gap:
            return None
        q = top(book, side)
        if q is None or q[2] < 1:
            return None
        bid = q[0]
        # Use the first later quote. If the deadline has passed, no later barrier gets credit.
        if ts > first + horizon:
            return 0
        if bid <= stop:
            return 0
        if bid >= target:
            return 1
        previous = ts
    return None


def sports_rows(c):
    rows, censored = [], 0
    markets = c.execute("""SELECT DISTINCT m.ticker,m.event_ticker FROM markets m JOIN series s ON s.ticker=m.series_ticker
                            WHERE lower(s.category) LIKE '%sport%' AND EXISTS
                            (SELECT 1 FROM observations o WHERE o.source='kalshi.orderbook' AND o.subject=m.ticker)""").fetchall()
    used = set()
    for m in markets:
        path = []
        for r in c.execute("SELECT received_ts,payload FROM observations WHERE source='kalshi.orderbook' AND subject=? ORDER BY received_ts,id", (m['ticker'],)):
            try: path.append((r['received_ts'], json.loads(r['payload'])))
            except (ValueError, TypeError): pass
        # A feed-aligned game state is required. No inference that a game has started from its name.
        states = c.execute("SELECT ts,state FROM ext_wp WHERE event_ticker=? ORDER BY ts", (m['event_ticker'],)).fetchall()
        state_times = [r['ts'] for r in states]
        for i, (ts, book) in enumerate(path):
            si = bisect.bisect_right(state_times, ts) - 1
            if si < 0 or ts - state_times[si] > 120 or states[si]['state'] not in ('in', 'halftime'):
                continue
            for side in ('yes','no'):
                q = top(book, side)
                if q is None or q[3] < 1 or not .05 <= q[1] <= .25 or q[1] - q[0] > .05:
                    continue
                key = (m['event_ticker'], side, bucket(q[1]))
                if key in used: continue
                used.add(key)  # do not reach forward for a better/fully observed entry later
                label = barrier_label(path, i, side, q[1])
                if label is None: censored += 1; continue
                rows.append({'group': m['event_ticker'], 'day': dt.datetime.fromtimestamp(ts,dt.timezone.utc).date().isoformat(),
                             # The 50% target-hit prior is a deliberately neutral research prior,
                             # not the settlement probability or a claim of an efficient-market price.
                             'p': q[1], 'baseline': .5, 'y': label})
    return rows, censored


def execution_rows(c, asof):
    rows, censored, attribution = [], 0, {'orders': 0, 'filled_contracts': 0., 'entry_fees': 0., 'entry_slippage_dollars': 0.,
                                        'markout_60s_dollars': 0., 'markout_contracts': 0.}
    orders = c.execute("""SELECT o.*,p.ts AS decision_ts,p.price AS proposed_price,p.exec_style
                           FROM orders o JOIN proposals p ON p.id=o.proposal_id
                           WHERE p.strategy NOT IN ('sys_exit','exit','close') ORDER BY o.created_time,o.order_id""").fetchall()
    for o in orders:
        start = timestamp(o['created_time'])
        if not start or o['decision_ts'] > start or start + 60 > asof or o['initial_count'] <= 0:
            continue
        fills = c.execute("SELECT * FROM fills WHERE order_id=? ORDER BY ts", (o['order_id'],)).fetchall()
        # Actual fills of OUR order, not some later public print.
        filled = sum(f['count'] for f in fills if start <= f['ts'] <= start + 60)
        end = timestamp(o['last_update_time'])
        if filled + 1e-8 < o['initial_count'] and (o['status'] not in ('executed','canceled','done') or end < start + 60):
            censored += 1; continue
        if o['exec_style'] != 'maker': continue
        price = o['proposed_price']
        if not number(price) or not 0 < price < 1: continue
        day = dt.datetime.fromtimestamp(start,dt.timezone.utc).date().isoformat()
        rows.append({'group':o['order_id'],'day':day,'p':price,'baseline':.5,'y':int(filled+1e-8 >= o['initial_count'])})
        attribution['orders'] += 1
        for f in fills:
            px = f['yes_price'] if o['outcome_side'] == 'yes' else f['no_price']
            attribution['filled_contracts'] += f['count'];attribution['entry_fees'] += f['fee'] or 0
            attribution['entry_slippage_dollars'] += f['count'] * (px - price)
            qr = c.execute("SELECT yes_bid,no_bid FROM quotes WHERE ticker=? AND ts>=? AND ts<=? ORDER BY ts,id LIMIT 1",
                           (o['ticker'], f['ts']+60, f['ts']+70)).fetchone()
            if qr:
                bid = qr['yes_bid'] if o['outcome_side']=='yes' else qr['no_bid']
                if number(bid) and 0 < bid < 1:
                    attribution['markout_60s_dollars'] += f['count']*(bid-px)
                    attribution['markout_contracts'] += f['count']
    return rows,censored,attribution


def conditional_report(name, objective, rows):
    # For barrier/fill outcomes a contract's price is a FEATURE, not the target probability.
    # Fit Beta(1,1) empirical hit rates by entry-price decile; compare to train-only base rate.
    days = sorted({r['day'] for r in rows}); cutoff = max(1,int(len(days)*.7))
    train_days=set(days[:cutoff]);train=[r for r in rows if r['day'] in train_days];test=[r for r in rows if r['day'] not in train_days]
    def fitted(sample):
        groups={}
        for r in sample: groups[r['group']]=groups.get(r['group'],0)+1
        out=[]
        for i in range(10):
            rs=[r for r in sample if bucket(r['p'])==i];n=sum(1/groups[r['group']] for r in rs);wins=sum(r['y']/groups[r['group']] for r in rs)
            out.append({'p':(1+wins)/(2+n),'event_weight':n,'groups':len({r['group'] for r in rs})})
        return out
    params=fitted(train);group_rates={}
    for r in train: group_rates.setdefault(r['group'],[]).append(r['y'])
    baseline=(1+sum(sum(v)/len(v) for v in group_rates.values()))/(2+len(group_rates))
    metrics=evaluate([{**r,'baseline':baseline} for r in test],lambda r:params[bucket(r['p'])]['p'])
    return {'name':name,'objective':objective,'status':'insufficient_independent_data' if len(group_rates)<200 or len(train_days)<20 or metrics['groups']<50 else 'shadow_validation',
            'live_enabled':False,'rows':len(rows),'groups':len({r['group'] for r in rows}),'days':len(days),
            'train_groups':len(group_rates),'train_days':sorted(train_days),'holdout_days':days[cutoff:],
            'holdout':metrics,'validation_parameters':params,'prospective_parameters':fitted(rows),
            'notice':'Conditional empirical baseline only. Price decile is the feature. Game state, depth and timing need richer models and prospective validation.'}


def run(database, output):
    asof=int(time.time());c=sqlite3.connect(database.resolve().as_uri()+'?mode=ro',uri=True,timeout=10);c.row_factory=sqlite3.Row
    c.execute('PRAGMA query_only=ON');c.execute('BEGIN')  # consistent WAL snapshot throughout the study
    required={'observations','ext_wp','model_fairs','markets','series','orders','proposals','fills','quotes'}
    missing=required-{r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    if missing:raise RuntimeError('Missing research tables: '+', '.join(sorted(missing)))
    weather=train_report('weather_calibration','Settlement probability calibration of recorded NWS contract forecasts',weather_rows(c))
    weather['limitations']=['Only final market results retained; historical result publication times and model versions were not recorded.',
                            'Independent contract calibration can violate event probability sums; never deploy these bins directly as live weather fair values.',
                            'Exact station, local climate day and settlement-source calibration still need a versioned temperature model.']
    swings,censored=sports_rows(c)
    sports=conditional_report('sports_swing','Chance of a displayed bid reaching 2x entry ask before a 70% stop or 20 minutes',swings)
    sports.update(censored=censored,study_kind='quote_opportunity',limitations=['Quote targets are not fills; entry/exit latency, queue position and fees are not simulated.',
        'First eligible observation per event/side/price bucket; incomplete paths are censored. Censoring may be informative.',
        'Only games with a contemporaneous recorded ESPN state are eligible. No overtime premium is assumed.'])
    executions,censored,attribution=execution_rows(c,asof)
    execution=conditional_report('maker_fill','Chance OUR maker order fully fills within 60 seconds',executions)
    execution.update(censored=censored,attribution=attribution,limitations=['Early cancellations and unknown completion times are censored; this is conditional on the existing entry/cancel policy.',
        'Price-only empirical baseline; insufficient to optimize queue priority or chase an unfilled order.',
        'Markout uses a later bid, not a realized exit. Positive slippage dollars means worse purchase cost than the proposal.'])
    commodities={'name':'commodity_interval','objective':'Exact Pyth underlying distribution over the contract reference window',
                 'status':'missing_underlying_history','live_enabled':False,'rows':0,'groups':0,
                 'notice':'Order-book prices alone cannot fit the underlying model. Needs exact contract-to-feed mapping, Pyth source timestamps, window/reference convention and a validated volatility estimate.'}
    counts={r['source']:r['n'] for r in c.execute('SELECT source,COUNT(*) n FROM observations GROUP BY source')}
    report={'schema':VERSION,'generated_ts':asof,'live_enabled':False,'models':[weather,sports,execution,commodities],
            'observation_counts':counts,'notice':'Offline research only. No orders, account settings, live fairs, or 15-minute crypto logic are changed.',
            'sources':['https://www.weather.gov/documentation/services-web-api','https://docs.kalshi.com/websockets/pyth-value']}
    c.rollback();c.close()
    report['version']=hashlib.sha256(json.dumps(report,sort_keys=True).encode()).hexdigest()[:16]
    output.parent.mkdir(parents=True,exist_ok=True)
    temp=output.with_suffix('.tmp');temp.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n');temp.replace(output)
    print('Research report: '+str(output.resolve()))
    for model in report['models']:print(model['name']+': '+model['status']+'; independent groups '+str(model['groups']))
    return report


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--db',type=Path,required=True);ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args();run(args.db,args.output)
