#!/usr/bin/env python3
"""Options-implied digital probabilities, and whether they can price a Kalshi contract.

The thesis for external_digital_surface_v1 is that a Kalshi contract on a tradable asset is a
digital option, and a real options market already prices that digital. That is true. The question
this answers is narrower and decides whether the lane is worth 40% of the effort:

    can we get an implied digital AT THE HORIZON KALSHI ACTUALLY SETTLES ON?

Deribit lists BTC/ETH options expiring daily at 08:00 UTC. Kalshi's crypto markets - 31,023 of the
markets on this desk's watchlist, far more than everything else combined - settle on the HOUR.
So the expiries coincide exactly once a day and are mismatched the other twenty-three times.

Two ways of reading the same chain are computed so that disagreement is visible:

  A. finite difference on quoted USD call prices.  P(S>K) = -dC/dK. Makes no model assumption
     beyond no-arbitrage, but goes through Deribit's inverse (BTC-margined) payoff convention.
  B. finite difference on Black-Scholes prices rebuilt from each strike's own mark_iv. Uses a
     model but stays in USD throughout and picks up the smile strike by strike.

If A and B agree, neither the inverse-payoff conversion nor the model choice is doing real work.
If they disagree, the "independent market hands us a probability" claim is weaker than it sounds.

  python3 digital_surface.py --currency BTC
"""
import argparse, json, math, sys, urllib.request, datetime

API = "https://www.deribit.com/api/v2/public"


def get(path, **params):
    q = "&".join(f"{k}={v}" for k, v in params.items())
    with urllib.request.urlopen(f"{API}/{path}?{q}", timeout=30) as r:
        return json.load(r)["result"]


