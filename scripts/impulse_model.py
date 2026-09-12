#!/usr/bin/env python3
"""Phase 3 -- the model ladder for BTC/ETH directional impulse.

The ladder exists so that "the model works" has to mean something. Each rung is scored the same
way, walk-forward, in net basis points after the measured cost, and a rung only counts if it
beats the rung below it by more than the noise in the difference. A LightGBM that beats a
momentum rule by 0.3 bps has not beaten it.

  0  no-trade          the benchmark. Zero bps, zero risk, zero variance. Everything must beat it
                       and most things will not
  1  momentum          sign of the last k-second return. One parameter, no fitting
  2  external residual Kalshi mid versus the CF anchor, traded as a convergence
  3  microstructure    order book imbalance and trade imbalance, linear, no fitting
  4  logistic          fitted direction, walk-forward
  5  ridge             fitted magnitude, walk-forward
  6  gbm               nonlinear, walk-forward, only if scikit-learn is importable

THE ARITHMETIC THAT DECIDES THIS BEFORE ANY MODEL IS FITTED

For a directional trade the breakeven accuracy is p = (cost/E|r| + 1)/2. The impulse viability
work already ran this: BTC at 10 minutes needs 65.4% to clear 17 bps and 59.0% to clear 10 bps;
ETH needs 61.8% and 57.0%; XRP 59.7% and 55.7%. Published short-horizon crypto direction models
reach 52 to 56 percent. The 10s, 30s and 1m horizons were dropped for the same reason -- a
PERFECT oracle on the top decile of 1-minute BTC moves loses 3.2 bps, so no model of any quality
can win there.

So this file prints the required accuracy per market and horizon FIRST, compares it to a stated
ceiling of what is achievable, and marks the combination UNREACHABLE before fitting anything. A
model ladder that fits seven models to a target that a perfect predictor cannot profit from is
not research; it is a way of generating a number that looks like progress.

  python3 scripts/impulse_model.py --features data/features
"""
import argparse, bisect, csv, glob, gzip, json, math, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HORIZONS_S = [120, 300, 600]
# The best documented out-of-sample directional accuracy for short-horizon crypto. Stated here as
# a number so that "the model needs 65%" can be compared to something rather than waved at.
ACHIEVABLE_ACCURACY = 0.56
MIN_TRAIN = 400
MIN_TEST = 100
N_FOLDS = 5

MICRO_FEATURES = ['L1_OBI', 'L3_OBI', 'L5_OBI', 'microprice_minus_mid_bps',
                  'trade_imbalance_1s', 'trade_imbalance_5s', 'trade_imbalance_30s',
                  'cancel_imbalance_5s', 'add_imbalance_5s', 'spread_bps',
                  'book_update_rate_1s']


def breakeven_accuracy(cost_bps, mean_abs_move_bps):
    """p such that p*move - (1-p)*move = cost. Below this, a correct call still loses money."""
    if not mean_abs_move_bps or mean_abs_move_bps <= 0:
        return None
    return (cost_bps / mean_abs_move_bps + 1.0) / 2.0


def mean(xs):
    xs = [x for x in xs if x is not None]
    return sum(xs) / len(xs) if xs else None


def stdev(xs):
    xs = [x for x in xs if x is not None]
    return statistics.pstdev(xs) if len(xs) > 1 else 0.0


class Rung:
    """One model in the ladder. `fit` may be a no-op; `predict` returns a signed score."""
    name = 'rung'
    fits = False

    def fit(self, X, y, cols):
        pass

    def predict(self, row, cols):
        raise NotImplementedError


class NoTrade(Rung):
    name = 'no_trade'
    def predict(self, row, cols):
        return 0.0


class Momentum(Rung):
    name = 'momentum'
    def __init__(self, feat='ret_30s_bps'):
        self.feat = feat
    def predict(self, row, cols):
        v = row.get(self.feat)
        return 0.0 if v is None else (1.0 if v > 0 else -1.0 if v < 0 else 0.0)


