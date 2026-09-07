#!/usr/bin/env python3
"""WX-1b research: same-day high-temperature brackets from the intraday climatology of the
remaining rise, versus the Kalshi tape.

Inputs: research/asos_<station>.csv (IEM ASOS hourly+special obs, local time, tmpf) and the
research DB (hist_markets, hist_trades for the KXHIGH* series).

Model at local decision hour h on day d:
  R_h      = running METAR max through h:00
  T_now    = last obs at or before h:00
  rise     = final METAR max of the day - R_h  (>= 0)
  settle   = final METAR max + shift, shift ~ {0: .63, 1: .31, 2: .06}  (wx-settle study)
  P(rise = k | station, month window, hour, trend bucket) from 2022-2025 (out of sample for 2026).
Outputs: Brier per hour (model vs market vs blend), simulated returns of trading the tape.
"""
import bisect, collections, csv, datetime as dt, json, math, os, sqlite3, sys
from zoneinfo import ZoneInfo

HERE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(HERE, '..', 'data', 'desk.sqlite')
STATIONS = {'KXHIGHNY': ('KNYC', 'America/New_York'), 'KXHIGHCHI': ('KMDW', 'America/Chicago'),
            'KXHIGHLAX': ('KLAX', 'America/Los_Angeles'), 'KXHIGHMIA': ('KMIA', 'America/New_York'),
            'KXHIGHAUS': ('KAUS', 'America/Chicago'), 'KXHIGHPHIL': ('KPHL', 'America/New_York'),
            'KXDENHIGH': ('KDEN', 'America/Denver'), 'KXHIGHHOU': ('KIAH', 'America/Chicago')}
SHIFT = [(0, 0.63), (1, 0.31), (2, 0.06)]
HOURS = [9, 10, 11, 12, 13, 14, 15, 16]


def load_obs(station):
    """-> {date: [(minute_of_day, tmpf), ...]} local time."""
    days = collections.defaultdict(list)
    with open(os.path.join(HERE, f'asos_{station}.csv')) as f:
        for line in f:
            if line.startswith('#') or line.startswith('station'):
                continue
            st, valid, t = line.strip().split(',')
            if t == 'M':
                continue
            d, hm = valid.split(' ')
            m = int(hm[:2]) * 60 + int(hm[3:])
            days[d].append((m, float(t)))
    for d in days:
        days[d].sort()
    return days


def day_state(obs, hour):
    """State at local hour:00 -> (running_max, t_now, trend_1h) or None if no obs yet."""
    cut = hour * 60
    before = [(m, t) for m, t in obs if m <= cut]
    if not before or before[-1][0] < cut - 90:  # last obs older than 90 min: treat as missing
        return None
    rmax = max(t for _, t in before)
    t_now = before[-1][1]
    prev = [t for m, t in before if m <= cut - 60]
    trend = t_now - prev[-1] if prev else 0.0
    return rmax, t_now, trend


def trend_bucket(x):
    return -1 if x <= -1 else (1 if x >= 2 else 0)


def build_tables(obs_by_station, years):
    """-> tables[(station, month, hour, tb)] = Counter of rise (integer F)."""
    tables = collections.defaultdict(collections.Counter)
    for station, days in obs_by_station.items():
        for d, obs in days.items():
            y, mo = int(d[:4]), int(d[5:7])
            if y not in years or len(obs) < 12:
                continue
            final = max(t for _, t in obs)
            for h in HOURS:
                s = day_state(obs, h)
                if s is None:
                    continue
                rmax, t_now, trend = s
                rise = int(round(final - rmax))
                for mm in (mo - 1, mo, mo + 1):  # +-1 month window
                    mm = (mm - 1) % 12 + 1
                    tables[(station, mm, h, trend_bucket(trend))][rise] += 1
                    tables[(station, mm, h, None)][rise] += 1
    return tables


def rise_dist(tables, station, month, hour, trend):
    c = tables.get((station, month, hour, trend_bucket(trend)))
    if not c or sum(c.values()) < 60:
        c = tables.get((station, month, hour, None))
    if not c:
        return None
    n = sum(c.values())
    return {k: v / n for k, v in c.items()}


def bracket_hit(strike_type, lo, hi, t):
    """settled integer t in bracket?"""
    if strike_type == 'less':
        return t < hi          # cap: T <= cap-1
    if strike_type == 'greater':
        return t > lo          # floor: T >= floor+1
    return lo <= t <= hi       # between (inclusive)


def p_bracket(dist, rmax, strike_type, lo, hi):
    p = 0.0
    for rise, w in dist.items():
        for k, ws in SHIFT:
            t = int(round(rmax)) + rise + k
            if bracket_hit(strike_type, lo, hi, t):
                p += w * ws
    return p


def fee_taker(p, n=1):
    return math.ceil(0.07 * p * (1 - p) * n * 10000) / 10000