def norm_cdf(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def bs_call(S, K, T, sigma):
    if T <= 0 or sigma <= 0 or S <= 0 or K <= 0:
        return max(S - K, 0.0)
    d1 = (math.log(S / K) + 0.5 * sigma * sigma * T) / (sigma * math.sqrt(T))
    d2 = d1 - sigma * math.sqrt(T)
    return S * norm_cdf(d1) - K * norm_cdf(d2)


def parse(name):
    # BTC-25SEP26-155000-P
    p = name.split("-")
    return p[0], p[1], float(p[2]), p[3]


def chain(currency):
    rows = get("get_book_summary_by_currency", currency=currency, kind="option")
    by_exp = {}
    for r in rows:
        _, exp, strike, cp = parse(r["instrument_name"])
        if r.get("mark_price") is None or r.get("underlying_price") is None:
            continue
        by_exp.setdefault(exp, []).append({
            "K": strike, "cp": cp,
            "mark": r["mark_price"], "iv": (r.get("mark_iv") or 0) / 100.0,
            "S": r["underlying_price"], "oi": r.get("open_interest") or 0,
            "bid": r.get("bid_price"), "ask": r.get("ask_price"),
        })
    return by_exp


def digitals(legs, T):
    """P(S_T > K) by both routes, on the shared strike grid."""
    S = legs[0]["S"]
    calls, ivs = {}, {}
    for r in legs:
        if r["cp"] == "C":
            calls[r["K"]] = r["mark"] * r["S"]      # BTC premium -> USD
            ivs[r["K"]] = r["iv"]
    ks = sorted(calls)
    out = []
    for i in range(1, len(ks) - 1):
        lo, k, hi = ks[i - 1], ks[i], ks[i + 1]
        # A: model-free, on quoted prices
        a = -(calls[hi] - calls[lo]) / (hi - lo)
        # B: BS rebuilt per strike from its own mark_iv, so the smile is carried
        cb_lo = bs_call(S, lo, T, ivs.get(lo, 0))
        cb_hi = bs_call(S, hi, T, ivs.get(hi, 0))
        b = -(cb_hi - cb_lo) / (hi - lo)
        oi = next((r["oi"] for r in legs if r["K"] == k and r["cp"] == "C"), 0)
        out.append({"K": k, "A": a, "B": b, "oi": oi, "iv": ivs.get(k, 0), "S": S})
    return out


def self_test():
    # a digital is a probability: bounded, and monotone falling in the strike
    S, T, sig = 100.0, 0.25, 0.5
    legs = [{"K": k, "cp": "C", "mark": bs_call(S, k, T, sig) / S, "iv": sig * 100 / 100,
             "S": S, "oi": 1, "bid": 0, "ask": 0} for k in range(60, 151, 5)]
    d = digitals(legs, T)
    assert all(0.0 <= x["A"] <= 1.0 for x in d), "A out of [0,1]"
    assert all(0.0 <= x["B"] <= 1.0 for x in d), "B out of [0,1]"
    assert all(d[i]["A"] >= d[i + 1]["A"] - 1e-9 for i in range(len(d) - 1)), "A not monotone"
    # against the closed form at the money, the finite difference should be close
    atm = min(d, key=lambda x: abs(x["K"] - S))
    d2 = (math.log(S / atm["K"]) - 0.5 * sig * sig * T) / (sig * math.sqrt(T))
    assert abs(atm["A"] - norm_cdf(d2)) < 0.02, f"{atm['A']} vs {norm_cdf(d2)}"
    assert abs(atm["B"] - norm_cdf(d2)) < 0.02
    # a flat smile must make the two routes agree
    assert max(abs(x["A"] - x["B"]) for x in d) < 0.01
    print("digital_surface self-test: PASS  digitals are bounded, monotone, match N(d2) at the")
    print("  money, and the two routes agree when the smile is flat.")


def exp_dt(e):
    return datetime.datetime.strptime(e, "%d%b%y").replace(hour=8)


def show(currency):
    now = datetime.datetime.utcnow()
    by_exp = chain(currency)
    exps = sorted(by_exp, key=exp_dt)[:3]
    print(f"{currency}  spot {by_exp[exps[0]][0]['S']:,.0f}   now {now:%Y-%m-%d %H:%M} UTC\n")
    for e in exps:
        T = max((exp_dt(e) - now).total_seconds(), 1) / (365.25 * 86400)
        hrs = (exp_dt(e) - now).total_seconds() / 3600
        d = digitals(by_exp[e], T)
        liq = [x for x in d if x["oi"] > 0]
        print(f"expiry {e}  in {hrs:6.2f}h   {len(d)} strikes, {len(liq)} with open interest")
        print(f"   {'strike':>8} {'A model-free':>13} {'B from smile':>13} {'A-B':>8} {'iv':>6} {'OI':>8}")
        S = d[0]["S"]
        for x in sorted(sorted(d, key=lambda z: abs(z["K"] - S))[:5], key=lambda z: z["K"]):
            print(f"   {x['K']:8.0f} {x['A']:13.4f} {x['B']:13.4f} {x['A']-x['B']:+8.4f} "
                  f"{x['iv']*100:5.1f}% {x['oi']:8.1f}")
        print()


def interp_test(currency="BTC", idx=(0, 1, 2)):
    """Hold a real expiry out and try to rebuild it from the two around it.

    The raw difference between two listed expiries is not what anyone would trade on. The question
    is whether interpolating TOTAL VARIANCE between bracketing expiries recovers a horizon we did
    not use - because that is exactly what pricing a Kalshi hour off a daily options chain
    requires. Deribit hands us a free out-of-sample test: the middle expiry is observable, so the
    rebuild can be scored against it rather than argued about.

    Measured 2026-09-12: with the 1.9h expiry in the bracket the rebuild misses by 4.9 cents on BTC
    and 3.2 on ETH; with a 25.9h-73.9h bracket it misses by 1.7 and 1.2. The error is a short-end
    problem, and the short end is where Kalshi's hourly crypto markets live.
    """
    now = datetime.datetime.utcnow()
    by_exp = chain(currency)
    exps = sorted(by_exp, key=exp_dt)
    sel = [exps[i] for i in idx]
    Ts, Ds, ivs = [], [], []
    for e in sel:
        T = max((exp_dt(e) - now).total_seconds(), 1) / (365.25 * 86400)
        Ts.append(T)
        d = digitals(by_exp[e], T)
        Ds.append({x["K"]: x for x in d})
        ivs.append({x["K"]: x["iv"] for x in d})
    T0, T1, T2 = Ts
    S = list(Ds[0].values())[0]["S"]
    shared = sorted(set(Ds[0]) & set(Ds[1]) & set(Ds[2]))
    h = [t * 365.25 * 24 for t in Ts]

    def s_at(k):
        a, b = ivs[0].get(k, 0), ivs[2].get(k, 0)
        if a <= 0 or b <= 0:
            return 0.0
        wa, wb = a * a * T0, b * b * T2
        return math.sqrt(max(wa + (wb - wa) * (T1 - T0) / (T2 - T0), 1e-12) / T1)

    print(f"\n{currency}: bracket {h[0]:.1f}h -> {h[2]:.1f}h, holding out the real {h[1]:.1f}h chain")
    print(f"   {'strike':>8} {'actual':>8} {'interp':>8} {'error':>8}")
    errs = []
    for K in sorted(shared, key=lambda k: abs(k - S))[:9]:
        lo = max([k for k in shared if k < K], default=None)
        hi = min([k for k in shared if k > K], default=None)
        if lo is None or hi is None or s_at(lo) <= 0 or s_at(hi) <= 0:
            continue
        est = -(bs_call(S, hi, T1, s_at(hi)) - bs_call(S, lo, T1, s_at(lo))) / (hi - lo)
        act = Ds[1][K]["A"]
        errs.append(abs(est - act))
        print(f"   {K:8.0f} {act:8.4f} {est:8.4f} {est-act:+8.4f}")
    if errs:
        print(f"\n   mean |error| {100*sum(errs)/len(errs):5.2f} cents    "
              f"max |error| {100*max(errs):5.2f} cents    (n={len(errs)})")
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--currency", default="BTC")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--interp-test", action="store_true",
                    help="hold a real expiry out and score the variance interpolation against it")
    ap.add_argument("--bracket", default="0,1,2", help="which three expiries, by index")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.interp_test:
        idx = tuple(int(x) for x in a.bracket.split(","))
        return interp_test(a.currency, idx)
    show(a.currency)


if __name__ == "__main__":
    main()
