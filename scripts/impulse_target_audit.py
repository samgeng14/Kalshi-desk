#!/usr/bin/env python3
"""Audit a prediction target for pre-entry contamination, before anything is fitted to it.

This exists because the last lane was optimised against a target that quietly contained a price
from before the fill. `kept = half_spread_at_POST + markout_from_FILL` looked like a measure of
what a passive entry is worth. It was a measure of the spread plus a measure of the move, and a
model maximised it by finding wide spreads while holding worse positions. Five separate studies
were run before anything caught it.

A model will find the reference before it finds the edge. So the target gets audited first.

(Placed in `scripts/` rather than `research/` so that the test suite can import it and the
runner picks it up; `research/` in this repository holds written records, not executables.)

TWO AUDITS, because either alone can be fooled.

  STRUCTURAL. A target declares which timestamps each of its components reads. Anything at a
  timestamp before `t_entry` fails immediately. This catches the bug by construction and needs
  no data, but only for targets honest enough to declare themselves.

  EMPIRICAL. Replace the post-entry price path of each event with the path from a DIFFERENT,
  randomly chosen event, and recompute the target. The path is substituted as RETURNS, not as
  absolute prices: two events sit at different price levels on different instruments, so handing
  one event another's absolute mids injects a price-level difference that has nothing to do with
  contamination and flags clean targets. The future is a shape; only the shape is swapped. A clean target becomes noise: everything it
  measured came from a path that is now unrelated. A contaminated target does not, because the
  pre-entry part of it was never touched. The audit compares how well pre-entry features predict
  the real target against how well they predict the shuffled one. A clean target's shuffled
  predictability collapses to zero; `kept`'s does not, because a spread feature still predicts
  the spread term.

The empirical audit needs no declaration and catches targets that lie about themselves. It is
the one that would have caught this.

  python3 scripts/impulse_target_audit.py --self-test
"""
import argparse, math, os, random, statistics, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

R2_RATIO_MAX = 0.25      # shuffled R^2 above a quarter of the real one is contamination
N_PERM = 300             # permutations used to find where the noise floor actually sits


class TargetSpec:
    """A declared target: which price timestamps it reads, relative to t_entry.

    `offsets_us` are the offsets of every price the target uses. `entry_price_used` says whether
    the entry fill price itself is a component, which is legitimate -- it is the price actually
    paid, known at t_entry -- and is the one reference at exactly t_entry that is allowed.
    """

    def __init__(self, name, offsets_us, entry_price_used=True, note=''):
        self.name = name
        self.offsets_us = list(offsets_us)
        self.entry_price_used = entry_price_used
        self.note = note

    def structural_violations(self):
        return [o for o in self.offsets_us if o < 0]


def structural_audit(spec):
    bad = spec.structural_violations()
    return {
        'name': spec.name, 'passed': not bad, 'violations': bad,
        'detail': ('clean: every price it reads is at or after the entry'
                   if not bad else
                   f'reads {len(bad)} price(s) BEFORE the entry, at offsets '
                   + ', '.join((f'{o/1e6:+.3f}s' if abs(o) >= 1000 else f'{o:+d}us')
                                for o in sorted(bad))
                   + ' -- these are known before the trade existed and cannot be part of what '
                     'the trade earned'),
    }


# ---- empirical audit ---------------------------------------------------------------------------
def _ols_r2(xs, y):
    """R^2 of a single-feature least squares fit. Just the squared correlation."""
    n = len(xs)
    if n < 30:
        return None
    mx, my = sum(xs) / n, sum(y) / n
    sxx = sum((a - mx) ** 2 for a in xs)
    syy = sum((b - my) ** 2 for b in y)
    if sxx <= 1e-15 or syy <= 1e-15:
        return 0.0
    sxy = sum((a - mx) * (b - my) for a, b in zip(xs, y))
    return (sxy * sxy) / (sxx * syy)


def noise_floor(rows, feats, target_key, n_perm=N_PERM, seed=99):
    """Where a best-of-k R^2 lands when there is nothing there, for THIS n and THIS k.

    Replaces a hardcoded threshold. A fixed 0.02 cutoff sat right on the noise floor of a
    1,200-row sample and the verdict flipped from one shuffle seed to the next -- an audit whose
    answer depends on its random draw is not an audit. Permuting the features destroys any real
    relationship while preserving the sample size and the number of features tried, which is
    exactly what sets the floor.
    """
    rng = random.Random(seed)
    y = [r[target_key] for r in rows]
    best = []
    for _ in range(n_perm):
        peak = 0.0
        for f in feats:
            xs = [r['x'].get(f) for r in rows]
            pairs = [(a, b) for a, b in zip(xs, y) if a is not None and b is not None]
            if len(pairs) < 30:
                continue
            col = [p[0] for p in pairs]
            rng.shuffle(col)
            r2 = _ols_r2(col, [p[1] for p in pairs])
            if r2 and r2 > peak:
                peak = r2
        best.append(peak)
    best.sort()
    return best[int(0.95 * len(best))] if best else 0.0


