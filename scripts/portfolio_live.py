#!/usr/bin/env python3
"""PORTFOLIO_V1 weekly decision. Writes the whole decision to disk before anything is sent.

ORDER OF OPERATIONS, AND IT IS NOT NEGOTIABLE:
  verify the config hash -> pull bars -> check staleness -> compute the frozen signal ->
  compute target weights -> read live account and markets -> solve the integer book ->
  score it against the pre-set feasibility thresholds -> apply the no-trade test ->
  WRITE THE DECISION FILE -> only then hand an order plan to the executor.

Nothing downstream may run if the config hash does not match what was frozen. That check is the
reason the hash exists: it is what makes "the spec stayed frozen" a fact rather than a promise.

SHADOW AND LIVE ARE COMPUTED SEPARATELY AND ALWAYS BOTH. Shadow is the theoretical book at
theoretical prices with modelled costs. Live is whatever integer contracts the account can
actually hold. The difference between them is the number this deployment exists to measure, and
collapsing them into one series would destroy it.
"""
import argparse, datetime as dt, hashlib, itertools, json, os, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import signal_data                                                   # noqa: E402

KALSHI_TICKER = {
    'AAVEUSDT': 'KXAAVEPERP', 'ADAUSDT': 'KXADAPERP', 'AVAXUSDT': None,
    'BCHUSDT': 'KXBCHPERP', 'BNBUSDT': 'KXBNBPERP', 'BTCUSDT': 'KXBTCPERP',
    'DOGEUSDT': 'KXDOGEPERP', 'ETHUSDT': 'KXETHPERP', 'LINKUSDT': 'KXLINKPERP',
    'LTCUSDT': 'KXLTCPERP', 'NEARUSDT': 'KXNEARPERP', 'SOLUSDT': 'KXSOLPERP',
    'XRPUSDT': 'KXXRPPERP',
}


# ---------------------------------------------------------------- frozen spec

def load_frozen(cfg_path=None, hash_path=None):
    cfg_path = cfg_path or os.path.join(ROOT, 'config', 'portfolio_v1.json')
    hash_path = hash_path or os.path.join(ROOT, 'config', 'portfolio_v1.sha256')
    cfg = json.load(open(cfg_path))
    canon = json.dumps(cfg, sort_keys=True, separators=(',', ':')).encode()
    got = hashlib.sha256(canon).hexdigest()
    want = open(hash_path).read().strip()
    if got != want:
        raise SystemExit('CONFIG HASH MISMATCH\n  frozen %s\n  now    %s\n'
                         'The spec changed. Nothing runs until this is explained.' % (want, got))
    return cfg, got


# ---------------------------------------------------------------- the signal

def zscore(v):
    v = np.asarray(v, dtype=float)
    ok = np.isfinite(v)
    if ok.sum() < 3:
        return np.zeros_like(v)
    m, s = v[ok].mean(), v[ok].std()
    out = np.zeros_like(v)
    if s > 1e-12:
        out[ok] = (v[ok] - m) / s
    return out


def rolling_pca_at(rets, t, window, n_pc):
    """PCA on the trailing window ending at t. Only data strictly before t is used."""
    W = rets[max(0, t - window):t]
    sd = W.std(axis=0)
    sd[sd < 1e-12] = 1.0
    mu = W.mean(axis=0)
    Z = (W - mu) / sd
    ev, evec = np.linalg.eigh(np.cov(Z, rowvar=False))
    evec = evec[:, ::-1][:, :n_pc]
    for j in range(n_pc):
        if evec[:, j].sum() < 0:
            evec[:, j] = -evec[:, j]
    return evec, sd, mu


def residuals_upto(rets, t, window, n_pc, need):
    """Residual returns for the `need` bars before t, each using the loadings current at that bar."""
    out = np.full((need, rets.shape[1]), np.nan)
    evec = sd = mu = None
    for j, tt in enumerate(range(t - need, t)):
        if tt < window:
            continue
        if evec is None or (tt - window) % 24 == 0:
            evec, sd, mu = rolling_pca_at(rets, tt, window, n_pc)
        z = (rets[tt] - mu) / sd
        out[j] = (z - evec @ (evec.T @ z)) * sd
    return out