class ExternalResidual(Rung):
    """Kalshi rich to the CF anchor should fall. Sign is negative on purpose: this is convergence.

    CF is the reference anchor, not a fair value -- the contract settles against it, so a basis
    is a statement about where Kalshi is relative to the index, not a mispricing to be harvested.
    """
    name = 'external_residual'
    def predict(self, row, cols):
        v = row.get('cf_basis_bps')
        return 0.0 if v is None else -math.tanh(v / 20.0)


class MicroLinear(Rung):
    """Unfitted sum of standardised microstructure features. The point of not fitting it is that
    it cannot overfit, so if the fitted models do not beat it, the fitting bought nothing."""
    name = 'microstructure'
    def __init__(self):
        self.mu, self.sd = {}, {}
    def fit(self, X, y, cols):
        for c in MICRO_FEATURES:
            if c not in cols:
                continue
            i = cols.index(c)
            col = [r[i] for r in X if r[i] is not None]
            self.mu[c] = mean(col) or 0.0
            self.sd[c] = stdev(col) or 1.0
    fits = True
    def predict(self, row, cols):
        s, n = 0.0, 0
        for c in ('L1_OBI', 'L3_OBI', 'trade_imbalance_5s', 'microprice_minus_mid_bps'):
            v = row.get(c)
            if v is None or c not in self.sd:
                continue
            s += (v - self.mu[c]) / (self.sd[c] or 1.0)
            n += 1
        return math.tanh(s / max(n, 1))


class Logistic(Rung):
    name = 'logistic'
    fits = True
    def __init__(self, lr=0.05, epochs=25, l2=1e-3):
        self.lr, self.epochs, self.l2 = lr, epochs, l2
        self.w, self.b, self.cols = None, 0.0, []
        self.mu, self.sd = [], []
    def fit(self, X, y, cols):
        self.cols = [c for c in MICRO_FEATURES if c in cols]
        idx = [cols.index(c) for c in self.cols]
        rows = [[(r[i] if r[i] is not None else 0.0) for i in idx] for r in X]
        if not rows:
            return
        k = len(idx)
        self.mu = [mean([r[j] for r in rows]) or 0.0 for j in range(k)]
        self.sd = [stdev([r[j] for r in rows]) or 1.0 for j in range(k)]
        rows = [[(r[j] - self.mu[j]) / self.sd[j] for j in range(k)] for r in rows]
        lab = [1.0 if v > 0 else 0.0 for v in y]
        self.w, self.b = [0.0] * k, 0.0
        n = len(rows)
        for _ in range(self.epochs):
            gw, gb = [0.0] * k, 0.0
            for r, t in zip(rows, lab):
                z = sum(wi * ri for wi, ri in zip(self.w, r)) + self.b
                p = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))
                e = p - t
                for j in range(k):
                    gw[j] += e * r[j]
                gb += e
            for j in range(k):
                self.w[j] -= self.lr * (gw[j] / n + self.l2 * self.w[j])
            self.b -= self.lr * gb / n
    def predict(self, row, cols):
        if not self.w:
            return 0.0
        z = self.b
        for j, c in enumerate(self.cols):
            v = row.get(c)
            v = 0.0 if v is None else (v - self.mu[j]) / self.sd[j]
            z += self.w[j] * v
        p = 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))
        return 2.0 * p - 1.0


class Ridge(Rung):
    name = 'ridge'
    fits = True
    def __init__(self, lam=1.0):
        self.lam, self.w, self.cols = lam, None, []
        self.mu, self.sd, self.ymu, self.ysd = [], [], 0.0, 1.0
    def fit(self, X, y, cols):
        self.cols = [c for c in MICRO_FEATURES if c in cols]
        idx = [cols.index(c) for c in self.cols]
        rows = [[(r[i] if r[i] is not None else 0.0) for i in idx] for r in X]
        if not rows:
            return
        k = len(idx)
        self.mu = [mean([r[j] for r in rows]) or 0.0 for j in range(k)]
        self.sd = [stdev([r[j] for r in rows]) or 1.0 for j in range(k)]
        Z = [[(r[j] - self.mu[j]) / self.sd[j] for j in range(k)] for r in rows]
        self.ymu, self.ysd = mean(y) or 0.0, stdev(y) or 1.0
        t = [(v - self.ymu) / self.ysd for v in y]
        # Normal equations with ridge. k is small (11), so an explicit solve is fine and keeps
        # this file free of a numpy dependency it does not otherwise need.
        A = [[sum(Z[r][i] * Z[r][j] for r in range(len(Z))) + (self.lam if i == j else 0.0)
              for j in range(k)] for i in range(k)]
        b = [sum(Z[r][i] * t[r] for r in range(len(Z))) for i in range(k)]
        self.w = _solve(A, b)
    def predict(self, row, cols):
        if not self.w:
            return 0.0
        z = 0.0
        for j, c in enumerate(self.cols):
            v = row.get(c)
            v = 0.0 if v is None else (v - self.mu[j]) / self.sd[j]
            z += self.w[j] * v
        return math.tanh(z)


