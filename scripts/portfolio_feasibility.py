#!/usr/bin/env python3
"""Before any model: what does Kalshi's fee schedule let a rebalanced portfolio do?

Three questions, answered on the 5.9-year hourly panel already downloaded, before a line of
model code is written. All three determine the architecture rather than being tuned inside it.

  1. THE TURNOVER BUDGET. A cross-sectional long/short book pays a one-way cost every time a
     position enters or leaves. What does that cost per year at each rebalance frequency, and how
     much must the strategy gross to break even?
  2. THE PCA RESIDUAL. How much variance does PC1 take, and how large is the residual that a
     residual-momentum or residual-reversion model would trade, measured against the same cost?
  3. THE PRICE OF NEUTRALITY. PC1-neutralisation removes crypto beta. Over 2021-2026 that beta
     was the single benchmark almost nothing beat. What does giving it up cost?
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

ONEWAY = C.KALSHI_ONEWAY_BPS
BARS_PER_YEAR_1H = 24 * 365


def panel(bar):
    data = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'data', 'binance')
    assets = sorted(a for a in os.listdir(os.path.join(data, 'klines_1h')) if a != 'SUIUSDT')
    rets, ts, names, err = C.build_panel(data, assets, bar, 'klines_1h')
    if err:
        sys.exit(err)
    return rets, np.asarray(ts), names


def q1_turnover_budget():
    print('=' * 78)
    print('1. THE TURNOVER BUDGET')
    print('=' * 78)
    rets, ts, names = panel(60)
    ow = np.array([ONEWAY.get(a, C.DEFAULT_ONEWAY) for a in names])
    print('Kalshi one-way cost, bps: min %.2f (%s)  median %.2f  max %.2f (%s)'
          % (ow.min(), names[int(ow.argmin())], np.median(ow), ow.max(), names[int(ow.argmax())]))
    print('Of every one of those, 12.00 bps is the tier-0 taker fee.\n')

    # A cross-sectional book: rank on trailing return, long top k, short bottom k, rebalance
    # every `h` hours. Turnover is measured, not assumed.
    print('%-10s %-8s %10s %12s %14s %14s'
          % ('rebalance', 'lookback', 'rebal/yr', 'turnover', 'cost %/yr', 'gross needed'))
    for h in (4, 8, 12, 24, 48, 168, 336):
        step = h
        idx = np.arange(0, len(rets) - step, step)
        if len(idx) < 50:
            continue
        k = 3
        w_prev = np.zeros(len(names))
        turn = []
        for t in idx[1:]:
            look = rets[t - step:t].sum(axis=0)
            order = np.argsort(look)
            w = np.zeros(len(names))
            w[order[-k:]] = 1.0 / (2 * k)
            w[order[:k]] = -1.0 / (2 * k)
            turn.append(np.abs(w - w_prev))
            w_prev = w
        turn = np.array(turn)                       # gross weight traded, per rebalance, per coin
        per_rebal_cost = float((turn * ow / 1e4).sum(axis=1).mean())    # fraction of gross
        rebals = BARS_PER_YEAR_1H / h
        annual = per_rebal_cost * rebals
        print('%-10s %-8s %10.0f %12.3f %13.1f%% %13.1f%%'
              % ('%dh' % h, '%dh' % h, rebals, float(turn.sum(axis=1).mean()),
                 annual * 100, annual * 100))
    print('\n"gross needed" is the gross return the book must earn just to pay its own costs.')

    print('\nSame table under the verified fee ladder, 12h rebalance:')
    print('%-16s %12s %14s' % ('30d volume', 'taker bps', 'cost %/yr at 12h'))
    h = 12
    step = h
    idx = np.arange(0, len(rets) - step, step)
    k = 3
    w_prev = np.zeros(len(names))
    turn = []
    for t in idx[1:]:
        look = rets[t - step:t].sum(axis=0)
        order = np.argsort(look)
        w = np.zeros(len(names))
        w[order[-k:]] = 1.0 / (2 * k)
        w[order[:k]] = -1.0 / (2 * k)
        turn.append(np.abs(w - w_prev))
        w_prev = w
    turn = np.array(turn)
    nonfee = ow - 12.00                      # the part of the one-way that is not the tier-0 fee
    for vol, taker, maker in (('$0 (tier 0)', 12.0, 5.0), ('$100k', 10.0, 4.0), ('$1m', 6.0, 2.4),
                              ('$3m', 5.0, 2.0), ('$10m', 4.0, 1.6), ('$100m', 3.2, 1.2)):
        for lbl, fee in (('taker', taker), ('maker', maker)):
            eff = nonfee + fee
            annual = float((turn * eff / 1e4).sum(axis=1).mean()) * (BARS_PER_YEAR_1H / h)
            print('%-16s %6s %5.1f %13.1f%%' % (vol if lbl == 'taker' else '', lbl, fee,
                                                annual * 100))


def q2_pca_residual():
    print('\n' + '=' * 78)
    print('2. THE PCA RESIDUAL, AGAINST THE COST OF TRADING IT')
    print('=' * 78)
    for bar in (60, 240, 720):
        rets, ts, names = panel(bar)
        X = rets - rets.mean(axis=0)
        sd = X.std(axis=0)
        Z = X / sd
        cov = np.cov(Z, rowvar=False)
        ev, evec = np.linalg.eigh(cov)
        ev, evec = ev[::-1], evec[:, ::-1]
        share = ev / ev.sum()
        pcs = Z @ evec[:, :3]
        # residual after 3 PCs, back in bps
        recon = pcs @ evec[:, :3].T
        resid = (Z - recon) * sd
        ow = np.array([ONEWAY.get(a, C.DEFAULT_ONEWAY) for a in names])
        print('\n%4dm bars   PC1 %.1f%%  PC2 %.1f%%  PC3 %.1f%%   (top 3 = %.1f%%)'
              % (bar, share[0] * 100, share[1] * 100, share[2] * 100, share[:3].sum() * 100))
        print('      %-10s %12s %12s %12s %10s'
              % ('', 'raw sd bps', 'resid sd bps', 'round trip', 'resid/rt'))
        for i, a in enumerate(names):
            if a not in ('BTCUSDT', 'ETHUSDT', 'SOLUSDT', 'XRPUSDT', 'LINKUSDT'):
                continue
            print('      %-10s %12.1f %12.1f %12.1f %10.2f'
                  % (a, float(rets[:, i].std()), float(resid[:, i].std()), 2 * ow[i],
                     float(resid[:, i].std()) / (2 * ow[i])))
        print('      %-10s %12.1f %12.1f %12.1f %10.2f'
              % ('MEDIAN', float(np.median(rets.std(axis=0))),
                 float(np.median(resid.std(axis=0))), float(np.median(2 * ow)),
                 float(np.median(resid.std(axis=0) / (2 * ow)))))


def q3_price_of_neutrality():
    print('\n' + '=' * 78)
    print('3. WHAT PC1-NEUTRALITY COSTS, 2021-2026')
    print('=' * 78)
    rets, ts, names = panel(720)
    import datetime as dt
    yrs = np.array([dt.datetime.utcfromtimestamp(t / 1000).year for t in ts[-len(rets):]])
    eq = rets.mean(axis=1)                      # equal-weight long-only, bps per 12h bar
    print('%-8s %14s %16s %14s' % ('year', 'EW long bp/bar', 'annualised %', 'cross-sec sd bp'))
    for y in sorted(set(yrs)):
        m = yrs == y
        if m.sum() < 50:
            continue
        disp = float(np.mean(rets[m].std(axis=1)))
        print('%-8d %14.2f %15.1f%% %14.1f'
              % (y, float(eq[m].mean()), float(eq[m].mean()) * 730 / 100, disp))
    print('%-8s %14.2f %15.1f%% %14.1f'
          % ('ALL', float(eq.mean()), float(eq.mean()) * 730 / 100,
             float(np.mean(rets.std(axis=1)))))
    print('\nThe last column is the cross-sectional dispersion a long/short book harvests from.')
    print('The middle column is what a PC1-neutral book gives up by construction.')


if __name__ == '__main__':
    q1_turnover_budget()
    q2_pca_residual()
    q3_price_of_neutrality()
