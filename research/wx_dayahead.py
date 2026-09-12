#!/usr/bin/env python3
"""WX-1c: does the DAY-AHEAD weather model beat the tape?

WX-1b (and WX-1d, hours 9-23) tested the same day and the market won at every hour. The desk
still trades weather a day out in manual mode, and that regime has never been measured. This is
the test that decides whether it should.

Model, exactly as src/strategies/weather_nws.cpp prices a day-ahead market:
    mu = day-ahead forecast max + bias_f,  sd = sigma_f,  no observations, no sensor shift.
Forecast: Open-Meteo previous-runs archive, the run issued one day before the target date
(temperature_2m_previous_day1), hourly max over the local day.
Market: the last print at or before the decision moment on D-1.
Execution: the next print on the side we would take, plus the 7% taker fee.
\nExecution: NONE. PREDICTION ONLY, for the same reason as wx_evening.py - fills were\ntaken from prints hours after the decision. See research/execution.py.\n"""
import bisect, collections, csv, datetime as dt, math, os, random, sqlite3
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'desk.sqlite')
TZ = {'KXHIGHNY': 'America/New_York', 'KXHIGHCHI': 'America/Chicago', 'KXHIGHLAX': 'America/Los_Angeles',
      'KXHIGHMIA': 'America/New_York', 'KXHIGHAUS': 'America/Chicago', 'KXHIGHPHIL': 'America/New_York'}
BIAS, SIGMA = 0.4, 2.5
DECISIONS = [('D-1 12:00', -1, 12), ('D-1 18:00', -1, 18), ('D-1 21:00', -1, 21), ('D 06:00', 0, 6)]


def norm_cdf(x):
    return 0.5 * math.erfc(-x / math.sqrt(2))


def bracket_prob(stype, lo, hi, mu, sd):
    a, b = -math.inf, math.inf
    if stype == 'less':
        b = hi - 0.5
    elif stype == 'greater':
        a = lo + 0.5
    else:
        a, b = lo - 0.5, hi + 0.5
    cdf = lambda t: (0.0 if t < 0 else 1.0) if math.isinf(t) else norm_cdf((t - mu) / sd)
    lo_p = 0.0 if math.isinf(a) and a < 0 else cdf(a)
    hi_p = 1.0 if math.isinf(b) and b > 0 else cdf(b)
    return max(0.0, min(1.0, hi_p - lo_p))


def fee_taker(p):
    return math.ceil(0.07 * p * (1 - p) * 10000) / 10000


def boot(xs, n=2000):
    if not xs:
        return (0.0, 0.0, 0.0)
    random.seed(11)
    m = sum(xs) / len(xs)
    ms = sorted(sum(random.choice(xs) for _ in xs) / len(xs) for _ in range(n))
    return (m, ms[int(0.05 * n)], ms[int(0.95 * n)])


def main():
    fc = {}
    with open(os.path.join(HERE, 'om_dayahead.csv')) as f:
        for r in csv.DictReader(f):
            if r['fc_d1_max']:
                fc[(r['series'], r['date'])] = float(r['fc_d1_max'])
    print(f'{len(fc)} day-ahead station-days of archived forecast')
    con = sqlite3.connect(f'file:{DB}?mode=ro', uri=True)
    markets = con.execute(
        "SELECT ticker,event_ticker,series_ticker,strike_type,floor_strike,cap_strike,result FROM hist_markets "
        "WHERE series_ticker IN (%s) AND result IN ('yes','no')" % ','.join('?' * len(TZ)), list(TZ)).fetchall()
    trades = collections.defaultdict(list)
    for t, ts, px, side in con.execute(
            "SELECT ticker, ts, yes_price, taker_side FROM hist_trades WHERE ticker LIKE 'KXHIGH%'"):
        trades[t].append((ts, px, side))
    for t in trades:
        trades[t].sort()

    rows = collections.defaultdict(list)
    used = 0
    for ticker, ev, series, stype, lo, hi, result in markets:
        date = ev.split('-')[1]
        d = dt.datetime.strptime(date, '%y%b%d').date()
        f = fc.get((series, d.isoformat()))
        if f is None:
            continue
        used += 1
        outcome = 1 if result == 'yes' else 0
        pm = bracket_prob(stype, lo or 0, hi or 0, f + BIAS, SIGMA)
        tl = trades.get(ticker, [])
        for label, off, hour in DECISIONS:
            when = dt.datetime.combine(d + dt.timedelta(days=off), dt.time(hour), tzinfo=ZoneInfo(TZ[series]))
            ts0 = int(when.timestamp())
            i = bisect.bisect_right(tl, (ts0, 9, 'z'))
            pk = tl[i - 1][1] if i > 0 and ts0 - tl[i - 1][0] <= 6 * 3600 else None
            ny = nn = None
            for ts, px, side in tl[i:]:
                if ts > ts0 + 6 * 3600:
                    break
                if side == 'yes' and ny is None:
                    ny = px
                if side == 'no' and nn is None:
                    nn = 1 - px
                if ny is not None and nn is not None:
                    break
            rows[label].append((pm, pk, outcome, ny, nn))
    print(f'{used} settled markets covered by the archive\n')
    for edge in (0.05, 0.08, 0.12):
        print(f'--- edge threshold {edge:.2f} ---')
        print(f'{"decision":<10} {"n":>5} {"n_tape":>6} | {"model":>7} {"market":>7} {"blend":>7} | '
              f'{"who":>6} | trades  ret/$ [90% CI]')
        for label, _o, _h in DECISIONS:
            r = rows[label]
            wt = [x for x in r if x[1] is not None]
            if not wt:
                print(f'{label:<10} {len(r):>5} {0:>6} | no tape')
                continue
            bm = sum((pm - o) ** 2 for pm, pk, o, *_ in wt) / len(wt)
            bk = sum((pk - o) ** 2 for pm, pk, o, *_ in wt) / len(wt)
            bb = sum(((pm + pk) / 2 - o) ** 2 for pm, pk, o, *_ in wt) / len(wt)
            ret = []
            for pm, pk, o, ny, nn in wt:
                if pk < 0.03 or pk > 0.97 or abs(pm - pk) < edge:
                    continue
                buy_yes = pm > pk
                px = ny if buy_yes else nn
                if px is None or px <= 0 or px >= 1:
                    continue
                ret.append((o if buy_yes else 1 - o) - px - fee_taker(px))
            m, l, h = boot(ret)
            print(f'{label:<10} {len(r):>5} {len(wt):>6} | {bm:>7.4f} {bk:>7.4f} {bb:>7.4f} | '
                  f'{"MODEL" if bm < bk else "market":>6} | {len(ret):>5}  {m:+.3f} [{l:+.3f}, {h:+.3f}]')
        print()


if __name__ == '__main__':
    main()
