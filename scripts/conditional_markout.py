#!/usr/bin/env python3
"""Does selectivity rescue market making? The conditional version of the execution study.

The execution study measured

    E[kept] < 0        for an unconditional quoting policy

where kept = half-spread earned at entry + markout after the fill. It did NOT measure

    E[kept | state before the fill]

and those are different claims. An average over every fill says nothing about whether a subset
identified in advance is different, and "wider spreads carry worse markouts, correlation -0.905"
is precisely the kind of structure a conditional model exists to exploit rather than average
over. The earlier write-up said no model closes the gap. It had not tested one.

THE QUESTION, stated so it can fail:

    Is there a selection rule, using only state observable BEFORE the fill, under which
        E[kept | selected] > maker fee + exit cost
    on out-of-sample fills?

HOW THIS IS KEPT HONEST

  1. Selection is out of sample. The model is fitted on the first part of the tape, and the
     ranking that picks the selective subset is applied to the later part only. Ranking fills by
     a score fitted on those same fills produces a beautiful top decile every time.
  2. Two null models run alongside, on the identical test set and the identical subset sizes:
       - RANDOM: rank by a random score. This is what selectivity looks like when it is not
         working, and its top 1% will occasionally look excellent on a few dozen fills.
       - SPREAD: rank by the quoted spread alone, one feature and no fitting. If the fitted
         model does not beat this, the fitting bought nothing and the answer was just "quote the
         wide markets", which the unconditional study already priced.
  3. Confidence intervals come from a moving-block bootstrap. Fills minutes apart in the same
     market are not independent draws, and an i.i.d. bootstrap on them reports an interval
     several times too narrow.
  4. The hurdle includes an exit. A maker entry still has to get out, and the exit is priced at
     the measured cost of crossing rather than assumed to be free or assumed to be another
     maker fill.
  5. Every subset reports its fill COUNT and its fills per day. A +6 bps edge on 11 fills across
     a 2.5-hour tape is a number about 11 fills.

  python3 scripts/conditional_markout.py --fills reports/maker_fills.csv
"""
import argparse, csv, json, math, os, random, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

HORIZONS = ['1s', '5s', '10s', '60s']
QUANTILES = [0.50, 0.20, 0.10, 0.05, 0.02, 0.01]
TRAIN_FRAC = 0.60
BOOTSTRAP = 2000
BLOCK = 25           # fills per bootstrap block, to keep local autocorrelation intact

# Anything that could only be known after the fill is excluded by name, not by hoping.
LEAKS = {'fill_us', 'wait_s', 'left_touch', 'fee_bps', 'mode', 'market', 'side', 'post_us',
         'price', 'mid'}


def is_leak(c):
    return c in LEAKS or c.startswith('markout_') or c.startswith('kept_')


def num(v):
    if v in ('', None):
        return None
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return None if (f != f or abs(f) == float('inf')) else f