def multi_r2(rows, feats, target_key):
    """Best single-feature R^2 across the pre-entry feature set.

    Deliberately univariate. The question is not how much a fitted model can explain, it is
    whether ANY quantity known before the trade explains the target once the future is
    scrambled. One feature doing it is enough to condemn the target, and a univariate statistic
    cannot itself overfit its way to an answer.
    """
    best, who = 0.0, None
    y = [r[target_key] for r in rows]
    for f in feats:
        xs = [r['x'].get(f) for r in rows]
        pairs = [(a, b) for a, b in zip(xs, y) if a is not None and b is not None]
        if len(pairs) < 30:
            continue
        r2 = _ols_r2([p[0] for p in pairs], [p[1] for p in pairs])
        if r2 is not None and r2 > best:
            best, who = r2, f
    return best, who


def empirical_audit(rows, feats, recompute, seed=13):
    """Recompute the target against a shuffled future and see what survives.

    `recompute(row, future)` must rebuild the target from a row and a supplied post-entry path.
    Passing another event's future is the whole test: a target built only from the future has
    nothing left to predict, and one carrying a pre-entry term keeps predicting it.
    """
    rng = random.Random(seed)
    real = []
    for r in rows:
        v = recompute(r, r['future'])
        if v is not None:
            real.append(dict(r, _t=v))
    if len(real) < 60:
        return None, 'too few rows'

    futures = [r['future'] for r in rows]
    shuffled = []
    for r in rows:
        other = futures[rng.randrange(len(futures))]
        v = recompute(r, other)
        if v is not None:
            shuffled.append(dict(r, _t=v))

    r2_real, f_real = multi_r2(real, feats, '_t')
    r2_shuf, f_shuf = multi_r2(shuffled, feats, '_t')
    floor = noise_floor(shuffled, feats, '_t')
    ratio = (r2_shuf / r2_real) if r2_real > 1e-12 else (1.0 if r2_shuf > floor else 0.0)
    contaminated = (r2_shuf > floor) and (ratio > R2_RATIO_MAX)
    return {
        'n_real': len(real), 'n_shuffled': len(shuffled),
        'r2_real': r2_real, 'feature_real': f_real,
        'r2_shuffled': r2_shuf, 'feature_shuffled': f_shuf,
        'ratio': ratio, 'noise_floor': floor, 'contaminated': contaminated,
        'detail': (
            f'a pre-entry feature (`{f_shuf}`) explains {r2_shuf:.2%} of the target even after '
            f'the future is scrambled -- {ratio:.0%} of what it explains on the real target, '
            f'against a permutation noise floor of {floor:.2%}. The target contains information '
            f'that predates the trade.'
            if contaminated else
            f'with the future scrambled, the best pre-entry feature explains {r2_shuf:.2%} of '
            f'the target, against a permutation noise floor of {floor:.2%}. Nothing survives the '
            f'shuffle, so the target is made of the future.'),
    }, None


# ---- the targets this project uses --------------------------------------------------------------
def markout_target(h_us):
    """The clean one: side * (mid(t_entry + h) - entry_price) / entry_price, in bps.

    Every price it touches is the entry fill or later. Nothing from before the trade existed.
    """
    def f(row, future):
        ret = future.get(h_us)
        pe = row['entry_price']
        if ret is None or not pe:
            return None
        m = pe * (1.0 + ret)
        return row['side'] * (m - pe) / pe * 1e4
    return f, TargetSpec(f'markout_{h_us//1000}ms', [0, h_us],
                         note='entry price at t_entry plus one mid after it')


def kept_target(h_us):
    """The contaminated one, kept so the audit can be shown failing something real.

    kept = half_spread_at_POST + markout_from_FILL. The first term reads the mid at t_signal,
    before the trade existed. This is the target the previous lane was optimised against.
    """
    def f(row, future):
        ret = future.get(h_us)
        pe, mid0 = row['entry_price'], row['x'].get('mid_at_signal')
        if ret is None or not pe or not mid0:
            return None
        m = pe * (1.0 + ret)
        half = row['side'] * (mid0 - pe) / pe * 1e4
        return half + row['side'] * (m - pe) / pe * 1e4
    return f, TargetSpec(f'kept_{h_us//1000}ms', [-1, 0, h_us],
                         note='reads the mid at signal time, before the trade existed')


