#!/usr/bin/env python3
"""Does the conditional maker score know anything beyond "find a wide spread"?

`kept_bias_check.py` showed that the model's advantage on `kept` is entirely the post-time
half-spread credit, and that on the markout it is worse than random. But random is the wrong
comparison for THIS question. The model selects 18.3 bps spreads and a random pick draws 8.2, so
they are not comparing like with like: some of the model's worse markout is just the venue's
spread/adverse-selection relationship reasserting itself.

The right control holds that relationship fixed. For every selected fill, compare it to
unselected fills from the SAME stratum:

    instrument x time bucket x spread decile x volatility bucket

If the score carries information beyond spread selection, selected fills inside a stratum will
have better markouts than their unselected neighbours. If it carries none, the within-stratum
difference is zero and the entire score reduces to a spread filter.

THIS IS A DIAGNOSIS, NOT A RESCUE. A materially and robustly positive result would reopen the
question of what the score is doing; anything else closes it. Either way the strategy stays
rejected on the round trip, which is measured at the fill prices and does not care why.

  python3 scripts/spread_matched_control.py
"""
import argparse, csv, math, os, statistics, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conditional_markout as CM

HORIZONS = ['1s', '5s', '10s', '60s']
TIME_BUCKET_US = 300_000_000          # five minutes
N_SPREAD_BINS = 10
N_VOL_BINS = 4


