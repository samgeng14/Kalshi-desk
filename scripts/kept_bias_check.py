#!/usr/bin/env python3
"""Does `kept` measure what a passive entry is worth? No, and this is the check that shows it.

`kept` was the target of the conditional maker study:

    kept_h = half_spread_at_POST + markout_h_from_FILL

Both terms are well defined. The sum is not what a one-sided passive entry is worth, and the
difference matters enough to have reversed that study's conclusion.

The half-spread term is credited against the mid at the moment the order was POSTED. You only
own that spread if you fill without the mid having moved. But the reason you fill is that the
mid came to you, so by the time you own the position the reference has already moved against
you -- and `kept` never charges that move. It is invisible to the formula, sitting in the gap
between the two terms.

The honest quantity for a one-sided entry is the markout alone: what the position is worth, at
the mid, once you actually hold it, against the price you actually paid. That is also what the
round-trip accounting in `exit_engine.py` is forced to use, because an exit has to be priced off
real fills on both legs and there is nowhere to put a post-time reference.

WHY THIS MATTERED. The model selects wide-spread states. Wide spreads carry a LARGER post-time
half-spread and, by the execution study's own measurement, WORSE markouts -- the correlation
between quoted spread and 60-second markout is -0.905. `kept` adds a term the model can inflate
to a term the model makes worse, and the two nearly cancel. What is left looks like alpha.

Both facts were already in this repository, in two different reports, and were not put together.

  python3 scripts/kept_bias_check.py
"""
import argparse, csv, os, statistics, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conditional_markout as CM

HORIZONS = ['1s', '5s', '10s', '60s']


def num(v):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return None if f != f else f


def decompose(fills_path, top_frac=0.10, horizon='5s', seed=5):
    """Split `kept` into its two terms, for the model's selection and for a random one."""
    import random
    rows, feats = CM.load(fills_path)
    if len(rows) < 400:
        return None, 'not enough fills'
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
    rnd = list(range(len(test)))
    random.Random(seed).shuffle(rnd)

    raw = {}
    with open(fills_path) as f:
        for r in csv.DictReader(f):
            try:
                raw[(r['market'], int(float(r['fill_us'])))] = r
            except (KeyError, TypeError, ValueError):
                continue

    out = {}
    k = max(int(len(test) * top_frac), 1)
    for label, idx in (('model', order[:k]), ('random', rnd[:k])):
        half, mo, kept, spread = [], {h: [] for h in HORIZONS}, {h: [] for h in HORIZONS}, []
        for i in idx:
            r = test[i]
            fr = raw.get((r['market'], int(r['fill_us'])))
            if not fr:
                continue
            px, mid = num(fr.get('price')), num(fr.get('mid'))
            if not px or not mid or px <= 0:
                continue
            sgn = 1.0 if fr['side'] == 'buy' else -1.0
            half.append(sgn * (mid - px) / px * 1e4)
            sp = num(fr.get('spread_bps'))
            if sp is not None:
                spread.append(sp)
            for h in HORIZONS:
                v, kv = num(fr.get(f'markout_{h}')), num(fr.get(f'kept_{h}'))
                if v is not None:
                    mo[h].append(v)
                if kv is not None:
                    kept[h].append(kv)
        m = statistics.mean
        # Intervals, not just means. A bare comparison of two sample means calls a 0.14 bps gap
        # a finding, which is how a check meant to catch a false positive becomes one.
        ci = {}
        for h in HORIZONS:
            _, lo, hi = CM.block_bootstrap(mo[h], block=15) if len(mo[h]) >= 20 \
                else (None, None, None)
            ci[h] = (lo, hi)
        out[label] = {
            'n': len(half), 'half_at_post': m(half) if half else None,
            'median_spread': statistics.median(spread) if spread else None,
            'markout': {h: (m(mo[h]) if mo[h] else None) for h in HORIZONS},
            'markout_ci': ci,
            'kept': {h: (m(kept[h]) if kept[h] else None) for h in HORIZONS},
        }
    return out, None


def materially_worse(mdl, rnd, h):
    """Is the model's markout worse than random by more than sampling noise at this horizon?

    The test is separation, not ordering: the model's 90% upper bound has to sit below the
    random selection's mean. Two means a tenth of a basis point apart on a few thousand
    correlated fills are the same number.
    """
    a, b = mdl['markout'][h], rnd['markout'][h]
    if a is None or b is None:
        return False
    hi = mdl['markout_ci'].get(h, (None, None))[1]
    return hi is not None and hi < b


