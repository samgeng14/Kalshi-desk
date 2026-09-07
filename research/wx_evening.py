#!/usr/bin/env python3
"""WX-1d: does the same-day weather model beat the tape AFTER the daily peak?

WX-1b tested hours 9-16 and found the market better at every one of them, which is why
same-day weather is research-only. It never tested the evening. That matters, because the
question changes character once the peak has passed: before it, the model is forecasting how
much further the temperature will rise; after it, the day's maximum is already an observed
number and the only uncertainty left is the sensor shift between the hourly METAR max and the
climate report Kalshi settles on (wx-settle: +0 63%, +1 31%, +2 6%).

Fit: rise-from-running-max climatology on 2022-2025 (station, month +-1, hour, trend bucket).
Score: every settled 2026 market in the research DB, hour by hour, out of sample.
Execution: decision on the last print at or before h:00; fill at the NEXT print on the side we
would take, plus the 7% taker fee. No fill assumed when that side never prints again.
"""
import bisect, collections, datetime as dt, json, math, os, random, sqlite3, sys
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'desk.sqlite')
STATIONS = {'KXHIGHNY': ('KNYC', 'America/New_York'), 'KXHIGHCHI': ('KMDW', 'America/Chicago'),
            'KXHIGHLAX': ('KLAX', 'America/Los_Angeles'), 'KXHIGHMIA': ('KMIA', 'America/New_York'),
            'KXHIGHAUS': ('KAUS', 'America/Chicago'), 'KXHIGHPHIL': ('KPHL', 'America/New_York'),
            'KXDENHIGH': ('KDEN', 'America/Denver'), 'KXHIGHHOU': ('KIAH', 'America/Chicago')}
SHIFT = [(0, 0.63), (1, 0.31), (2, 0.06)]
HOURS = list(range(9, 24))
FIT_YEARS = {2022, 2023, 2024, 2025}
EDGE = 0.08


def load_obs(station):
    days = collections.defaultdict(list)
    path = os.path.join(HERE, f'asos_{station}.csv')
    if not os.path.exists(path):
        return days
    with open(path) as f:
        for line in f:
            if line.startswith('#') or line.startswith('station'):
                continue
            parts = line.strip().split(',')
            if len(parts) < 3 or parts[2] == 'M':
                continue
            d, hm = parts[1].split(' ')
            days[d].append((int(hm[:2]) * 60 + int(hm[3:]), float(parts[2])))
    for d in days:
        days[d].sort()
    return days


def day_state(obs, hour):
    """Running max, latest temp and 1-hour trend at local hour:00."""
    cut = hour * 60
    rmax, tnow, last_m, prev = -999, -999, -1, None
    for m, t in obs:
        if m > cut:
            break
        rmax = max(rmax, t)
        tnow, last_m = t, m
        if m <= cut - 60:
            prev = t
    if last_m < 0 or last_m < cut - 90:
        return None
    return rmax, tnow, (tnow - prev if prev is not None else 0.0)


def tbucket(x):
    return -1 if x <= -1 else (1 if x >= 2 else 0)


def bracket_hit(stype, lo, hi, t):
    if stype == 'less':
        return t < hi
    if stype == 'greater':
        return t > lo
    return lo <= t <= hi


def fee_taker(p):
    return math.ceil(0.07 * p * (1 - p) * 10000) / 10000


def boot(xs, n=2000):
    if not xs:
        return (0.0, 0.0, 0.0)
    mean = sum(xs) / len(xs)
    means = []
    for _ in range(n):
        means.append(sum(random.choice(xs) for _ in xs) / len(xs))
    means.sort()
    return (mean, means[int(0.05 * n)], means[int(0.95 * n)])


