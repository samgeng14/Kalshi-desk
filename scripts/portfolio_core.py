#!/usr/bin/env python3
"""Panel, rolling PCA, cost model and simulator for the weekly Kalshi perp portfolio.

DESIGN DECISIONS, MADE ONCE AND WRITTEN DOWN HERE RATHER THAN TUNED LATER.

Rebalance weekly. A measured 3-long/3-short cross-sectional book turns over ~1.51 in gross
weight per rebalance, so at Kalshi's fee schedule a 12-hour rebalance costs 187% a year and a
weekly one costs 13%. That is not a parameter to search; it is the constraint the architecture
has to satisfy. See reports/portfolio_feasibility.md.

Hold weekly, mark hourly. Positions change on the weekly schedule, but profit and loss accrues
every hour, so the return series the bootstrap sees has thousands of observations clustered by
calendar day rather than 307 weekly points.

Cost is charged on weight CHANGES, never per bar. cost_t = sum_i |w_it - w_i,t-1| * oneway_i.
The previous study in this project charged a full round trip every bar while the position only
moved on half of them, which made the hurdle 1.9x too harsh and inverted a headline. The same
mistake is not available here because weights are explicit.

Every strategy is scored against the better of always-long and always-short on the same slice.
Equal-weight long returned +191% in 2021 and -151% in 2022; a book that merely rides that beta
is not a strategy, and the drift control is what says so.
"""
import os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cross_crypto as C

HOURS_PER_YEAR = 24 * 365
DEFAULT_UNIVERSE = ['AAVEUSDT', 'ADAUSDT', 'AVAXUSDT', 'BCHUSDT', 'BNBUSDT', 'BTCUSDT',
                    'DOGEUSDT', 'ETHUSDT', 'LINKUSDT', 'LTCUSDT', 'NEARUSDT', 'SOLUSDT',
                    'XRPUSDT']


def load_panel(data_dir=None, universe=None, bar=60):
    """Hourly log returns in bps, a common clock, and the asset names."""
    if data_dir is None:
        data_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                'data', 'binance')
    universe = universe or DEFAULT_UNIVERSE
    rets, ts, names, err = C.build_panel(data_dir, universe, bar, 'klines_1h')
    if err:
        raise SystemExit(err)
    return rets, np.asarray(ts), names


def oneway_bps(names):
    return np.array([C.KALSHI_ONEWAY_BPS.get(a, C.DEFAULT_ONEWAY) for a in names])


def rolling_pca(rets, window=720, n_pc=3, step=24):
    """Trailing-window PCA loadings, recomputed every `step` bars and held constant between.

    Trailing rather than expanding: the factor structure of this market is not stationary over
    five years, and a loading estimated partly on 2021 is not the loading that will neutralise a
    2026 position. Recomputed daily rather than hourly because the loadings move slowly and the
    full decomposition on every bar dominates runtime for no measurable gain.

    Returns (betas, resid) both aligned to `rets`. Rows before the first full window are NaN, and
    every caller must drop them -- a zero-filled warm-up period would be a silent in-sample leak.
    """
    T, N = rets.shape
    betas = np.full((T, N, n_pc), np.nan)
    resid = np.full((T, N), np.nan)
    last = None
    for t in range(window, T):
        if last is None or (t - window) % step == 0:
            W = rets[t - window:t]
            sd = W.std(axis=0)
            sd[sd < 1e-12] = 1.0
            Z = (W - W.mean(axis=0)) / sd
            cov = np.cov(Z, rowvar=False)
            ev, evec = np.linalg.eigh(cov)
            evec = evec[:, ::-1][:, :n_pc]
            # Sign convention: PC1 points the way the market does, so a positive PC1 loading
            # means "moves with crypto". Without this the neutralisation still works but the
            # sign of every reported beta flips arbitrarily between refits.
            for j in range(n_pc):
                if evec[:, j].sum() < 0:
                    evec[:, j] = -evec[:, j]
            last = (evec, sd, W.mean(axis=0))
        evec, sd, mu = last
        betas[t] = evec
        z = (rets[t] - mu) / sd
        resid[t] = (z - evec @ (evec.T @ z)) * sd
    return betas, resid


