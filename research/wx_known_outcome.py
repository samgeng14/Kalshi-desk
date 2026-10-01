#!/usr/bin/env python3
"""Weather known-outcome lag (M7, research): is the Kalshi book slow to price a bracket the
station's running maximum has already decided?

The daily-high contracts settle on the NWS climate report's maximum. During the day the desk
polls the station's observations and records the running maximum (ext_weather.obs_max). That
running maximum is a LOWER bound on the settlement value: the report's maximum comes from finer
readings than the ones polled, so it can only be equal or higher. (KXHIGHNY 2026-09-15: running
max 71.06F, settled in the 72-73 bracket.) So once it clears an edge the running max proves two
things: every bracket whose top is below it is NO, and every "above X" bracket whose floor is below
it is YES. Nothing in between is decided before the day ends.

The rules tested: a bracket with an upper edge `cap` (a "less than" or "between" contract) is decided
NO once running_max >= cap + 1 + margin; an "above" bracket with floor `f` is decided YES once
running_max >= f + 1 + margin. For each decision:

  1. Safety. How often did the decided bracket settle YES anyway? It must be never; a single
     case means the margin is too small.
  2. Lag. The bracket's YES bid when the decision was first recorded, and 5, 15 and 60 minutes
     later. A YES bid above a cent or two after the outcome is known is money a seller of YES
     collects at settlement, less the fee: that is the lane A "known outcome" trade.

Resolution: the desk polled observations about every cycle but NWS stations publish hourly (with
specials), and the Kalshi book was logged on change or once a minute. The decision time here is
when the DESK first saw the running max cross, which is later than the station's reading.

  python3 research/wx_known_outcome.py --extract     # desk stopped
  python3 research/wx_known_outcome.py
"""
import argparse, bisect, math, os, sqlite3, statistics, sys, time
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from espn_leadlag import take_desk_lock  # noqa: E402  same lock rule for every extract

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIVE = os.path.join(ROOT, 'data', 'prod.sqlite')
EXTRACT = os.path.join(ROOT, 'data', 'research', 'wx_known_outcome.sqlite')
REPORT = os.path.join(ROOT, 'reports', 'wx_known_outcome.md')


def extract(live, out):
    lock = take_desk_lock(live)   # held for the whole read
    if lock is None:
        sys.exit('the desk is running on this database; stop it first (scripts/tradeit down)')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    src = sqlite3.connect(f'file:{live}?mode=ro', uri=True)
    dst = sqlite3.connect(out)
    dst.execute('CREATE TABLE ext_weather(ts, series, date, obs_max, have_obs)')
    dst.executemany('INSERT INTO ext_weather VALUES(?,?,?,?,?)',
                    src.execute('SELECT ts, series, date, obs_max, have_obs FROM ext_weather WHERE have_obs=1').fetchall())
    series = [r[0] for r in dst.execute('SELECT DISTINCT series FROM ext_weather')]
    dst.execute('CREATE TABLE markets(ticker, event_ticker, series_ticker, strike_type, floor_strike, cap_strike, result)')
    dst.execute('CREATE TABLE quotes(ts, ticker, yes_bid, yes_ask)')
    n_q = 0
    for s in series:
        ms = src.execute('SELECT ticker, event_ticker, series_ticker, strike_type, floor_strike, cap_strike, result '
                         'FROM markets WHERE series_ticker=?', (s,)).fetchall()
        dst.executemany('INSERT INTO markets VALUES(?,?,?,?,?,?,?)', ms)
        for m in ms:
            q = src.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes WHERE ticker=? ORDER BY ts', (m[0],)).fetchall()
            dst.executemany('INSERT INTO quotes VALUES(?,?,?,?)', q)
            n_q += len(q)
    dst.commit()
    src.close()
    os.close(lock)
    print(f'extracted {len(series)} series, {n_q:,} quotes -> {out}')


def fmt(x, d=2):
    return '-' if x is None or (isinstance(x, float) and math.isnan(x)) else f'{x:.{d}f}'