def target_book(cfg, rets, t):
    """The frozen composite and the neutralised target weights at bar t."""
    N = rets.shape[1]
    w_pca = cfg['pca']['window_hours']
    n_pc = cfg['pca']['n_components']
    xs_h = cfg['components']['cross_sectional_momentum_hours']
    rs_h = cfg['components']['residual_momentum_hours']
    need = max(max(xs_h), max(rs_h))
    resid = residuals_upto(rets, t, w_pca, n_pc, need)

    comps, detail = [], {}
    for h in xs_h:
        v = rets[t - h:t].sum(axis=0)
        comps.append(zscore(v))
        detail['xs_%dh' % h] = v.tolist()
    for h in rs_h:
        v = np.nansum(resid[need - h:], axis=0)
        comps.append(zscore(v))
        detail['resid_%dh' % h] = v.tolist()
    score = np.mean(comps, axis=0)

    evec, _, _ = rolling_pca_at(rets, t, w_pca, n_pc)
    beta1 = evec[:, 0]

    k = cfg['portfolio']['k_long']
    order = np.argsort(score)
    w = np.zeros(N)
    w[order[-k:]] = 0.5 / k
    w[order[:k]] = -0.5 / k
    gross = np.abs(w).sum()
    if beta1 @ beta1 > 1e-12:
        w = w - (w @ beta1) / (beta1 @ beta1) * beta1
        g = np.abs(w).sum()
        if g > 1e-12:
            w = w * (gross / g)
    return score, w, beta1, detail


# ---------------------------------------------------------------- feasibility