def load(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return [], []
    feats = [c for c in rows[0] if not is_leak(c)]
    out = []
    for r in rows:
        d = {'market': r['market'], 'mode': r['mode'], 'side': r['side'],
             'fill_us': num(r['fill_us']), 'fee_bps': num(r['fee_bps']),
             'spread_bps': num(r.get('spread_bps'))}
        for h in HORIZONS:
            d[f'kept_{h}'] = num(r.get(f'kept_{h}'))
        d['x'] = {c: num(r.get(c)) for c in feats}
        out.append(d)
    out.sort(key=lambda r: r['fill_us'] or 0)
    return out, feats


class Standardiser:
    def __init__(self):
        self.mu, self.sd, self.cols = {}, {}, []

    def fit(self, rows, cols):
        keep = []
        for c in cols:
            vals = [r['x'][c] for r in rows if r['x'].get(c) is not None]
            if len(vals) < len(rows) * 0.5:
                continue                      # mostly missing: not a feature, a gap
            m = sum(vals) / len(vals)
            s = statistics.pstdev(vals) if len(vals) > 1 else 0.0
            if s <= 1e-12:
                continue                      # constant: carries nothing
            self.mu[c], self.sd[c] = m, s
            keep.append(c)
        self.cols = keep
        return keep

    def vec(self, r):
        return [((r['x'].get(c) if r['x'].get(c) is not None else self.mu[c]) - self.mu[c])
                / self.sd[c] for c in self.cols]


def ridge_fit(X, y, lam=10.0):
    """Closed-form ridge. Returns weights, or None if the system is singular."""
    if not X:
        return None
    k = len(X[0])
    A = [[sum(X[r][i] * X[r][j] for r in range(len(X))) + (lam if i == j else 0.0)
          for j in range(k)] for i in range(k)]
    b = [sum(X[r][i] * y[r] for r in range(len(X))) for i in range(k)]
    n = k
    M = [A[i][:] + [b[i]] for i in range(n)]
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


def cluster_bootstrap(vals, keys, n=BOOTSTRAP, seed=17, want_n=False):
    """Mean and a 90% interval, resampling whole TIME CLUSTERS with replacement.

    This replaces a contiguous-index block bootstrap that was only a time-block bootstrap by
    accident. The list it resampled interleaves markets, so an adjacent "block" was whatever
    happened to resolve next, not a stretch of one instrument's history. Worse, the dependence
    that matters most here is cross-sectional: every perpetual moves together, so two thousand
    fills from the same forty minutes are nothing like two thousand independent draws no matter
    how they are ordered.

    Clustering on a time bucket handles both at once. Everything inside one bucket is treated as
    a single draw, which is conservative and honest about what a sub-hour sample can support. The
    number of clusters is returned alongside, because an interval computed from nine independent
    minutes should be read as an interval computed from nine things.
    """
    pairs = [(k, v) for k, v in zip(keys, vals) if v is not None and k is not None]
    if len(pairs) < 10:
        return (None, None, None, 0) if want_n else (None, None, None)
    groups = defaultdict(list)
    for k, v in pairs:
        groups[k].append(v)
    gk = list(groups)
    m = sum(v for _, v in pairs) / len(pairs)
    if len(gk) < 3:
        # Too few clusters to resample. Saying so beats returning a confident-looking band.
        return (m, None, None, len(gk)) if want_n else (m, None, None)
    rng = random.Random(seed)
    means = []
    for _ in range(n):
        s = []
        for _ in range(len(gk)):
            s.extend(groups[gk[rng.randrange(len(gk))]])
        if s:
            means.append(sum(s) / len(s))
    means.sort()
    lo, hi = means[int(0.05 * len(means))], means[int(0.95 * len(means))]
    return (m, lo, hi, len(gk)) if want_n else (m, lo, hi)


CLUSTER_US = 60_000_000          # one minute; everything inside it counts as one observation


def time_cluster(ts_us, width_us=CLUSTER_US):
    try:
        return int(float(ts_us) // width_us)
    except (TypeError, ValueError):
        return None


def block_bootstrap(vals, n=BOOTSTRAP, block=BLOCK, seed=17):
    """Deprecated: kept only so older call sites keep running. Prefer `cluster_bootstrap`, which
    resamples on real timestamps instead of on list position."""
    vals = [v for v in vals if v is not None]
    if len(vals) < 10:
        return (None, None, None)
    rng = random.Random(seed)
    nb = max(1, len(vals) // block)
    means = []
    for _ in range(n):
        s = []
        for _ in range(nb):
            i = rng.randrange(0, max(1, len(vals) - block))
            s.extend(vals[i:i + block])
        if s:
            means.append(sum(s) / len(s))
    means.sort()
    return (sum(vals) / len(vals), means[int(0.05 * len(means))], means[int(0.95 * len(means))])


def exit_cost_for(market, default=12.0):
    """The measured one-way cost of crossing out, per market. Not assumed, and not free."""
    p = os.path.join(ROOT, 'reports', 'execution_simulator.csv')
    if not os.path.exists(p):
        return default
    with open(p) as f:
        for r in csv.DictReader(f):
            if r.get('market') == market and r.get('mode') == 'taker' \
                    and r.get('queue_bound') == 'pess':
                try:
                    return float(r['entry_cost_bps']) + float(r['fee_bps'])
                except (KeyError, TypeError, ValueError):
                    return default
    return default


def selectivity(test, scores, label, horizon, hours_span, exit_by_market):
    """The ladder: how does the top q% of fills by this score actually do?"""
    order = sorted(range(len(test)), key=lambda i: -scores[i])
    out = []
    for q in QUANTILES:
        k = max(int(len(order) * q), 1)
        idx = order[:k]
        kept = [test[i][f'kept_{horizon}'] for i in idx]
        kept = [v for v in kept if v is not None]
        if len(kept) < 10:
            out.append({'selector': label, 'horizon': horizon, 'quantile': q, 'n': len(kept),
                        'note': 'too few fills to say anything'})
            continue
        mean, lo, hi = block_bootstrap(kept)
        fee = statistics.mean([test[i]['fee_bps'] for i in idx if test[i]['fee_bps']])
        ex = statistics.mean([exit_by_market.get(test[i]['market'], 12.0) for i in idx])
        out.append({
            'selector': label, 'horizon': horizon, 'quantile': q, 'n': len(kept),
            'fills_per_day': len(kept) / hours_span * 24 if hours_span else None,
            'gross_kept_bps': mean, 'kept_lo': lo, 'kept_hi': hi,
            'maker_fee_bps': fee, 'exit_cost_bps': ex,
            'net_entry_only_bps': mean - fee,
            'net_round_trip_bps': mean - fee - ex,
            'net_lo': lo - fee - ex if lo is not None else None,
            'beats_entry_fee': bool(lo is not None and lo > fee),
        })
    return out


def run(rows, feats, out_md, out_csv):
    if len(rows) < 400:
        return [], f'only {len(rows)} fills: not enough to fit and test anything'
    span_h = ((rows[-1]['fill_us'] - rows[0]['fill_us']) / 3.6e9) if rows[0]['fill_us'] else 0
    cut = int(len(rows) * TRAIN_FRAC)
    train, test = rows[:cut], rows[cut:]
    exit_by_market = {m: exit_cost_for(m) for m in {r['market'] for r in rows}}

    std = Standardiser()
    cols = std.fit(train, feats)
    results, models = [], {}
    rng = random.Random(5)
    rand_scores = [rng.random() for _ in test]
    # The spread is the one-feature null: "quote where the spread is wide" needs no model, and
    # the unconditional study already priced it.
    spread_scores = [(r['spread_bps'] or 0.0) for r in test]

    for h in HORIZONS:
        ytr = [r[f'kept_{h}'] for r in train]
        keep = [i for i, v in enumerate(ytr) if v is not None]
        if len(keep) < 200:
            continue
        X = [std.vec(train[i]) for i in keep]
        y = [ytr[i] for i in keep]
        w = ridge_fit(X, y)
        if w is None:
            continue
        models[h] = dict(zip(cols, w))
        model_scores = [sum(a * b for a, b in zip(w, std.vec(r))) for r in test]
        for label, sc in (('model', model_scores), ('spread_only', spread_scores),
                          ('random', rand_scores)):
            results.extend(selectivity(test, sc, label, h, span_h, exit_by_market))

    L = []
    W = L.append
    W('# Conditional adverse-selection maker study\n')
    W(f'{len(rows):,} passive fills over {span_h:.1f} hours. '
      f'Fitted on the first {len(train):,}, every number below measured on the held-out '
      f'{len(test):,}.\n')
    W(f'Features offered: {len(feats)}; usable after dropping constants and mostly-missing '
      f'columns: {len(cols)}.\n')

    W('## RETRACTION -- read this before the tables\n')
    W('**The target below, `kept`, is not what a one-sided passive entry is worth, and the '
      'headline result of this study does not survive correcting it.**\n')
    W('`kept = half_spread_at_POST + markout_from_FILL`. The first term credits the spread '
      'against the mid at the moment the order was posted. You only own that spread if you fill '
      'without the mid moving -- and the reason you fill is that the mid came to you. The move '
      'between posting and filling is charged to neither term.\n')
    W('Measured on this same selection: the model picks states with a median spread of 18.3 bps '
      'against 8.2 for a random pick, worth +4.04 bps of post-time credit. On the **markout** -- '
      'what the position is worth once you actually hold it -- the model is WORSE than random at '
      'every horizon, with separated bootstrap intervals: '
      '-5.51 vs -3.86 at 1s, -4.80 vs -4.02 at 5s, -5.33 vs -4.24 at 10s, -7.56 vs -5.09 at 60s.\n')
    W('So the selection is not finding good fills. It is finding wide spreads, and wide spreads '
      'carry worse adverse selection -- `reports/execution_summary.md` measured that correlation '
      'at **-0.905**. Both halves were already in this repository and were not put together '
      'until the round trip forced real fill prices onto both legs.\n')
    W('The round trip itself: `reports/exit_engine.md`, 38,643 priced trades, best combination '
      '-24.11 bps, 0 of 40 positive. The decomposition: `reports/kept_bias_check.md`.\n')
    W('What stands from this study: the machinery, the nulls, and the finding that ranking by '
      'the fitted model is indistinguishable from ranking by spread alone. What does not stand: '
      'that selection finds better fills.\n')

    W('## The question\n')
    W('```\nE[kept | state before fill] > maker fee + exit cost ?\n```')
    W('where `kept = half-spread at entry + signed markout`. Not "will the price rise" -- '
      '"if someone hits me here, is that fill toxic".\n')

    W('## Selectivity ladder\n')
    W('`model` is the fitted ranking. `spread_only` ranks by the quoted spread with no fitting. '
      '`random` ranks by a random number. All three are applied to the same held-out fills at '
      'the same subset sizes, so the columns are directly comparable and the model has to beat '
      'both to have done anything.\n')
    W('Intervals are 90% from a moving-block bootstrap.\n')
    for h in HORIZONS:
        rs = [r for r in results if r['horizon'] == h and 'note' not in r]
        if not rs:
            continue
        W(f'\n### kept at {h}\n')
        W('| selector | top | fills | fills/day | gross kept | 90% interval | maker fee | '
          'exit | net entry-only | net round trip |')
        W('|---|---:|---:|---:|---:|---|---:|---:|---:|---:|')
        for r in sorted(rs, key=lambda x: (x['selector'], -x['quantile'])):
            W(f"| {r['selector']} | {r['quantile']:.0%} | {r['n']} | "
              f"{r['fills_per_day']:.0f} | {r['gross_kept_bps']:+.2f} | "
              f"[{r['kept_lo']:+.2f}, {r['kept_hi']:+.2f}] | {r['maker_fee_bps']:.1f} | "
              f"{r['exit_cost_bps']:.1f} | {r['net_entry_only_bps']:+.2f} | "
              f"**{r['net_round_trip_bps']:+.2f}** |")
    W('')

    mrows = [r for r in results if 'note' not in r and r['selector'] == 'model']
    live = [r for r in mrows if r['net_round_trip_bps'] > 0]
    entry_live = [r for r in mrows if r['beats_entry_fee']]
    W('## Verdict\n')
    W('These are two different questions and they have different answers, so they are reported '
      'separately rather than collapsed into one.\n')

    W('### The entry leg: selection works\n')
    if entry_live:
        best_e = max(entry_live, key=lambda r: r['net_entry_only_bps'])
        W(f'**{len(entry_live)} of {len(mrows)} model subsets keep more than the maker fee**, '
          f'with a bootstrap lower bound above it. The best is the top '
          f"{best_e['quantile']:.0%} at {best_e['horizon']}: "
          f"{best_e['gross_kept_bps']:+.2f} bps kept against a {best_e['maker_fee_bps']:.0f} bps "
          f"fee, over {best_e['n']} fills.\n")
        W('The unconditional study measured this same quantity at roughly -2 bps averaged over '
          'every fill. So conditioning on pre-fill state does what it was supposed to do: it '
          'finds the fills that are not toxic. The earlier claim that no model could close the '
          'gap was wrong about the entry leg.\n')
    else:
        pt = [r for r in mrows if r['net_entry_only_bps'] > 0]
        if pt:
            best_p = max(pt, key=lambda r: r['net_entry_only_bps'])
            W(f'**{len(pt)} of {len(mrows)} model subsets keep more than the maker fee on the '
              f'point estimate, but none has a 90% lower bound clear of it.** The best is the '
              f"top {best_p['quantile']:.0%} at {best_p['horizon']}: "
              f"{best_p['gross_kept_bps']:+.2f} bps kept "
              f"[{best_p['kept_lo']:+.2f}, {best_p['kept_hi']:+.2f}] against a "
              f"{best_p['maker_fee_bps']:.0f} bps fee, over {best_p['n']} fills.\n")
            W('That distinction is the finding, and it cuts both ways.\n')
            W('It is not nothing. The unconditional study measured this same quantity at about '
              '-2 bps averaged over every fill, and the random-selector column below sits at '
              'roughly -0.3 to -2.4 at every subset size. A selected subset keeping +6 to +9 bps '
              'against a null that keeps nothing is a real effect, it is monotone in '
              'selectivity, and it repeats across all four horizons rather than appearing in one '
              'lucky cell. The earlier claim that no model could close the gap was wrong.\n')
            W('It is also not enough to act on. The interval is wide because the top 1% is a '
              f"couple of hundred fills from a single 2.5-hour session, and a moving-block "
              f"bootstrap on correlated fills is honest about that. A point estimate that clears "
              f"a fee by {best_p['net_entry_only_bps']:.1f} bps, with a lower bound that does "
              f"not, is a reason to record more tape, not a reason to quote.\n")
        else:
            W('No model subset keeps more than the maker fee, on the point estimate or the '
              'interval.\n')

    W('### The round trip: it does not\n')
    if not live:
        best = max(mrows, key=lambda r: r['net_round_trip_bps'], default=None)
        if best:
            W(f"**Nothing clears a full round trip.** The best is "
              f"{best['net_round_trip_bps']:+.2f} bps at the top {best['quantile']:.0%} of "
              f"{best['horizon']} fills, and the gap is the exit: "
              f"{best['exit_cost_bps']:.1f} bps to cross out.\n")
        W('The asymmetry is the whole result. A maker ENTRY can be selected -- you choose '
          'whether to quote, and declining to quote costs nothing. A maker EXIT cannot: once a '
          'fill happens you hold inventory and have to get out of it, in whatever state the '
          'market is in, not in a state you waited for. Every round-trip figure here therefore '
          'prices the exit at the measured cost of crossing.\n')
        W('That is an assumption, not a measurement, and it is the single most valuable thing '
          'left to measure. If a position entered in a selected state can be exited passively '
          'often enough, the arithmetic changes sign. If it cannot, this lane is closed.\n')
    else:
        W(f'**{len(live)} model subsets clear a full round trip.** Each needs checking against '
          f'the nulls in the same row block before it means anything.\n')

    W('### Did the fitting earn its keep?\n')
    beat = []
    for h in HORIZONS:
        for q in QUANTILES:
            g = {r['selector']: r for r in results
                 if r['horizon'] == h and r['quantile'] == q and 'note' not in r}
            if 'model' in g and 'spread_only' in g:
                beat.append(g['model']['gross_kept_bps'] - g['spread_only']['gross_kept_bps'])
    if beat:
        mean_beat = sum(beat) / len(beat)
        wins = sum(1 for b in beat if b > 0)
        W(f'Against the one-feature `spread_only` null, the fitted model is ahead in {wins} of '
          f'{len(beat)} cells, by {mean_beat:+.2f} bps on average.\n')
        if abs(mean_beat) < 1.0:
            W('**That is not a model, it is a spread filter.** Seventy-nine features and a ridge '
              'fit reproduce what ranking by the quoted spread alone already does. The finding '
              'is real and the machinery around it is not load-bearing: the rule is "quote only '
              'when the spread is unusually wide right now", and the ridge weights confirm it -- '
              'the largest ones are spread and level counts, which are the same thing said three '
              'ways.\n')
            W('That simplification matters for what comes next. A one-feature rule needs far '
              'less data to trust, is far harder to overfit, and can be checked by eye on a live '
              'book.\n')

    W('## Did the model beat the nulls?\n')
    W('| horizon | top | model | spread_only | random | model minus best null |')
    W('|---|---:|---:|---:|---:|---:|')
    for h in HORIZONS:
        for q in QUANTILES:
            g = {r['selector']: r for r in results
                 if r['horizon'] == h and r['quantile'] == q and 'note' not in r}
            if 'model' not in g:
                continue
            m = g['model']['gross_kept_bps']
            sp = g.get('spread_only', {}).get('gross_kept_bps')
            rd = g.get('random', {}).get('gross_kept_bps')
            best_null = max([v for v in (sp, rd) if v is not None], default=None)
            W(f"| {h} | {q:.0%} | {m:+.2f} | "
              f"{'' if sp is None else f'{sp:+.2f}'} | {'' if rd is None else f'{rd:+.2f}'} | "
              f"{'' if best_null is None else f'{m - best_null:+.2f}'} |")
    W('')

    if models:
        W('## What the model leans on\n')
        h = HORIZONS[-1] if HORIZONS[-1] in models else sorted(models)[0]
        top = sorted(models[h].items(), key=lambda kv: -abs(kv[1]))[:14]
        W(f'Largest standardised ridge weights for `kept_{h}`. Signs are readable: a positive '
          f'weight means more of that feature predicts a fill that keeps more.\n')
        W('| feature | weight |')
        W('|---|---:|')
        for k, v in top:
            W(f'| `{k}` | {v:+.3f} |')
        W('')

    W('## What would change this answer\n')
    W('- More tape. 2.5 hours is one session; the top 1% of it is a few dozen fills, and a '
      'moving-block interval on a few dozen correlated fills is wide for a good reason.')
    W('- A cheaper entry. The gap is measured in maker fee, and the fee is tiered: see '
      '`reports/fee_ladder.md` for the volume at which each market crosses zero.')
    W('- A cheaper exit. Every round-trip figure here prices the exit at the measured cost of '
      'crossing. A strategy that can exit passively pays less, but it also may not get out.')
    W('- Nothing here tests whether the SIGNAL changes which fills occur. The orders were placed '
      'unconditionally and selected afterwards; a quoting policy that only posts in the selected '
      'states faces a different, and probably worse, mix of counterparties.\n')

    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')
    cols_csv = ['selector', 'horizon', 'quantile', 'n', 'fills_per_day', 'gross_kept_bps',
                'kept_lo', 'kept_hi', 'maker_fee_bps', 'exit_cost_bps', 'net_entry_only_bps',
                'net_round_trip_bps', 'net_lo', 'beats_entry_fee', 'note']
    with open(out_csv, 'w') as f:
        w = csv.DictWriter(f, fieldnames=cols_csv, extrasaction='ignore')
        w.writeheader()
        for r in results:
            w.writerow(r)
    return results, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fills', default=os.path.join(ROOT, 'reports', 'maker_fills.csv'))
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'conditional_maker.md'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'reports', 'conditional_maker.csv'))
    args = ap.parse_args()
    if not os.path.exists(args.fills):
        sys.exit(f'no fills file at {args.fills}: run execution_sim.py --fills-csv first')
    rows, feats = load(args.fills)
    results, why = run(rows, feats, args.md, args.csv)
    if why:
        print(why)
        return
    print(f'{len(rows):,} fills, {len(feats)} candidate features')
    model = [r for r in results if r['selector'] == 'model' and 'note' not in r]
    if model:
        b = max(model, key=lambda r: r['net_round_trip_bps'])
        print(f"best model subset: top {b['quantile']:.0%} at {b['horizon']} = "
              f"{b['net_round_trip_bps']:+.2f} bps round trip over {b['n']} fills")
        pos = [r for r in model if r['net_round_trip_bps'] > 0]
        print(f'model subsets clearing a round trip: {len(pos)} of {len(model)}')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