def audit_report(structural, empirical, out_md, name):
    L = []
    W = L.append
    W(f'# Target audit: `{name}`\n')
    W('## Structural\n')
    W(f"**{'PASS' if structural['passed'] else 'FAIL'}** -- {structural['detail']}\n")
    W('## Empirical: scramble the future\n')
    if empirical is None:
        W('Not run.\n')
    else:
        e = empirical
        W(f"**{'FAIL -- CONTAMINATED' if e['contaminated'] else 'PASS'}**\n")
        W('| | best pre-entry feature | R^2 |')
        W('|---|---|---:|')
        W(f"| real target | `{e['feature_real']}` | {e['r2_real']:.3%} |")
        W(f"| future scrambled | `{e['feature_shuffled']}` | {e['r2_shuffled']:.3%} |")
        W(f"| permutation noise floor | (95th pct of best-of-k) | {e['noise_floor']:.3%} |")
        W(f"\nRatio: {e['ratio']:.0%}. Fails above {R2_RATIO_MAX:.0%} when the shuffled R^2 also "
          f"clears the noise floor, which is measured for this sample size rather than assumed.\n")
        W(e['detail'] + '\n')
    W('## Why both\n')
    W('The structural audit catches a target that declares itself honestly. The empirical one '
      'catches a target that does not, which is the case that matters: nobody writes down '
      '"reads a price from before the fill". They write down a formula that happens to.\n')
    if out_md:
        os.makedirs(os.path.dirname(out_md), exist_ok=True)
        with open(out_md, 'w') as f:
            f.write('\n'.join(L) + '\n')
    return '\n'.join(L)


# ---- self-test ----------------------------------------------------------------------------------
def synth(n=1200, seed=7, spread_predicts_future=False):
    """Events whose pre-entry spread is real and, by default, tells you nothing about the future.

    That is the exact shape of the trap: a pre-entry quantity with genuine variation and no
    predictive content. A clean target must be blind to it; `kept` is not.
    """
    rng = random.Random(seed)
    rows = []
    H = [1_000_000, 5_000_000, 10_000_000]
    for i in range(n):
        spread = abs(rng.gauss(10, 5)) + 1.0
        mid0 = 10.0
        side = 1.0 if rng.random() < 0.5 else -1.0
        # A taker entry: buy the offer, sell the bid. Entry is worse than the mid by half.
        pe = mid0 * (1 + side * spread / 2 / 1e4)
        drift = (2.5 * spread if spread_predicts_future else 0.0)
        fut = {}
        for h in H:
            # Stored as a RETURN from the entry price, so that swapping futures between events
            # swaps the shape of the move and nothing else.
            # The drift is NOT multiplied by side here. The target is signed by side, so a
            # side-dependent drift cancels out across a balanced sample and the "predictable"
            # control ends up with no predictability at all -- which is what happened first.
            move = rng.gauss(drift, 20.0)
            fut[h] = side * move / 1e4
        rows.append({'entry_price': pe, 'side': side, 'future': fut,
                     'x': {'spread_at_signal': spread, 'mid_at_signal': mid0,
                           'noise': rng.gauss(0, 1)}})
    return rows, ['spread_at_signal', 'noise']


def self_test():
    rows, feats = synth()
    bad = 0

    def check(c, label):
        nonlocal bad
        if not c:
            bad += 1
            print(f'  FAIL {label}')

    f_clean, s_clean = markout_target(5_000_000)
    f_kept, s_kept = kept_target(5_000_000)

    sc, sk = structural_audit(s_clean), structural_audit(s_kept)
    check(sc['passed'], 'structural: the markout target passes')
    check(not sk['passed'], 'structural: the kept target fails')
    check(sk['violations'] == [-1], 'structural: and it names the offending offset')

    ec, _ = empirical_audit(rows, feats, f_clean)
    ek, _ = empirical_audit(rows, feats, f_kept)
    check(ec and not ec['contaminated'],
          f"empirical: the markout target survives the shuffle "
          f"(r2 {ec and ec['r2_shuffled']:.4f})")
    check(ek and ek['contaminated'],
          f"empirical: the kept target is caught "
          f"(r2 {ek and ek['r2_shuffled']:.4f}, ratio {ek and ek['ratio']:.2f})")
    check(ek and ek['feature_shuffled'] == 'spread_at_signal',
          'empirical: and it names the spread as the contaminating feature')

    # Negative control: only post-entry movement carries information. The audit must stay quiet
    # even though the spread genuinely predicts the future here.
    rows2, feats2 = synth(spread_predicts_future=True, seed=21)
    ec2, _ = empirical_audit(rows2, feats2, f_clean)
    check(ec2 and not ec2['contaminated'],
          f"negative control: a target that is genuinely predictable from a pre-entry feature "
          f"is NOT flagged, because the predictability dies with the shuffle "
          f"(real {ec2 and ec2['r2_real']:.3f} -> shuffled {ec2 and ec2['r2_shuffled']:.4f})")
    check(ec2 and ec2['r2_real'] > ec2['r2_shuffled'] * 3,
          'negative control: and the real target IS predictable, so the test had something to lose')

    print(f'\nstructural: markout {sc["passed"]}, kept {sk["passed"]}')
    print(f'empirical markout : real R2 {ec["r2_real"]:.4f}  shuffled {ec["r2_shuffled"]:.4f}'
          f'  -> {"CONTAMINATED" if ec["contaminated"] else "clean"}')
    print(f'empirical kept    : real R2 {ek["r2_real"]:.4f}  shuffled {ek["r2_shuffled"]:.4f}'
          f'  -> {"CONTAMINATED" if ek["contaminated"] else "clean"}')
    print(f'\n{"ALL CHECKS PASS" if bad == 0 else str(bad) + " CHECKS FAILED"}')
    return bad