def report(d, out_md, top_frac):
    L = []
    W = L.append
    mdl, rnd = d['model'], d['random']
    W('# Is `kept` the right target? No.\n')
    W(f"Top {top_frac:.0%} of held-out entries by the conditional model, against a random "
      f"selection of the same size ({mdl['n']:,} entries each).\n")

    W('## The decomposition\n')
    W('| term | model | random | model advantage |')
    W('|---|---:|---:|---:|')
    W(f"| median quoted spread | {mdl['median_spread']:.1f} | {rnd['median_spread']:.1f} | "
      f"{mdl['median_spread'] - rnd['median_spread']:+.1f} |")
    W(f"| half-spread credited at POST | **{mdl['half_at_post']:+.2f}** | "
      f"{rnd['half_at_post']:+.2f} | {mdl['half_at_post'] - rnd['half_at_post']:+.2f} |")
    for h in HORIZONS:
        a, b = mdl['markout'][h], rnd['markout'][h]
        if a is None or b is None:
            continue
        lo, hi = mdl['markout_ci'].get(h, (None, None))
        band = '' if lo is None else f' [{lo:+.2f}, {hi:+.2f}]'
        W(f"| markout {h} (what you own){band} | {a:+.2f} | {b:+.2f} | **{a - b:+.2f}** |")
    for h in HORIZONS:
        a, b = mdl['kept'][h], rnd['kept'][h]
        if a is None or b is None:
            continue
        W(f"| kept {h} = half + markout | {a:+.2f} | {b:+.2f} | {a - b:+.2f} |")
    W('')

    adv_half = mdl['half_at_post'] - rnd['half_at_post']
    worse = [h for h in HORIZONS if materially_worse(mdl, rnd, h)]
    ordered_worse = [h for h in HORIZONS if mdl['markout'][h] is not None
                     and mdl['markout'][h] < rnd['markout'][h]]
    W('## What this says\n')
    W(f'The model beats random on `kept`. Every bit of that comes from the first row: it selects '
      f'states whose quoted spread is {mdl["median_spread"] - rnd["median_spread"]:+.1f} bps '
      f'wider, worth {adv_half:+.2f} bps of post-time half-spread credit.\n')
    if len(worse) == len(HORIZONS):
        W(f'On the markout -- what the position is actually worth once you hold it -- the model '
          f'is **worse than random at every horizon**: '
          + ', '.join(f"{h} {mdl['markout'][h]:+.2f} vs {rnd['markout'][h]:+.2f}"
                      for h in HORIZONS) + '.\n')
        W('So the selection is not finding good fills. It is finding wide spreads, and wide '
          'spreads on this venue carry worse adverse selection -- the execution study measured '
          'that correlation at **-0.905** and it is the reason this happens. `kept` adds a term '
          'the model can inflate to a term the model makes worse, and the cancellation leaves a '
          'residue that reads as alpha.\n')
        W('Both halves of that were already measured here, in two reports, and were not put '
          'together until the round trip forced actual fill prices onto both legs.\n')
    elif ordered_worse:
        W(f'The model is worse on the markout at {len(ordered_worse)} of {len(HORIZONS)} '
          f'horizons, but the gap is inside the sampling interval at some of them, so the bias '
          f'is present and not cleanly established. The per-horizon intervals above are what to '
          f'read.\n')
    else:
        W('The model is not worse on the markout, so this particular bias does not explain the '
          'result here. The spread term still should not be credited to a one-sided entry, but '
          'removing it would not by itself reverse the finding.\n')

    W('## The rule this leaves\n')
    W('- For a ONE-SIDED passive entry, the honest value is the **markout**: the mid once you '
      'hold the position, against the price you paid. Nothing else.')
    W('- `half_spread_at_post` is only earned if you fill without the mid moving, and the reason '
      'you fill is that it moved. It is not a credit, it is a hope.')
    W('- A two-sided maker who fills on BOTH sides does capture a spread, and for that strategy '
      'a spread term is legitimate. That is a different strategy from the one that was tested.')
    W('- Any future target that references a price from before the fill has to be checked this '
      'way before it is optimised against, because a model will find the reference before it '
      'finds the edge.\n')
    os.makedirs(os.path.dirname(out_md), exist_ok=True)
    with open(out_md, 'w') as f:
        f.write('\n'.join(L) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fills', default=os.path.join(ROOT, 'reports', 'maker_fills.csv'))
    ap.add_argument('--top', type=float, default=0.10)
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'kept_bias_check.md'))
    args = ap.parse_args()
    if not os.path.exists(args.fills):
        sys.exit(f'no fills at {args.fills}')
    d, why = decompose(args.fills, args.top)
    if why:
        sys.exit(why)
    report(d, args.md, args.top)
    m, r = d['model'], d['random']
    print(f"half-spread at post: model {m['half_at_post']:+.2f}  random {r['half_at_post']:+.2f}")
    for h in HORIZONS:
        if m['markout'][h] is None:
            continue
        flag = ('MODEL WORSE' if materially_worse(m, r, h)
                else 'worse, inside noise' if m['markout'][h] < r['markout'][h]
                else 'model better')
        print(f"  markout {h:>4s}: model {m['markout'][h]:+6.2f}  random {r['markout'][h]:+6.2f}"
              f"   {flag}")
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