def analyse(path, report, margin):
    db = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    obs = defaultdict(list)   # (series, date) -> [(ts, running max)]
    for ts, s, d, mx in db.execute('SELECT ts, series, date, obs_max FROM ext_weather ORDER BY ts'):
        if mx is not None and mx > -100:
            obs[(s, d)].append((ts, mx))
    quotes = defaultdict(lambda: ([], []))
    for ts, t, b, a in db.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes ORDER BY ticker, ts'):
        quotes[t][0].append(ts); quotes[t][1].append(b or 0.0)

    def bid_at(t, when, max_age=900):
        xs, bs = quotes.get(t, ([], []))
        i = bisect.bisect_right(xs, when) - 1
        if i < 0 or when - xs[i] > max_age:
            return None
        return bs[i]

    kalshi_months = {m: i + 1 for i, m in enumerate(['JAN','FEB','MAR','APR','MAY','JUN','JUL','AUG','SEP','OCT','NOV','DEC'])}

    def event_date(ev):
        code = ev.split('-')[1][:7]
        return f'20{code[:2]}-{kalshi_months.get(code[2:5], 0):02d}-{code[5:7]}'

    # The YES ask, for the "above" brackets the running max decides YES (buy YES under $1).
    asks = defaultdict(lambda: ([], []))
    for ts, t, b, a in db.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes ORDER BY ticker, ts'):
        asks[t][0].append(ts); asks[t][1].append(a or 0.0)

    def ask_at(t, when, max_age=900):
        xs, ys = asks.get(t, ([], []))
        i = bisect.bisect_right(xs, when) - 1
        if i < 0 or when - xs[i] > max_age:
            return None
        return ys[i]

    yes_decisions = []
    for t, ev, s, lo, res in db.execute(
            "SELECT ticker, event_ticker, series_ticker, floor_strike, result FROM markets "
            "WHERE strike_type='greater' AND floor_strike IS NOT NULL AND result IN ('yes','no')"):
        series_obs = obs.get((s, event_date(ev)))
        if not series_obs:
            continue
        crossed = next((ts for ts, mx in series_obs if mx >= lo + 1 + margin), None)
        if crossed is None:
            continue
        yes_decisions.append({'ticker': t, 'series': s, 'result': res, 'floor': lo, 'ts': crossed,
                              'asks': {h: ask_at(t, crossed + 60 * h) for h in (0, 5, 15, 60)}})

    decisions = []
    for t, ev, s, st, lo, cap, res in db.execute(
            "SELECT ticker, event_ticker, series_ticker, strike_type, floor_strike, cap_strike, result FROM markets "
            "WHERE strike_type IN ('less','between') AND cap_strike IS NOT NULL AND result IN ('yes','no')"):
        series_obs = obs.get((s, event_date(ev)))
        if not series_obs:
            continue
        crossed = next((ts for ts, mx in series_obs if mx >= cap + 1 + margin), None)
        if crossed is None:
            continue
        rec = {'ticker': t, 'series': s, 'result': res, 'cap': cap, 'ts': crossed, 'bids': {}}
        for h in (0, 5, 15, 60):
            rec['bids'][h] = bid_at(t, crossed + 60 * h)
        decisions.append(rec)

    lines = []
    w = lines.append
    w('# Weather known-outcome lag: brackets the running maximum has already decided')
    w('')
    w(f'Generated {time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())} by `research/wx_known_outcome.py` from the '
      f'desk\'s recordings. Rules: a bracket with upper edge `cap` is decided NO once the recorded running maximum is '
      f'at least cap + 1 + {margin:g}F; an "above" bracket with floor `f` is decided YES once it is at least f + 1 + '
      f'{margin:g}F. The running maximum is a lower bound on the settlement value, so these are the two things it can '
      'decide.')
    w('')
    wrong = [d for d in decisions if d['result'] == 'yes']
    w('## 1. Safety')
    w('')
    w(f'{len(decisions)} bracket-days decided NO by the rule; {len(wrong)} of them settled YES.')
    for d in wrong[:10]:
        w(f'- {d["ticker"]} settled YES although the running max passed {d["cap"] + 1 + margin:g}F')
    w('')
    w('## 2. The YES bid after the outcome was known')
    w('')
    w('| minutes after the desk saw it | n with a quote | median YES bid | share with a bid >= 0.02 | share >= 0.05 | mean bid |')
    w('|---|---|---|---|---|---|')
    for h in (0, 5, 15, 60):
        bs = [d['bids'][h] for d in decisions if d['bids'][h] is not None and d['result'] == 'no']
        if not bs:
            w(f'| {h} | 0 | - | - | - | - |')
            continue
        w(f'| {h} | {len(bs)} | {fmt(statistics.median(bs))} | {fmt(100 * sum(b >= 0.02 for b in bs) / len(bs), 0)}% | '
          f'{fmt(100 * sum(b >= 0.05 for b in bs) / len(bs), 0)}% | {fmt(statistics.fmean(bs), 3)} |')
    w('')
    at0 = [d['bids'][0] for d in decisions if d['bids'][0] is not None and d['result'] == 'no']
    # Selling YES at the bid b on a contract that settles NO earns b less the taker fee on b.
    fee = lambda b: math.ceil(100 * 0.07 * b * (1 - b) - 1e-9) / 100
    net = [b - fee(b) for b in at0 if b > 0]
    per_series = defaultdict(list)
    for d in decisions:
        if d['bids'][0] is not None and d['result'] == 'no':
            per_series[d['series']].append(d['bids'][0])
    if per_series:
        w('By series, the YES bid when the desk first saw the bracket decided:')
        w('')
        w('| series | n | median bid | max bid |')
        w('|---|---|---|---|')
        for s, bs in sorted(per_series.items()):
            w(f'| {s} | {len(bs)} | {fmt(statistics.median(bs))} | {fmt(max(bs))} |')
        w('')
    w('## 3. The "above" brackets the running max decides YES')
    w('')
    wrong_yes = [d for d in yes_decisions if d['result'] == 'no']
    w(f'{len(yes_decisions)} bracket-days decided YES; {len(wrong_yes)} of them settled NO.')
    w('')
    w('| minutes after the desk saw it | n with a quote | median YES ask | share with an ask <= 0.97 | mean ask |')
    w('|---|---|---|---|---|')
    for h in (0, 5, 15, 60):
        xs = [d['asks'][h] for d in yes_decisions if d['asks'][h] is not None and 0 < d['asks'][h] <= 1 and d['result'] == 'yes']
        if not xs:
            w(f'| {h} | 0 | - | - | - |')
            continue
        w(f'| {h} | {len(xs)} | {fmt(statistics.median(xs))} | {fmt(100 * sum(x <= 0.97 for x in xs) / len(xs), 0)}% | '
          f'{fmt(statistics.fmean(xs), 3)} |')
    w('')
    buy = [1 - a - fee(a) for a in (d['asks'][0] for d in yes_decisions if d['result'] == 'yes') if a is not None and 0 < a < 1]
    w(f'Buying YES at the ask when the desk first saw it: {len(buy)} with an ask under $1, netting '
      f'${fmt(sum(x for x in buy if x > 0))} in total after the taker fee on the ones that paid anything '
      f'({sum(1 for x in buy if x > 0)} of them).')
    w('')
    w(f'**Reading (both directions).** {len(net)} decided-NO brackets still showed a YES bid above zero when the desk first saw the '
      f'running max cross; selling YES into those bids nets ${fmt(sum(net))} in total per contract-set after the taker '
      f'fee (${fmt(statistics.fmean(net) if net else float("nan"), 3)} each). '
      + ('The rule was never wrong on this sample. ' if not wrong else f'The rule was wrong {len(wrong)} times: widen the margin before anything trades on it. ')
      + 'A bid of one cent is the book\'s floor and pays nothing after the fee; the question is how often it is '
      'more than that, which the table answers. These are one contract each: depth is not recorded here.')
    os.makedirs(os.path.dirname(report), exist_ok=True)
    with open(report, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', action='store_true')
    ap.add_argument('--live', default=LIVE)
    ap.add_argument('--db', default=EXTRACT)
    ap.add_argument('--report', default=REPORT)
    ap.add_argument('--margin', type=float, default=0.5, help='degrees F past cap+1 before a bracket counts as decided')
    a = ap.parse_args()
    if a.extract:
        extract(a.live, a.db)
        return
    if not os.path.exists(a.db):
        sys.exit(f'no extract at {a.db}; run with --extract while the desk is stopped')
    analyse(a.db, a.report, a.margin)


if __name__ == '__main__':
    main()