def main():
    years_fit = {2022, 2023, 2024, 2025}
    obs = {st: load_obs(st) for st, _ in STATIONS.values()}
    tables = build_tables(obs, years_fit)
    print(f'tables: {len(tables)} cells')
    con = sqlite3.connect(f'file:{DB}?mode=ro', uri=True)
    cur = con.cursor()
    markets = cur.execute("SELECT ticker,event_ticker,series_ticker,strike_type,floor_strike,cap_strike,result,close_time "
                          "FROM hist_markets WHERE series_ticker IN (%s) AND result!=''" %
                          ','.join('?' * len(STATIONS)), list(STATIONS)).fetchall()
    print(f'{len(markets)} settled weather markets')
    trades = collections.defaultdict(list)  # ticker -> [(ts, yes_price)]
    for t, ts, px in cur.execute("SELECT ticker, ts, yes_price FROM hist_trades WHERE ticker LIKE 'KXHIGH%' OR ticker LIKE 'KXDENHIGH%'"):
        trades[t].append((ts, px))
    for t in trades:
        trades[t].sort()
    print(f'{sum(len(v) for v in trades.values())} weather trades on {len(trades)} markets')

    # Per (hour): lists of (p_model, p_market, outcome, station, ticker, date, rmax)
    rows = collections.defaultdict(list)
    settled_check = collections.Counter()
    for ticker, ev, series, stype, lo, hi, result, close_time in markets:
        station, tz = STATIONS[series]
        date = ev.split('-')[1]  # 26SEP05
        d = dt.datetime.strptime(date, '%y%b%d').date()
        dkey = d.isoformat()
        day_obs = obs[station].get(dkey)
        if not day_obs or len(day_obs) < 12:
            continue
        outcome = 1 if result == 'yes' else 0
        final = max(t for _, t in day_obs)
        # settlement check: which shift makes the METAR max land in the settled bracket?
        if outcome == 1:
            for k, _ in SHIFT:
                if bracket_hit(stype, lo, hi, int(round(final)) + k):
                    settled_check[k] += 1
                    break
            else:
                settled_check['other'] += 1
        for h in HOURS:
            s = day_state(day_obs, h)
            if s is None:
                continue
            rmax, t_now, trend = s
            dist = rise_dist(tables, station, d.month, h, trend)
            if dist is None:
                continue
            pm = p_bracket(dist, rmax, stype, lo, hi)
            # market: last trade at or before local h:00 (converted to UTC), within 3 h
            local = dt.datetime(d.year, d.month, d.day, h, 0, tzinfo=ZoneInfo(tz))
            ts = int(local.timestamp())
            tl = trades.get(ticker, [])
            i = bisect.bisect_right(tl, (ts, 9))
            if i == 0 or ts - tl[i - 1][0] > 3 * 3600:
                pk = None
            else:
                pk = tl[i - 1][1]
            rows[h].append((pm, pk, outcome, station, ticker, dkey, rmax, t_now, trend))
    print('settlement shift vs METAR max among YES brackets:', dict(settled_check))
    print()
    print('hour  n_all  brier_model | n_mkt  brier_model  brier_market  brier_blend  | trade sim (edge>=.08, taker fee): n  mean_ret/$  total')
    for h in HOURS:
        r = rows[h]
        if not r:
            continue
        bm_all = sum((pm - o) ** 2 for pm, pk, o, *_ in r) / len(r)
        rk = [x for x in r if x[1] is not None]
        if not rk:
            print(f'{h:>4} {len(r):>6} {bm_all:.4f} | no tape')
            continue
        bm = sum((pm - o) ** 2 for pm, pk, o, *_ in rk) / len(rk)
        bk = sum((pk - o) ** 2 for pm, pk, o, *_ in rk) / len(rk)
        bb = sum(((pm + pk) / 2 - o) ** 2 for pm, pk, o, *_ in rk) / len(rk)
        # trade sim
        n = 0; tot = 0.0
        for pm, pk, o, *_ in rk:
            if pm - pk >= 0.08 and 0.03 <= pk <= 0.97:          # buy YES at pk
                n += 1; tot += (o - pk) - fee_taker(pk)
            elif pk - pm >= 0.08 and 0.03 <= pk <= 0.97:        # buy NO at 1-pk
                n += 1; tot += ((1 - o) - (1 - pk)) - fee_taker(pk)
        print(f'{h:>4} {len(r):>6} {bm_all:.4f} | {len(rk):>5}  {bm:.4f}       {bk:.4f}        {bb:.4f}      | {n:>4}  {tot / n if n else 0:+.3f}   {tot:+.2f}')
    # by-station Brier at 13:00
    print()
    print('station breakdown at 12:00 and 14:00 (model vs market Brier, n):')
    for h in (12, 14):
        by = collections.defaultdict(list)
        for pm, pk, o, station, *_ in rows[h]:
            if pk is not None:
                by[station].append((pm, pk, o))
        for st, v in sorted(by.items()):
            bm = sum((pm - o) ** 2 for pm, pk, o in v) / len(v)
            bk = sum((pk - o) ** 2 for pm, pk, o in v) / len(v)
            print(f'  {h:>2}h {st} n={len(v):>3} model {bm:.4f} market {bk:.4f}')
    json.dump({f'{k[0]}|{k[1]}|{k[2]}|{k[3]}': dict(v) for k, v in tables.items()},
              open(os.path.join(HERE, 'wx_rise_tables.json'), 'w'))


if __name__ == '__main__':
    main()