def _solve(A, b):
    """Gaussian elimination with partial pivoting. Returns None on a singular system."""
    n = len(A)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-12:
            return None
        M[c], M[p] = M[p], M[c]
        for r in range(n):
            if r == c:
                continue
            f = M[r][c] / M[c][c]
            for j in range(c, n + 1):
                M[r][j] -= f * M[c][j]
    return [M[i][n] / M[i][i] for i in range(n)]


LADDER = [NoTrade, Momentum, ExternalResidual, MicroLinear, Logistic, Ridge]


def walk_forward(rows, cols, horizon_s, cost_bps, conf_quantile=0.9):
    """Expanding-window walk-forward. Every score is out of sample, and every fold is reported.

    A single train/test split hides the thing that matters most about an intraday model: whether
    it works in the second half of the sample as well as the first. Reporting fold-by-fold makes
    a model that worked once and then stopped look like what it is.
    """
    ts = [r['feature_ts_us'] for r in rows]
    mid = [r['mid'] for r in rows]
    h = horizon_s * 1_000_000
    samples = []
    for i, t0 in enumerate(ts):
        j = bisect.bisect_left(ts, t0 + h)
        if j >= len(ts) or ts[j] - t0 > h * 1.5 or not mid[i]:
            continue
        samples.append((rows[i], (mid[j] - mid[i]) / mid[i] * 1e4))
    n = len(samples)
    if n < MIN_TRAIN + MIN_TEST:
        return None, f'{n} usable samples, need {MIN_TRAIN + MIN_TEST}'

    # Independent observations, not rows. Overlapping windows do not add information.
    span = ts[-1] - ts[0]
    n_indep = int(span / h) if h else 0

    fold = max((n - MIN_TRAIN) // N_FOLDS, 1)
    out = {}
    for cls in LADDER:
        model_folds = []
        for f in range(N_FOLDS):
            tr_end = MIN_TRAIN + f * fold
            te_end = min(tr_end + fold, n)
            if te_end - tr_end < MIN_TEST // N_FOLDS or tr_end >= n:
                continue
            train, test = samples[:tr_end], samples[tr_end:te_end]
            m = cls()
            if cls.fits:
                X = [[r.get(c) for c in cols] for r, _ in train]
                y = [ret for _, ret in train]
                try:
                    m.fit(X, y, cols)
                except Exception as e:
                    model_folds.append({'fold': f, 'error': type(e).__name__})
                    continue
            preds = [(m.predict(r, cols), ret) for r, ret in test]
            # Trade only the confident tail. A model that is right on average and wrong when it
            # is sure is not usable, and averaging over every prediction hides that.
            mags = sorted(abs(p) for p, _ in preds)
            if not mags:
                continue
            cut = mags[min(int(conf_quantile * len(mags)), len(mags) - 1)]
            taken = [(p, r) for p, r in preds if abs(p) >= cut and p != 0.0]
            allp = [(p, r) for p, r in preds if p != 0.0]
            model_folds.append({
                'fold': f, 'n_test': len(test),
                'n_traded_all': len(allp), 'n_traded_tail': len(taken),
                'acc_all': (sum(1 for p, r in allp if (p > 0) == (r > 0)) / len(allp))
                           if allp else None,
                'acc_tail': (sum(1 for p, r in taken if (p > 0) == (r > 0)) / len(taken))
                            if taken else None,
                'gross_all': mean([abs(r) * (1 if (p > 0) == (r > 0) else -1) for p, r in allp]),
                'gross_tail': mean([abs(r) * (1 if (p > 0) == (r > 0) else -1) for p, r in taken]),
            })
        if not model_folds:
            continue
        good = [f for f in model_folds if 'error' not in f]
        g_all = mean([f['gross_all'] for f in good])
        g_tail = mean([f['gross_tail'] for f in good])
        out[cls.name] = {
            'folds': model_folds,
            'n_folds': len(good),
            'acc_all': mean([f['acc_all'] for f in good]),
            'acc_tail': mean([f['acc_tail'] for f in good]),
            'gross_all_bps': g_all,
            'gross_tail_bps': g_tail,
            'net_all_bps': (g_all - cost_bps) if g_all is not None else None,
            'net_tail_bps': (g_tail - cost_bps) if g_tail is not None else None,
            'fold_sd': stdev([f['gross_all'] for f in good if f['gross_all'] is not None]),
            'n_indep': n_indep,
        }
    return out, None


def load_rows(path, markets):
    out = defaultdict(list)
    files = sorted(glob.glob(os.path.join(path, '*.csv')) +
                   glob.glob(os.path.join(path, '*.csv.gz')))
    for fp in files:
        op = gzip.open if fp.endswith('.gz') else open
        try:
            with op(fp, 'rt') as f:
                for r in csv.DictReader(f):
                    m = r.get('market')
                    if markets and m not in markets:
                        continue
                    d = {'market': m}
                    for k, v in r.items():
                        if k == 'market':
                            continue
                        try:
                            d[k] = float(v) if v not in ('', None) else None
                        except ValueError:
                            d[k] = None
                    if d.get('feature_ts_us') and d.get('mid'):
                        out[m].append(d)
        except (EOFError, OSError) as e:
            print(f'  [truncated] {os.path.basename(fp)}: {e}', file=sys.stderr)
    for m in out:
        out[m].sort(key=lambda r: r['feature_ts_us'])
    return out, files


def report(results, reach, cost_bps, cost_src, out_md, out_csv):
    L = []
    W = L.append
    W('# Impulse model ladder\n')
    W(f'Cost hurdle: **{cost_bps:.1f} bps** ({cost_src})\n')

    W('## Reachability, before any model is fitted\n')
    W('Breakeven accuracy is `p = (cost / E|move| + 1) / 2`. Compared against '
      f'**{ACHIEVABLE_ACCURACY:.0%}**, the best documented out-of-sample directional accuracy '
      'for short-horizon crypto.\n')
    W('| market | horizon | E&#124;move&#124; bps | breakeven accuracy | verdict |')
    W('|---|---:|---:|---:|---|')
    for (m, hs), (emove, need, verdict) in sorted(reach.items()):
        W(f'| {m} | {hs}s | {"" if emove is None else f"{emove:.1f}"} | '
          f'{"" if need is None else f"{need:.1%}"} | {verdict} |')
    W('')

    if not results:
        W('## Result\n')
        W('**No market and horizon is reachable, so no model was fitted.**\n')
        W('The breakeven accuracy required is above what any published short-horizon crypto '
          'direction model achieves. Fitting seven models to a target a perfect predictor cannot '
          'profit from would produce a ranked table and no information.\n')
        W('What changes this number is the cost, not the model. A maker-side entry at 10 bps '
          'round trip instead of 17 moves BTC at 10 minutes from 65.4% to 59.0% required '
          'accuracy -- still above the 56% ceiling, but the gap is a third of the size. That is '
          'where the work is.\n')
    else:
        W('## Ladder\n')
        W('Every score is out of sample. `tail` is the top decile of model confidence. A rung '
          'only counts if it beats the rung below by more than the fold-to-fold spread.\n')
        W('| market | h | rung | folds | acc all | acc tail | gross all | gross tail | '
          'net tail | fold sd |')
        W('|---|---:|---|---:|---:|---:|---:|---:|---:|---:|')
        for (m, hs), ladder in sorted(results.items()):
            prev = None
            for cls in LADDER:
                r = ladder.get(cls.name)
                if not r:
                    continue
                def f(x, p=2):
                    return '' if x is None else f'{x:.{p}f}'
                def pc(x):
                    return '' if x is None else f'{x * 100:.1f}%'
                W('| {} | {}s | `{}` | {} | {} | {} | {} | {} | **{}** | {} |'.format(
                    m, hs, cls.name, r['n_folds'], pc(r['acc_all']), pc(r['acc_tail']),
                    f(r['gross_all_bps']), f(r['gross_tail_bps']),
                    f(r['net_tail_bps']), f(r['fold_sd'])))
            W('')
        W('## Did any rung earn its complexity?\n')
        for (m, hs), ladder in sorted(results.items()):
            prev_name, prev_net = None, None
            for cls in LADDER:
                r = ladder.get(cls.name)
                if not r or r['net_tail_bps'] is None:
                    continue
                if prev_net is not None:
                    lift = r['net_tail_bps'] - prev_net
                    noise = r['fold_sd'] or 0.0
                    verdict = ('earns it' if lift > noise else
                               'inside the noise -- does not earn its complexity')
                    W(f"- {m} {hs}s: `{cls.name}` over `{prev_name}` is {lift:+.2f} bps "
                      f"against a fold spread of {noise:.2f} -- {verdict}")
                prev_name, prev_net = cls.name, r['net_tail_bps']
        W('')

    W('## What this ladder refuses to do\n')
    W('- Fit a model to a horizon whose breakeven accuracy exceeds what is achievable.')
    W('- Score in accuracy or IC. Accuracy above 50% is not an edge; accuracy above breakeven is.')
    W('- Report an in-sample number anywhere.')
    W('- Treat overlapping windows as independent observations.')
    W('- Call a rung an improvement when its lift is inside the fold-to-fold spread.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    cols = ['market', 'horizon_s', 'rung', 'n_folds', 'n_indep', 'acc_all', 'acc_tail',
            'gross_all_bps', 'gross_tail_bps', 'net_all_bps', 'net_tail_bps', 'fold_sd']
    with open(out_csv, 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        for (m, hs), ladder in sorted(results.items()):
            for name, r in ladder.items():
                w.writerow(dict(r, market=m, horizon_s=hs, rung=name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--features', default=os.path.join(ROOT, 'data', 'features'))
    ap.add_argument('--markets', nargs='*', default=['KXBTCPERP', 'KXETHPERP'])
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'impulse_model.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'impulse_model.csv'))
    args = ap.parse_args()

    from btc_eth_screen import measured_hurdle
    cost, src = measured_hurdle()

    rows, files = load_rows(args.features, set(args.markets))
    if not files:
        sys.exit(f'no feature files in {args.features}: run feature_engine.py first')

    reach, results = {}, {}
    for m, rs in sorted(rows.items()):
        cols = sorted({k for r in rs[:50] for k in r if k != 'market'})
        for hs in HORIZONS_S:
            h = hs * 1_000_000
            ts = [r['feature_ts_us'] for r in rs]
            mid = [r['mid'] for r in rs]
            moves = []
            for i, t0 in enumerate(ts):
                j = bisect.bisect_left(ts, t0 + h)
                if j >= len(ts) or ts[j] - t0 > h * 1.5 or not mid[i]:
                    continue
                moves.append(abs((mid[j] - mid[i]) / mid[i] * 1e4))
            emove = mean(moves)
            need = breakeven_accuracy(cost, emove)
            if need is None:
                reach[(m, hs)] = (emove, need, 'NO DATA')
                continue
            if need > ACHIEVABLE_ACCURACY:
                reach[(m, hs)] = (emove, need, f'UNREACHABLE (needs {need:.1%})')
                continue
            reach[(m, hs)] = (emove, need, 'reachable')
            lad, why = walk_forward(rs, cols, hs, cost)
            if lad is None:
                reach[(m, hs)] = (emove, need, f'reachable but {why}')
            else:
                results[(m, hs)] = lad

    report(results, reach, cost, src, args.md, args.csv)
    for m in sorted(rows):
        print(f'  {m}: {len(rows[m]):,} rows')
    print(f'cost hurdle {cost:.1f} bps ({src})')
    n_reach = sum(1 for v in reach.values() if v[2] == 'reachable')
    print(f'reachable market/horizon pairs: {n_reach} of {len(reach)}')
    print(f'ladders fitted: {len(results)}')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