def load_real(fills_path, horizon='5s'):
    """Turn recorded maker fills into audit rows, so the audit can be run on the actual bug.

    The future is stored as a return from the entry price, recovered from the recorded markout:
    markout = side * (mid_h - px) / px * 1e4, so the return is side * markout / 1e4.
    """
    import csv as _csv
    h_us = {'1s': 1_000_000, '5s': 5_000_000, '10s': 10_000_000, '60s': 60_000_000}[horizon]
    rows = []
    with open(fills_path) as f:
        for r in _csv.DictReader(f):
            def n(k):
                v = r.get(k)
                try:
                    x = float(v)
                except (TypeError, ValueError):
                    return None
                return None if x != x else x
            px, mid, mo = n('price'), n('mid'), n(f'markout_{horizon}')
            if not px or not mid or mo is None:
                continue
            side = 1.0 if r.get('side') == 'buy' else -1.0
            rows.append({
                'entry_price': px, 'side': side,
                'future': {h_us: side * mo / 1e4},
                'x': {'spread_at_signal': n('spread_bps'), 'mid_at_signal': mid,
                      'queue_ahead': n('queue_ahead'), 'L1_OBI': n('L1_OBI'),
                      'touch_depth': n('touch_depth'), 'vol_60s': n('vol_60s_bps')},
            })
    feats = ['spread_at_signal', 'queue_ahead', 'L1_OBI', 'touch_depth', 'vol_60s']
    return rows, feats, h_us


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--real', default='', help='audit the real kept/markout targets on recorded fills')
    ap.add_argument('--horizon', default='5s')
    ap.add_argument('--md', default=os.path.join(ROOT, 'reports', 'impulse_target_audit.md'))
    args = ap.parse_args()
    if args.self_test:
        sys.exit(1 if self_test() else 0)
    if args.real:
        rows, feats, h_us = load_real(args.real, args.horizon)
        print(f'{len(rows):,} recorded fills, {len(feats)} pre-entry features')
        out = []
        for maker, label in ((markout_target, 'markout'), (kept_target, 'kept')):
            f_, spec = maker(h_us)
            st = structural_audit(spec)
            em, why = empirical_audit(rows, feats, f_)
            if why:
                print(f'  {label}: {why}')
                continue
            verdict = 'CONTAMINATED' if em['contaminated'] else 'clean'
            print(f"  {label:8s} structural {'PASS' if st['passed'] else 'FAIL'}   "
                  f"real R2 {em['r2_real']:.4f} -> shuffled {em['r2_shuffled']:.4f} "
                  f"({em['feature_shuffled']})  ratio {em['ratio']:.2f}  -> {verdict}")
            out.append((st, em, spec.name))
        if out:
            body = '\n\n'.join(audit_report(st, em, None, nm) for st, em, nm in out)
            os.makedirs(os.path.dirname(args.md), exist_ok=True)
            with open(args.md, 'w') as fh:
                fh.write('# Target audit on recorded fills\n\n' + body + '\n')
            print(f'wrote {os.path.relpath(args.md, ROOT)}')
        return
    rows, feats = synth()
    f, spec = markout_target(5_000_000)
    st = structural_audit(spec)
    em, _ = empirical_audit(rows, feats, f)
    audit_report(st, em, args.md, spec.name)
    print(f'{spec.name}: structural {"PASS" if st["passed"] else "FAIL"}, '
          f'empirical {"FAIL" if em and em["contaminated"] else "PASS"}')
    print(f'wrote {os.path.relpath(args.md, ROOT)}')


if __name__ == '__main__':
    main()