def main():
    random.seed(7)
    obs = {st: load_obs(st) for st, _ in STATIONS.values()}
    tables = collections.defaultdict(collections.Counter)
    for series, (station, _tz) in STATIONS.items():
        for d, day in obs[station].items():
            y, mo = int(d[:4]), int(d[5:7])
            if y not in FIT_YEARS or len(day) < 12:
                continue
            final = max(t for _, t in day)
            for h in HOURS:
                s = day_state(day, h)
                if s is None:
                    continue
                rmax, _tnow, trend = s
                rise = int(round(final - rmax))
                for mm in (mo - 1, mo, mo + 1):
                    mw = (mm - 1) % 12 + 1
                    tables[(station, mw, h, tbucket(trend))][rise] += 1
                    tables[(station, mw, h, None)][rise] += 1

    con = sqlite3.connect(f'file:{DB}?mode=ro', uri=True)
    markets = con.execute(
        "SELECT ticker,event_ticker,series_ticker,strike_type,floor_strike,cap_strike,result "
        "FROM hist_markets WHERE series_ticker IN (%s) AND result IN ('yes','no')" % ','.join('?' * len(STATIONS)),
        list(STATIONS)).fetchall()
    trades = collections.defaultdict(list)   # ticker -> [(ts, yes_price, taker_side)]
    for t, ts, px, side in con.execute(
            "SELECT ticker, ts, yes_price, taker_side FROM hist_trades WHERE ticker LIKE 'KXHIGH%' OR ticker LIKE 'KXDENHIGH%'"):
        trades[t].append((ts, px, side))
    for t in trades:
        trades[t].sort()
    print(f'{len(markets)} settled markets, {sum(len(v) for v in trades.values())} prints on {len(trades)} of them')

    rows = collections.defaultdict(list)
    for ticker, ev, series, stype, lo, hi, result in markets:
        station, tz = STATIONS[series]
        date = ev.split('-')[1]
        d = dt.datetime.strptime(date, '%y%b%d').date()
        day = obs[station].get(d.isoformat())
        if not day or len(day) < 12:
            continue
        outcome = 1 if result == 'yes' else 0
        tl = trades.get(ticker, [])
        for h in HOURS:
            s = day_state(day, h)
            if s is None:
                continue
            rmax, _tnow, trend = s
            cell = tables.get((station, d.month, h, tbucket(trend)))
            if not cell or sum(cell.values()) < 60:
                cell = tables.get((station, d.month, h, None))
            if not cell:
                continue
            n = sum(cell.values())
            pm = 0.0
            for rise, cnt in cell.items():
                for k, w in SHIFT:
                    if bracket_hit(stype, lo, hi, int(round(rmax)) + rise + k):
                        pm += w * cnt / n
            t_dec = int(dt.datetime(d.year, d.month, d.day, h, 0, tzinfo=ZoneInfo(tz)).timestamp())
            i = bisect.bisect_right(tl, (t_dec, 9, 'z'))
            pk = tl[i - 1][1] if i > 0 and t_dec - tl[i - 1][0] <= 3 * 3600 else None
            nxt_yes = nxt_no = None
            for ts, px, side in tl[i:]:
                if ts > t_dec + 3 * 3600:
                    break
                if side == 'yes' and nxt_yes is None:
                    nxt_yes = px
                if side == 'no' and nxt_no is None:
                    nxt_no = 1 - px
                if nxt_yes is not None and nxt_no is not None:
                    break
            rows[h].append((pm, pk, outcome, nxt_yes, nxt_no, rmax))

    print()
    print(f'{"hr":>3} {"n":>5} {"n_tape":>6} | {"Brier model":>11} {"market":>8} {"blend":>7} | '
          f'rule |model-mkt|>={EDGE:.2f}, filled at the next print on our side, taker fee')
    for h in HOURS:
        r = rows[h]
        if not r:
            continue
        with_tape = [x for x in r if x[1] is not None]
        bm_all = sum((pm - o) ** 2 for pm, _pk, o, *_ in r) / len(r)
        if not with_tape:
            print(f'{h:>3} {len(r):>5} {0:>6} | {bm_all:>11.4f}      no tape')
            continue
        bm = sum((pm - o) ** 2 for pm, pk, o, *_ in with_tape) / len(with_tape)
        bk = sum((pk - o) ** 2 for pm, pk, o, *_ in with_tape) / len(with_tape)
        bb = sum(((pm + pk) / 2 - o) ** 2 for pm, pk, o, *_ in with_tape) / len(with_tape)
        ret = []
        for pm, pk, o, ny, nn, _rmax in with_tape:
            if pk < 0.03 or pk > 0.97 or abs(pm - pk) < EDGE:
                continue
            buy_yes = pm > pk
            px = ny if buy_yes else nn
            if px is None or px <= 0 or px >= 1:
                continue
            ret.append((o if buy_yes else 1 - o) - px - fee_taker(px))
        m, lo90, hi90 = boot(ret)
        better = 'MODEL' if bm < bk - 1e-9 else 'market'
        print(f'{h:>3} {len(r):>5} {len(with_tape):>6} | {bm:>11.4f} {bk:>8.4f} {bb:>7.4f} | '
              f'{better:>6} better · trades {len(ret):>4} ret {m:+.3f} [{lo90:+.3f}, {hi90:+.3f}]')
    print()
    print('A lower Brier is better. WX-1b only ever printed hours 9-16.')


if __name__ == '__main__':
    main()