def candidate_counts(price, target_notional, equity, cap_frac):
    if price is None or price <= 0:
        return [0]
    cap = int(equity * cap_frac // price)
    ideal = target_notional / price
    c = {0, int(np.floor(ideal)) - 1, int(np.floor(ideal)), int(np.ceil(ideal)),
         int(np.ceil(ideal)) + 1}
    return sorted({x for x in c if 0 <= x <= max(cap, 0)})


def solve_integer_book(names, w, beta1, markets, equity, lim):
    """Best integer contract book under the gross, margin and single-name caps.

    Exhaustive over a bounded per-leg candidate set. For an L2 objective the optimum for each leg
    is within one contract of the ideal fraction, so the candidate set is {0, floor+-1, ceil+1}
    and six legs enumerate in milliseconds. A rounding heuristic would be faster and would
    occasionally pick a book that the gross cap makes worse than an alternative.
    """
    legs = [i for i in range(len(names)) if abs(w[i]) > 0.01]
    meta, grids, unlisted, missing_lev = [], [], [], []
    for i in legs:
        tk = KALSHI_TICKER.get(names[i])
        if not tk or tk not in markets:
            unlisted.append(i)
            continue
        m = markets[tk]
        px = m['ask'] if w[i] > 0 else m['bid']
        tgt = abs(w[i]) * equity * lim['gross_over_equity_max']
        lev = float(m.get('leverage') or 0)
        if lev <= 0:
            # Defaulting a missing leverage to 1.0 makes margin equal full notional, the 35% cap
            # then binds on every leg, and the book silently shrinks from six names to three and
            # fails feasibility. That looks like the signal changing its mind. It is a missing
            # field. Refuse to size the leg rather than size it wrongly.
            missing_lev.append(names[i])
            continue
        meta.append((i, tk, float(px), lev, float(np.sign(w[i]))))
        grids.append(candidate_counts(px, tgt, equity,
                                      lim['single_name_notional_over_equity_max']))
    best = None
    for counts in itertools.product(*grids) if grids else []:
        notional = np.array([c * mm[2] * mm[4] for c, mm in zip(counts, meta)])
        gross = float(np.abs(notional).sum())
        if gross > equity * lim['gross_over_equity_max'] + 1e-9:
            continue
        margin = float(sum(abs(n) / mm[3] for n, mm in zip(notional, meta) if mm[3] > 0))
        if margin > equity * lim['margin_utilization_max'] + 1e-9:
            continue
        wl = np.zeros(len(names))
        for (i, _, _, _, _), n in zip(meta, notional):
            wl[i] = n / equity
        e = float(np.linalg.norm(wl - w))
        if best is None or e < best[0]:
            best = (e, counts, wl, gross, margin)
    return best, meta, unlisted, missing_lev


def score_feasibility(w, wl, beta1, counts, thr):
    gross_l = float(np.abs(wl).sum()) or 1.0
    rel_te = float(np.linalg.norm(wl - w)) / (float(np.linalg.norm(w)) or 1.0)
    net = abs(float(wl.sum())) / gross_l
    pc1 = abs(float(wl @ beta1)) / ((gross_l * float(np.abs(beta1).mean())) or 1.0)
    legs = int(sum(1 for c in counts if c != 0))
    checks = {
        'relative_weight_tracking_error': (rel_te, thr['relative_weight_tracking_error_max'], 'le'),
        'net_exposure_over_gross': (net, thr['net_exposure_over_gross_max'], 'le'),
        'pc1_exposure_over_gross': (pc1, thr['pc1_exposure_over_gross_max'], 'le'),
        'representable_legs': (legs, thr['min_representable_legs'], 'ge'),
    }
    passed = all((v <= t + 1e-12) if how == 'le' else (v >= t)
                 for v, t, how in checks.values())
    return passed, {k: {'value': v, 'threshold': t, 'pass':
                        bool((v <= t + 1e-12) if how == 'le' else (v >= t))}
                    for k, (v, t, how) in checks.items()}


# ---------------------------------------------------------------- no-trade test

def no_trade_filter(names, w_target, w_current, score, markets, equity, cfg, dispersion_bps):
    """Trade a leg only when the expected benefit clears its cost times the required ratio.

    A changed target weight is not a reason to trade. The benefit of moving a leg is estimated as
    the weight change times that name's score advantage times the return scale of the horizon;
    the cost is the measured one-way cost of the move. Kalshi's fee is 12 bps a side, so a leg
    that improves the book by 3 bps is a leg worth leaving alone.
    """
    nt = cfg['no_trade_test']
    ow = cfg['cost_assumptions']['oneway_bps']
    dflt = cfg['cost_assumptions']['default_oneway_bps']
    need = float(nt['require_benefit_over_cost_ratio'])
    keep, rows = [], []
    s_rng = float(np.nanmax(score) - np.nanmin(score)) or 1.0
    for i, nm in enumerate(names):
        dw = float(w_target[i] - w_current[i])
        if abs(dw) < 1e-6:
            continue
        cost_bps = abs(dw) * float(ow.get(nm, dflt))
        benefit_bps = abs(dw) * (abs(float(score[i])) / s_rng) * float(dispersion_bps)
        ok = benefit_bps >= need * cost_bps
        rows.append({'symbol': nm, 'kalshi': KALSHI_TICKER.get(nm),
                     'w_current': w_current[i], 'w_target': w_target[i], 'delta_w': dw,
                     'score': float(score[i]),
                     'expected_benefit_bps': benefit_bps, 'expected_cost_bps': cost_bps,
                     'benefit_over_cost': (benefit_bps / cost_bps) if cost_bps > 0 else None,
                     'required_ratio': need, 'trade': bool(ok)})
        if ok:
            keep.append(i)
    return keep, rows


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', default=os.path.join(ROOT, 'data', 'snapshots',
                                                       'kalshi_perp_snapshot.json'),
                    help='market/account snapshot; the live path refreshes this first')
    # 30 hours, not 6. Binance publishes DAILY kline files, so the freshest balanced panel
    # across 13 assets ends at yesterday's close -- about 23 hours old at any given moment. On a
    # weekly rebalance whose shortest component looks back 168 hours, that is immaterial. This is
    # an operational parameter about data plumbing, not part of the frozen model spec, and it is
    # deliberately kept out of config/portfolio_v1.json so that changing it cannot change the
    # config hash.
    ap.add_argument('--max-stale-hours', type=float, default=30.0)
    ap.add_argument('--out', default=os.path.join(ROOT, 'reports', 'decisions'))
    ap.add_argument('--label', default='')
    args = ap.parse_args()

    cfg, cfg_hash = load_frozen()
    lim = cfg['canary_risk_limits']
    thr = cfg['feasibility_thresholds']
    now = dt.datetime.now(dt.timezone.utc)

    rets, ts, names, info = signal_data.build(cfg['signal_universe']['symbols'],
                                              max_age_h=args.max_stale_hours)
    t = rets.shape[0]
    score, w, beta1, detail = target_book(cfg, rets, t)

    snap = json.load(open(args.snapshot))
    markets = {m['ticker']: m for m in snap['markets']}
    acct = snap['account']
    equity = float(acct['equity'])
    held = {p['ticker']: float(p['position']) for p in snap.get('positions', [])}

    w_current = np.zeros(len(names))
    for i, nm in enumerate(names):
        tk = KALSHI_TICKER.get(nm)
        if tk and tk in held and tk in markets:
            w_current[i] = held[tk] * float(markets[tk]['mark']) / equity

    best, meta, unlisted, missing_lev = solve_integer_book(names, w, beta1, markets, equity, lim)
    if missing_lev:
        print('\nREFUSING TO SIZE: no leverage figure for %s' % ', '.join(missing_lev))
        print('Re-run scripts/kalshi_snapshot.py; a null leverage silently shrinks the book.')
    if best is None:
        feasible, checks, wl, counts, gross, margin = False, {}, np.zeros(len(names)), (), 0.0, 0.0
    else:
        _, counts, wl, gross, margin = best
        feasible, checks = score_feasibility(w, wl, beta1, counts, thr)

    disp = float(np.mean(np.abs(rets[-168:].sum(axis=0))))
    keep, nt_rows = no_trade_filter(names, wl, w_current, score, markets, equity, cfg, disp)

    mode = 'PORTFOLIO_V1' if feasible else 'SHADOW_V1 + EXECUTION_CANARY'
    decision = {
        'decided_at_utc': now.strftime('%Y-%m-%dT%H:%M:%SZ'),
        'config_hash': cfg_hash,
        'spec_name': cfg['spec_name'],
        'mode': mode,
        'pnl_label': 'PORTFOLIO_V1' if feasible else 'EXECUTION_CANARY',
        'data': {k: (v if k != 'names' else v) for k, v in info.items() if k != 'source_bars'},
        'account': acct,
        'universe': names,
        'composite_score': {n: float(s) for n, s in zip(names, score)},
        'beta1': {n: float(b) for n, b in zip(names, beta1)},
        'target_weights': {n: float(x) for n, x in zip(names, w)},
        'current_weights': {n: float(x) for n, x in zip(names, w_current)},
        'live_weights': {n: float(x) for n, x in zip(names, wl)},
        'integer_book': ([{'symbol': names[m[0]], 'kalshi': m[1],
                           'contracts': int(c * m[4]), 'price': m[2],
                           'notional': c * m[2] * m[4], 'leverage': m[3]}
                          for m, c in zip(meta, counts)] if best else []),
        'unlisted_legs': ([{'symbol': names[i], 'target_weight': float(w[i]),
                            'reason': 'no Kalshi perp contract exists'} for i in unlisted] +
                          [{'symbol': nm, 'target_weight': None,
                            'reason': 'snapshot carried no leverage figure for this market'}
                           for nm in missing_lev]),
        'feasibility': checks,
        'feasible': bool(feasible),
        'gross_over_equity': (gross / equity) if equity else None,
        'margin_utilisation': (margin / equity) if equity else None,
        'no_trade_test': nt_rows,
        'legs_to_trade': [names[i] for i in keep],
        'horizon_dispersion_bps': disp,
    }

    os.makedirs(args.out, exist_ok=True)
    stamp = now.strftime('%Y%m%dT%H%M%SZ') + (('-' + args.label) if args.label else '')
    path = os.path.join(args.out, 'decision_%s.json' % stamp)
    with open(path, 'w') as f:
        json.dump(decision, f, indent=1)
    latest = os.path.join(args.out, 'latest.json')
    with open(latest, 'w') as f:
        json.dump(decision, f, indent=1)

    print('PORTFOLIO_V1 DECISION   %s' % decision['decided_at_utc'])
    print('config hash   %s  (verified against the freeze)' % cfg_hash[:16])
    print('bars          %d, last %s UTC, %.1f hours old'
          % (info['bars'], dt.datetime.utcfromtimestamp(info['last_bar_ms'] / 1000)
             .strftime('%Y-%m-%d %H:%M'), info['stale_hours']))
    print('equity        $%.4f' % equity)
    print('mode          %s' % mode)
    print('\n%-10s %-13s %8s %9s %9s %9s %10s'
          % ('symbol', 'kalshi', 'score', 'target', 'live', 'current', 'contracts'))
    ctr = {names[m[0]]: int(c * m[4]) for m, c in zip(meta, counts)} if best else {}
    for i, nm in enumerate(names):
        if abs(w[i]) < 0.01 and abs(wl[i]) < 1e-9 and abs(w_current[i]) < 1e-9:
            continue
        print('%-10s %-13s %+8.3f %+9.4f %+9.4f %+9.4f %10s'
              % (nm, KALSHI_TICKER.get(nm) or 'NOT LISTED', score[i], w[i], wl[i],
                 w_current[i], ctr.get(nm, '-')))
    print('\n%-34s %10s %10s %8s' % ('feasibility check', 'value', 'threshold', 'verdict'))
    for k, v in checks.items():
        print('%-34s %10.4f %10.4f %8s'
              % (k, v['value'], v['threshold'], 'PASS' if v['pass'] else 'FAIL'))
    print('\nwrote %s' % path)
    print('     %s' % latest)
    if not feasible:
        print('\nLive PnL from this decision is labelled EXECUTION_CANARY, not PORTFOLIO_V1.')
    return 0 if feasible else 2


if __name__ == '__main__':
    sys.exit(main())
