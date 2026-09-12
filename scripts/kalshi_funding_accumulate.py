#!/usr/bin/env python3
"""Append a daily snapshot of Kalshi perp funding, so the carry question gets a real answer.

Funding was measured once, on 2026-09-11, and 14 of 18 perps read exactly 0.0000 bps per period.
That is a snapshot, not a distribution. A cross-sectional carry model needs dispersion, and a
single day cannot say whether the zeros are structural or that day's state.

So this appends one row per market per run to reports/kalshi_funding_history.csv and reports the
cross-sectional dispersion over whatever history has accumulated. Run it daily. After a few weeks
the question "is there anything to rank on" has a measured answer instead of a guess.

Idempotent by (date, market): running twice in a day overwrites rather than duplicates.
"""
import csv, os, subprocess, sys, datetime as dt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HIST = os.path.join(ROOT, 'reports', 'kalshi_funding_history.csv')
SNAP = os.path.join(ROOT, 'reports', 'funding_study.csv')
COLS = ['date', 'market', 'rate_bps_per_period_median', 'rate_bps_min', 'rate_bps_max',
        'vol_bps_per_period', 'median_mid_minus_mark_bps', 'n_obs']


def refresh_snapshot():
    """Re-run the funding study over the recorder's current tape, if it is runnable."""
    script = os.path.join(ROOT, 'scripts', 'funding_study.py')
    if not os.path.exists(script):
        return False
    try:
        subprocess.run([sys.executable, script], cwd=ROOT, timeout=900,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        return True
    except (subprocess.TimeoutExpired, OSError) as e:
        print('funding_study did not run: %s' % e, file=sys.stderr)
        return False


def main():
    refresh_snapshot()
    if not os.path.exists(SNAP):
        sys.exit('no %s; run scripts/funding_study.py first' % SNAP)
    today = dt.datetime.now(dt.timezone.utc).strftime('%Y-%m-%d')

    rows = []
    if os.path.exists(HIST):
        rows = [r for r in csv.DictReader(open(HIST)) if r.get('date') != today]

    for r in csv.DictReader(open(SNAP)):
        rows.append({'date': today, 'market': r['market'],
                     'rate_bps_per_period_median': r.get('rate_bps_per_period_median', ''),
                     'rate_bps_min': r.get('rate_bps_min', ''),
                     'rate_bps_max': r.get('rate_bps_max', ''),
                     'vol_bps_per_period': r.get('vol_bps_per_period', ''),
                     'median_mid_minus_mark_bps': r.get('median_mid_minus_mark_bps', ''),
                     'n_obs': r.get('n_obs', '')})

    rows.sort(key=lambda r: (r['date'], r['market']))
    with open(HIST, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=COLS, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)

    days = sorted({r['date'] for r in rows})
    print('%s: %d markets, history now spans %d day(s) (%s .. %s)'
          % (today, sum(1 for r in rows if r['date'] == today), len(days), days[0], days[-1]))

    print('\ncross-sectional funding by day -- the thing a carry model would rank on')
    print('%-12s %6s %8s %9s %9s %9s' % ('date', 'mkts', 'nonzero', 'sd bps', 'min', 'max'))
    for d in days:
        v = []
        for r in rows:
            if r['date'] != d:
                continue
            try:
                v.append(float(r['rate_bps_per_period_median']))
            except (TypeError, ValueError):
                pass
        if not v:
            continue
        import statistics as st
        nz = sum(1 for x in v if abs(x) > 1e-9)
        sd = st.pstdev(v) if len(v) > 1 else 0.0
        print('%-12s %6d %8d %9.3f %9.3f %9.3f' % (d, len(v), nz, sd, min(v), max(v)))
    print('\nA carry model needs the "nonzero" column to be most of "mkts". While it is 4 of 18,')
    print('a cross-sectional rank produces four names and fourteen ties.')


if __name__ == '__main__':
    main()