def neutralise(w, beta1):
    """Project the PC1 direction out of a weight vector, then restore gross exposure.

    Projection rather than a constrained optimiser: with equal-weight buckets there is nothing
    for an optimiser to do that the projection does not, and the projection cannot fail to
    converge at 3am.
    """
    b = np.asarray(beta1, dtype=float)
    if not np.isfinite(b).all() or b @ b < 1e-12:
        return w
    gross = np.abs(w).sum()
    w = w - (w @ b) / (b @ b) * b
    g = np.abs(w).sum()
    return w * (gross / g) if g > 1e-12 else w


def rank_book(score, k=3, long_short=True, prev=None, buffer=0):
    """Equal-weight top-k long, bottom-k short, gross exposure 1.0.

    `buffer` is hysteresis in RANK, not in weight. With equal weights of 0.5/k a held position is
    either in the bucket or not, so |dw| is 0 or 0.5/k and a weight-threshold no-trade band can
    never bind -- an earlier version had exactly that bug and the band column did nothing. Rank
    hysteresis is the version that means something: a name already held is kept while it stays
    inside the top (k + buffer), and only a name outside that wider bucket is replaced. That is
    what stops ranking noise at the boundary from generating a round trip every week.
    """
    s = np.asarray(score, dtype=float)
    ok = np.isfinite(s)
    if ok.sum() < 2 * k:
        return np.zeros_like(s)
    live = np.where(ok)[0]
    order = live[np.argsort(s[live])]
    n = len(order)
    w = np.zeros_like(s)
    if not long_short:
        w[order[-k:]] = 1.0 / k
        return w
    if prev is None or buffer <= 0:
        w[order[-k:]] = 0.5 / k
        w[order[:k]] = -0.5 / k
        return w
    rank = {int(a): i for i, a in enumerate(order)}
    wide = k + buffer
    keep_l = [i for i in np.where(prev > 0)[0] if rank.get(int(i), -1) >= n - wide]
    keep_s = [i for i in np.where(prev < 0)[0] if rank.get(int(i), n) < wide]
    longs = list(keep_l)
    for a in order[::-1]:
        if len(longs) >= k:
            break
        if int(a) not in longs:
            longs.append(int(a))
    shorts = list(keep_s)
    for a in order:
        if len(shorts) >= k:
            break
        if int(a) not in shorts and int(a) not in longs:
            shorts.append(int(a))
    longs, shorts = longs[:k], shorts[:k]
    w[longs] = 0.5 / k
    w[shorts] = -0.5 / k
    return w


def simulate(rets, ts, names, score_fn, rebalance_h=168, k=3, neutral=True,
             betas=None, buffer=0, vol_target_pct=None, vol_window=720, max_leverage=3.0,
             warmup=720):
    """Run one strategy. Returns the hourly net P&L series and the diagnostics behind it.

    `buffer` is rank hysteresis: a held name is kept while it stays inside the top (k + buffer).
    It exists because ranking noise at the boundary of the bucket generates trades that cost real
    money and carry no information.
    """
    T, N = rets.shape
    ow = oneway_bps(names)
    w = np.zeros(N)
    raw_prev = None          # pre-neutralisation book, which is what hysteresis reasons about
    W = np.zeros((T, N))
    gross_pnl = np.zeros(T)
    cost = np.zeros(T)
    traded = np.zeros(T)
    lev = np.ones(T)
    for t in range(warmup, T):
        if (t - warmup) % rebalance_h == 0:
            s = score_fn(t)
            tgt = rank_book(s, k=k, prev=raw_prev, buffer=buffer)
            raw_prev = tgt.copy()
            if neutral and betas is not None:
                tgt = neutralise(tgt, betas[t, :, 0])
            if vol_target_pct:
                # vol_target_pct is ANNUALISED PERCENT. Bar returns are bps per hour, so the
                # equivalent per-bar target is pct * 100 / sqrt(hours per year).
                tgt_bar = vol_target_pct * 100.0 / np.sqrt(HOURS_PER_YEAR)
                hist = rets[max(0, t - vol_window):t]
                pv = float(np.std(hist @ tgt)) if len(hist) > 10 else 0.0
                scale = min(max_leverage, tgt_bar / pv) if pv > 1e-9 else 1.0
                tgt = tgt * scale
                lev[t:] = scale
            d = np.abs(tgt - w)
            cost[t] = float((d * ow).sum())
            traded[t] = float(d.sum())
            w = tgt
        W[t] = w
        gross_pnl[t] = float(w @ rets[t])
    net = gross_pnl - cost
    return dict(net=net[warmup:], gross=gross_pnl[warmup:], cost=cost[warmup:],
                traded=traded[warmup:], W=W[warmup:], ts=ts[-T:][warmup:], lev=lev[warmup:])


