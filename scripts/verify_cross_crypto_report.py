#!/usr/bin/env python3
"""Every number in reports/cross_crypto_replication.md, checked against its source.

Written because two conclusions in this project have already had to be withdrawn after review.
A report is not finished when it reads well; it is finished when each figure in it has been
traced back to the file that produced it. Run this after any edit to the report.

Normalises the Unicode minus (U+2212) used in the prose to ASCII before matching, and reads the
null table rows rather than the observed block, which repeats the same statistic names.
"""
import csv, glob, re, sys
import numpy as np

REP = 'reports/cross_crypto_replication.md'
LOG = 'reports/cross_crypto_close_raw.log'
FAILS = []


def norm(s):
    return s.replace('−', '-').replace('–', '-')


CHECKS = [0]


def chk(cond, msg):
    CHECKS[0] += 1
    if not cond:
        FAILS.append(msg)
    print(('  ok   ' if cond else '  FAIL ') + msg)


def null_rows(log):
    """The rows under the 'null distributions' header, keyed by statistic."""
    out, seen = {}, False
    for ln in log.splitlines():
        if ln.startswith('null distributions'):
            seen = True
            continue
        if not seen:
            continue
        parts = ln.split()
        if len(parts) >= 7 and parts[0] in ('max', 'mean', 'median', 'n_over_1', 'frac_positive'):
            out[parts[0]] = ln
    return out