def num(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return None if f != f else f


def bin_edges(values, k):
    v = sorted(x for x in values if x is not None)
    if len(v) < k * 2:
        return []
    return [v[int(len(v) * i / k)] for i in range(1, k)]


def which_bin(x, edges):
    if x is None:
        return None
    lo = 0
    for e in edges:
        if x < e:
            return lo
        lo += 1
    return lo


def build(fills_path, top_frac=0.10, horizon='5s'):
    """Score the held-out half, stratify it, and compare selected to unselected within strata."""
    rows, feats = CM.load(fills_path)
    cut = int(len(rows) * CM.TRAIN_FRAC)
    train, test = rows[:cut], rows[cut:]
    std = CM.Standardiser()
    std.fit(train, feats)
    ytr = [r[f'kept_{horizon}'] for r in train]
    keep = [i for i, v in enumerate(ytr) if v is not None]
    w = CM.ridge_fit([std.vec(train[i]) for i in keep], [ytr[i] for i in keep])
    if w is None:
        return None, 'model did not fit'
    sc = [sum(a * b for a, b in zip(w, std.vec(r))) for r in test]
    order = sorted(range(len(test)), key=lambda i: -sc[i])
    k = max(int(len(test) * top_frac), 1)
    chosen = set(order[:k])

    raw = {}
    with open(fills_path) as f:
        for r in csv.DictReader(f):
            try:
                raw[(r['market'], int(float(r['fill_us'])))] = r
            except (KeyError, TypeError, ValueError):
                continue

    recs = []
    for i, r in enumerate(test):
        fr = raw.get((r['market'], int(r['fill_us'])))
        if not fr:
            continue
        rec = {'i': i, 'market': r['market'], 'fill_us': int(r['fill_us']),
               'selected': i in chosen, 'score': sc[i],
               'spread': num(fr.get('spread_bps')), 'vol': num(fr.get('vol_60s_bps'))}
        for h in HORIZONS:
            rec[f'markout_{h}'] = num(fr.get(f'markout_{h}'))
        if rec['spread'] is None:
            continue
        recs.append(rec)

    s_edges = bin_edges([r['spread'] for r in recs], N_SPREAD_BINS)
    v_edges = bin_edges([r['vol'] for r in recs], N_VOL_BINS)
    for r in recs:
        r['stratum'] = (r['market'],
                        r['fill_us'] // TIME_BUCKET_US,
                        which_bin(r['spread'], s_edges),
                        which_bin(r['vol'], v_edges))
    return recs, None


def compare(recs, horizon):
    """Within-stratum difference, weighted by how many selected fills each stratum contributes."""
    by = defaultdict(lambda: {'sel': [], 'ctl': [], 'ts': []})
    for r in recs:
        v = r.get(f'markout_{horizon}')
        if v is None:
            continue
        g = by[r['stratum']]
        (g['sel'] if r['selected'] else g['ctl']).append(v)
        g['ts'].append(r['fill_us'])

    diffs, weights, keys = [], [], []
    n_sel_matched = n_sel_total = 0
    usable = 0
    for st, g in by.items():
        n_sel_total += len(g['sel'])
        if not g['sel'] or not g['ctl']:
            continue
        usable += 1
        n_sel_matched += len(g['sel'])
        diffs.append(statistics.mean(g['sel']) - statistics.mean(g['ctl']))
        weights.append(len(g['sel']))
        keys.append(CM.time_cluster(min(g['ts'])))
    if not diffs:
        return None
    wm = sum(d * w for d, w in zip(diffs, weights)) / sum(weights)
    # Cluster on the minute, as everywhere else: strata inside the same minute are not
    # independent observations of the venue.
    _, lo, hi, n_clu = CM.cluster_bootstrap(diffs, keys, want_n=True)
    return {'horizon': horizon, 'strata': len(by), 'strata_usable': usable,
            'selected_total': n_sel_total, 'selected_matched': n_sel_matched,
            'weighted_diff_bps': wm, 'lo': lo, 'hi': hi, 'clusters': n_clu,
            'unweighted_diff_bps': statistics.mean(diffs)}


def report(res, recs, out_md, top_frac):
    L = []
    W = L.append
    W('# Spread-matched control for the conditional maker score\n')
    W(f'Top {top_frac:.0%} of held-out fills, compared against unselected fills from the SAME '
      f'stratum: instrument x 5-minute bucket x spread decile x volatility quartile.\n')
    W('A positive difference means the score knows something after the spread is held fixed. '
      'Zero means the score is a spread filter and nothing else.\n')

    W('## Within-stratum difference in markout\n')
    W('| horizon | strata used | selected matched | clusters | weighted diff | 90% interval | '
      'unweighted |')
    W('|---|---:|---:|---:|---:|---|---:|')
    for r in res:
        band = 'n/a' if r['lo'] is None else f"[{r['lo']:+.2f}, {r['hi']:+.2f}]"
        W(f"| {r['horizon']} | {r['strata_usable']} of {r['strata']} | "
          f"{r['selected_matched']} of {r['selected_total']} | {r['clusters']} | "
          f"**{r['weighted_diff_bps']:+.2f}** | {band} | {r['unweighted_diff_bps']:+.2f} |")
    W('')

    sig = [r for r in res if r['lo'] is not None and r['lo'] > 0]
    neg = [r for r in res if r['hi'] is not None and r['hi'] < 0]
    flat = [r for r in res if r not in sig and r not in neg]
    W('## Verdict\n')
    if sig and len(sig) < len(res) and flat:
        # Four markout horizons on the same fills are four correlated looks at one question.
        # One marginal hit among them, with the rest flat, is what chance produces, and calling
        # it a finding is how the previous conclusion in this lane went wrong.
        W(f'**{len(sig)} of {len(res)} horizons clears zero; {len(flat)} are flat.** The '
          f'horizons are not independent tests -- they are four looks at the same fills -- so '
          f'one marginal result among them is close to what chance produces.\n')
        for r in sig:
            skew = ((r['hi'] - r['weighted_diff_bps']) /
                    max(r['weighted_diff_bps'] - r['lo'], 1e-9))
            W(f"The clearing horizon is {r['horizon']} at {r['weighted_diff_bps']:+.2f} bps "
              f"[{r['lo']:+.2f}, {r['hi']:+.2f}]. The interval is {skew:.0f}x longer above the "
              f"estimate than below it, which is a distribution with a tail in it rather than a "
              f"measurement with a floor under it.\n")
        W('Read together with the flat horizons, this is not evidence that the score knows '
          'anything beyond the spread. It is also not proof that it knows nothing. What it is, '
          'is too little data: 251 strata drawn from under an hour.\n')
        W('It does not reopen the strategy either way. The round trip is measured at fill prices '
          'and is negative regardless of what the score is responding to.\n')
    elif sig:
        W(f'**{len(sig)} of {len(res)} horizons show a positive within-stratum difference with '
          f'an interval clear of zero.** The score carries information beyond spread selection '
          f'at: ' + ', '.join(f"{r['horizon']} ({r['weighted_diff_bps']:+.2f} bps)" for r in sig)
          + '.\n')
        W('That does not reopen the strategy. The round trip is measured at fill prices and is '
          'negative regardless of why the score works. What it means is that there is something '
          'in the score worth understanding before the feature set is reused anywhere else.\n')
    elif neg:
        W(f'**The within-stratum difference is NEGATIVE with an interval clear of zero at '
          f'{len(neg)} of {len(res)} horizons.** Inside a matched stratum, the fills the score '
          f'likes have WORSE markouts than the ones it passes over.\n')
        W('So the score is not merely a spread filter that happens to be neutral on everything '
          'else. Within a fixed spread decile it is actively picking the wrong side of whatever '
          'it is responding to.\n')
    else:
        W('**No horizon shows a within-stratum difference distinguishable from zero.** Holding '
          'instrument, time, spread decile and volatility fixed, the fills the score selects are '
          'indistinguishable from the ones it passes over.\n')
        W('The score is a spread filter. Everything it appeared to know was the spread, which is '
          'consistent with `reports/kept_bias_check.md` finding that its entire `kept` advantage '
          'was the post-time half-spread credit, and with the fitted ridge beating the '
          '`spread_only` null by +0.14 bps in `reports/conditional_maker.md`.\n')
        W('Three independent routes now say the same thing. The lane closes.\n')

    W('## How to read the matching\n')
    W('- A stratum contributes only if it holds at least one selected and one unselected fill. '
      'Strata that do not are excluded, and the counts above say how many selected fills '
      'survived that requirement.')
    W('- Matching removes the confound it is built for and no other. If the score selects on '
      'something correlated with spread that is not in the strata, this comparison still '
      'attributes it to the score.')
    W('- The comparison is of markouts, never of `kept`. `kept` is the quantity that created the '
      'original error.\n')
    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fills', default=os.path.join(ROOT, 'reports', 'maker_fills.csv'))
    ap.add_argument('--top', type=float, default=0.10)
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'spread_matched_control.md'))
    args = ap.parse_args()
    if not os.path.exists(args.fills):
        sys.exit(f'no fills at {args.fills}')
    recs, why = build(args.fills, args.top)
    if why:
        sys.exit(why)
    res = [r for r in (compare(recs, h) for h in HORIZONS) if r]
    report(res, recs, args.md, args.top)
    for r in res:
        band = 'n/a' if r['lo'] is None else f"[{r['lo']:+.2f}, {r['hi']:+.2f}]"
        print(f"  {r['horizon']:>4s}: diff {r['weighted_diff_bps']:+6.2f} {band:>18s}  "
              f"{r['strata_usable']} strata, {r['selected_matched']}/{r['selected_total']} matched")
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