def metrics(res, label=''):
    net, gross, cost = res['net'], res['gross'], res['cost']
    ts = res['ts']
    n = len(net)
    yrs = n / HOURS_PER_YEAR
    ann = float(net.mean()) * HOURS_PER_YEAR / 100.0
    vol = float(net.std()) * np.sqrt(HOURS_PER_YEAR) / 100.0
    dn = net[net < 0]
    dvol = float(dn.std()) * np.sqrt(HOURS_PER_YEAR) / 100.0 if len(dn) > 10 else float('nan')
    eq = np.cumsum(net) / 100.0
    dd = eq - np.maximum.accumulate(eq)
    mdd = float(-dd.min())
    pos, neg = net[net > 0].sum(), -net[net < 0].sum()
    return dict(label=label, years=yrs,
                ann_pct=ann, vol_pct=vol,
                sharpe=(ann / vol if vol > 1e-9 else float('nan')),
                sortino=(ann / dvol if dvol == dvol and dvol > 1e-9 else float('nan')),
                max_dd_pct=mdd, calmar=(ann / mdd if mdd > 1e-9 else float('nan')),
                profit_factor=(float(pos / neg) if neg > 0 else float('inf')),
                cost_pct_yr=float(cost.mean()) * HOURS_PER_YEAR / 100.0,
                gross_pct_yr=float(gross.mean()) * HOURS_PER_YEAR / 100.0,
                turnover_yr=float(res['traded'].mean()) * HOURS_PER_YEAR,
                net_bps_per_hour=float(net.mean()))


def daily_keys(ts_ms):
    return np.asarray(ts_ms, dtype=np.int64) // 86_400_000


def gate(res, bench_net=None):
    """The gates, applied to a portfolio.

    net_lo is a block-bootstrap 90% lower bound on the net hourly return, clustered by calendar
    day. excess_lo is the same bound on the strategy minus a VOL-MATCHED constant position.

    The vol match is the correction that matters. The constant book here runs at 81% annualised
    volatility and a 183% drawdown; a market-neutral book runs near 30%. Comparing their mean
    returns asks the strategy to beat a position carrying nearly three times its risk, which
    penalises precisely the risk reduction the book exists to deliver. Scaling the benchmark to
    the strategy's own realised volatility asks the question actually worth asking: at the same
    risk, does this beat holding the market?
    """
    keys = daily_keys(res['ts'])
    m, lo, hi, g = C.block_ci(res['net'], keys)
    out = dict(net_mean=m, net_lo=lo, net_hi=hi, clusters=g)
    if bench_net is not None:
        sv, bv = float(np.std(res['net'])), float(np.std(bench_net))
        scale = (sv / bv) if bv > 1e-12 else 1.0
        out['bench_scale'] = scale
        em, elo, ehi, _ = C.block_ci(res['net'] - scale * bench_net, keys)
        out.update(excess=em, excess_lo=elo, excess_hi=ehi)
    return out