def main():
    rep = norm(open(REP).read())
    log = norm(open(LOG).read())
    nulls = null_rows(log)

    print('NULL TABLE (report values must appear in the raw log)')
    expect = {
        'max':           ['1.0854', '1.9354', '2.0430', '2.8229', '2.9346', '1.5424', '0.167'],
        'mean':          ['-0.0026', '0.1343', '0.1717', '0.2379', '0.2961', '-0.0376', '0.630'],
        'median':        ['-0.0001', '0.0353', '0.0451', '0.0742', '0.0900', '-0.0528', '0.943'],
        'frac_positive': ['0.4976', '0.6286', '0.6762', '0.7098', '0.7714', '0.3238', '0.923'],
    }
    for stat, vals in expect.items():
        chk(stat in nulls, 'null row present: ' + stat)
        for v in vals:
            chk(v in nulls.get(stat, ''), '  %-14s %-9s in log' % (stat, v))
            chk(v in rep, '  %-14s %-9s in report' % (stat, v))
    # n_over_1 is printed with %.4f in the log and as integers in the report table
    chk('n_over_1' in nulls, 'null row present: n_over_1')
    for v, as_int in (('1.0000', '1'), ('12.0000', '12'), ('26.0000', '26'),
                      ('41.0000', '41'), ('46.0000', '46')):
        chk(v in nulls.get('n_over_1', ''), '  n_over_1       %-9s in log' % v)
        chk(re.search(r'\|\s*%s\s*\|' % as_int, rep) is not None,
            '  n_over_1       %-9s in report table' % as_int)
    chk('0.237' in nulls.get('n_over_1', '') and '0.237' in rep, '  n_over_1       p=0.237')

    print('\nSIGN STABILITY')
    for v in ('39.87', '6.61', '84.89', '11.63', '-21.63', '56.65', '0.434', '0.481', '0.660'):
        chk(v in log, '  %-8s in log' % v)
        chk(v in rep, '  %-8s in report' % v)

    print('\nLEADERBOARD (report table vs the per-bar CSVs, and arithmetic)')
    rows = {}
    for f in glob.glob('reports/cross_crypto_*m.csv'):
        for r in csv.DictReader(open(f)):
            rows[(r['focal'], r['label'], r['bar_min'])] = r
    lead = [('SOLUSDT', 'own momentum', 43.56, 28.24, 15.32, 1.5424),
            ('AAVEUSDT', 'own momentum', 58.24, 38.56, 19.68, 1.5104),
            ('XRPUSDT', 'own momentum', 34.14, 29.08, 5.06, 1.1740),
            ('ETHUSDT', 'equal-weight basket', 28.12, 25.00, 3.12, 1.1250),
            ('XRPUSDT', 'equal-weight basket', 32.06, 29.08, 2.98, 1.1024),
            ('AAVEUSDT', 'equal-weight basket', 40.42, 38.56, 1.86, 1.0483)]
    for a, l, g, rt, n, er in lead:
        r = rows.get((a, l, '720'))
        chk(r is not None, '  csv row %s / %s' % (a, l))
        if not r:
            continue
        chk(abs(float(r['gross_bps_per_trade']) - g) < 0.011, '    %-9s gross %8.2f' % (a, g))
        chk(abs(float(r['round_trip_bps']) - rt) < 0.011, '    %-9s rt    %8.2f' % (a, rt))
        chk(abs(float(r['net_bps_per_trade']) - n) < 0.011, '    %-9s net   %8.2f' % (a, n))
        chk(abs(float(r['economic_ratio']) - er) < 0.0001, '    %-9s ratio %8.4f' % (a, er))
        chk(abs((g - rt) - n) < 0.011, '    %-9s net = gross - round trip' % a)
        chk(abs(g / rt - er) < 0.0002, '    %-9s ratio = gross / round trip' % a)

    print('\nMODEL CLASS')
    allr = []
    for f in glob.glob('reports/cross_crypto_*m.csv'):
        for r in csv.DictReader(open(f)):
            try:
                allr.append((float(r['economic_ratio']), r['label']))
            except (TypeError, ValueError):
                pass
    lz = [x for x in allr if x[1].lower().startswith('lasso')]
    nm = [x for x in allr if not x[1].lower().startswith('lasso')]
    see = [x for x in nm if x[1].startswith('seesaw')]
    cx = [x for x in nm if x[1] in ('BTC lagged return', 'leader-only (BTC+ETH)',
                                    'seesaw sign (minus basket)', 'equal-weight basket')]
    om = [x for x in nm if x[1] == 'own momentum']
    for name, got, want in (('LASSO specs', len(lz), 42), ('seesaw specs', len(see), 42),
                            ('no-model specs', len(nm), 210), ('cross-asset specs', len(cx), 168),
                            ('own-momentum specs', len(om), 42), ('CSV total', len(allr), 252)):
        chk(got == want, '  %-20s %d == %d' % (name, got, want))
    v = lambda xs: np.array([x[0] for x in xs])
    chk(abs(v(lz).mean() + 0.1187) < 0.0001, '  LASSO  mean   -0.1187')
    chk(abs(v(lz).max() - 0.4915) < 0.0001, '  LASSO  best    0.4915')
    chk((v(lz) >= 1).sum() == 0, '  LASSO  clears 1.0x zero times')
    chk(abs((v(lz) > 0).mean() - 0.214) < 0.002, '  LASSO  21.4% positive')
    chk(abs(v(see).mean() + 0.0840) < 0.0001, '  seesaw mean   -0.0840')
    chk(abs(v(see).max() - 0.7888) < 0.0001, '  seesaw best    0.7888')
    chk((v(see) >= 1).sum() == 0, '  seesaw clears 1.0x zero times')
    chk((v(cx) >= 1).sum() == 3, '  cross-asset clears 3')
    chk(abs(v(cx).max() - 1.1250) < 0.0001, '  cross-asset best 1.1250')
    chk((v(om) >= 1).sum() == 3, '  own momentum clears 3')
    chk((v(nm) >= 1).sum() == 6, '  no-model clears 6')
    chk(abs(v(nm).mean() + 0.0376) < 0.0001, '  no-model mean -0.0376')
    chk(abs((v(nm) > 0).mean() - 0.3238) < 0.0002, '  no-model frac positive 0.3238')
    chk(len(lz) + len(see) == 84, '  LASSO + seesaw = 84 specifications')

    print('\nCLAIMS PRESENT IN THE REPORT')
    # Prose wraps, so collapse whitespace before matching a sentence fragment.
    flat = re.sub(r'\s+', ' ', rep)
    for phrase in ('zero times out of 84', 'clears zero times out of 42',
                   'FAILED_SELECTION',
                   'not "cross-crypto predictability does not exist,"'):
        chk(phrase in flat, '  "%s"' % phrase)

    print('\nRESULT: %d checks, %d failed' % (CHECKS[0], len(FAILS)))
    for f in FAILS:
        print('   ->', f.strip())
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
