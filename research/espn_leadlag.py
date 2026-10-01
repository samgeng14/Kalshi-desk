#!/usr/bin/env python3
"""ESPN in-game win probability against Kalshi: who moves first, and does the discrepancy pay?

The volume plan (2026-09-23, lane B) buys the side ESPN's number rates 8c or more above its
Kalshi price, cheap side only, held to settlement. It is only worth doing if ESPN moves FIRST:
if Kalshi has already repriced by the time ESPN publishes, a gap between them is Kalshi being
early and ESPN being stale, and trading it is trading against the better number.

Two questions, answered from what the desk recorded (ext_wp every 30 s during live games, the
Kalshi book in `quotes`, settlements in `markets`):

  1. Lead/lag. For every ESPN win-probability jump of 5c or more between consecutive samples,
     how much of the matching Kalshi move had already happened when ESPN's new number was first
     seen, and how much by +30, +60, +120 and +300 s.
  2. The trade itself. At every ESPN sample, each side: ESPN's probability against the Kalshi ask
     after the taker fee. The first trigger per game-side, held to settlement, sized at $5.

WHAT THE DATA CAN AND CANNOT RESOLVE. ESPN is sampled every 30 s, so a play is placed only within
the 30 s before the sample that shows it. The Kalshi book was refreshed once per desk cycle
(roughly every 15-25 s in this period), so the Kalshi mid "at" a moment can be up to a cycle old.
Both blur timing; the second one makes Kalshi look LATER than it was, so it biases question 1
toward "ESPN leads". The plan's 0/5/15 s horizons need the Kalshi WebSocket book on these
markets, which the desk does not yet record for sports. What this CAN say: whether Kalshi is
already most of the way there at ESPN's first sample (then ESPN is not leading), and whether the
trigger made or lost money at settlement.

  python3 research/espn_leadlag.py --extract     # desk stopped: copy the rows it needs
  python3 research/espn_leadlag.py               # analyse the extract, write the report

The extract refuses to run while the desk holds its database lock; the analysis never opens the
live database at all.
"""
import argparse, bisect, fcntl, math, os, sqlite3, statistics, sys, time
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIVE = os.path.join(ROOT, 'data', 'prod.sqlite')
EXTRACT = os.path.join(ROOT, 'data', 'research', 'espn_leadlag.sqlite')
REPORT = os.path.join(ROOT, 'reports', 'espn_leadlag.md')


# ---- extract ------------------------------------------------------------------------------

def take_desk_lock(db):
    """Take the desk's flock on <db>.lock and keep it for the caller's whole read: while it is held a
    desk (or a supervisor's restart) cannot start on this database. None when a desk holds it now."""
    fd = os.open(db + '.lock', os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(fd)
        return None
    return fd


def extract(live, out):
    lock = take_desk_lock(live)   # held until this function returns
    if lock is None:
        sys.exit('the desk is running on this database; stop it first (scripts/tradeit down)')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    src = sqlite3.connect(f'file:{live}?mode=ro', uri=True)
    dst = sqlite3.connect(out)
    wp_cols = ['ts', 'event_ticker', 'league', 'state', 'period', 'clock', 'home_score', 'away_score', 'wp_home',
               'mkt_home', 'mkt_away', 'home_ticker', 'away_ticker']
    dst.execute(f"CREATE TABLE ext_wp({', '.join(wp_cols)})")
    rows = src.execute(f"SELECT {', '.join(wp_cols)} FROM ext_wp ORDER BY event_ticker, ts").fetchall()
    dst.executemany(f"INSERT INTO ext_wp VALUES({','.join('?' * len(wp_cols))})", rows)
    tickers = sorted({t for r in rows for t in (r[11], r[12]) if t})
    dst.execute('CREATE TABLE quotes(ts, ticker, yes_bid, yes_ask)')
    n_q = 0
    for t in tickers:
        q = src.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes WHERE ticker=? ORDER BY ts', (t,)).fetchall()
        dst.executemany('INSERT INTO quotes VALUES(?,?,?,?)', q)
        n_q += len(q)
    dst.execute('CREATE TABLE markets(ticker, series_ticker, result, status, close_time)')
    for t in tickers:
        m = src.execute('SELECT ticker, series_ticker, result, status, close_time FROM markets WHERE ticker=?', (t,)).fetchone()
        if m:
            dst.execute('INSERT INTO markets VALUES(?,?,?,?,?)', m)
    dst.execute('CREATE TABLE series(ticker, fee_type, fee_multiplier)')
    dst.executemany('INSERT INTO series VALUES(?,?,?)',
                    src.execute('SELECT ticker, fee_type, fee_multiplier FROM series').fetchall())
    dst.execute('CREATE INDEX iq ON quotes(ticker, ts)')
    dst.commit()
    src.close()
    os.close(lock)
    print(f'extracted {len(rows):,} ESPN samples, {len(tickers)} tickers, {n_q:,} Kalshi quotes -> {out}')


# ---- analysis -----------------------------------------------------------------------------

class Book:
    """Kalshi YES top of book per ticker, as recorded; lookups return the last row at or before t."""

    def __init__(self, db):
        self.ts, self.bid, self.ask = defaultdict(list), defaultdict(list), defaultdict(list)
        for ts, t, b, a in db.execute('SELECT ts, ticker, yes_bid, yes_ask FROM quotes ORDER BY ticker, ts'):
            self.ts[t].append(ts); self.bid[t].append(b or 0.0); self.ask[t].append(a or 0.0)

    def at(self, ticker, t, max_age):
        xs = self.ts.get(ticker)
        if not xs:
            return None
        i = bisect.bisect_right(xs, t) - 1
        if i < 0 or t - xs[i] > max_age:
            return None
        b, a = self.bid[ticker][i], self.ask[ticker][i]
        if not (0 < b < a < 1):
            return None
        return b, a

    def mid(self, ticker, t, max_age=90):
        q = self.at(ticker, t, max_age)
        return None if q is None else 0.5 * (q[0] + q[1])


def taker_fee(price, count, mult):
    """Kalshi's taker fee for one order, rounded up to the cent."""
    return math.ceil(100 * mult * 0.07 * count * price * (1 - price) - 1e-9) / 100


def med(xs):
    return statistics.median(xs) if xs else float('nan')


def fmt(x, d=2):
    return '-' if x is None or (isinstance(x, float) and math.isnan(x)) else f'{x:.{d}f}'


def lead_lag(samples, book):
    """ESPN jumps and the Kalshi home mid around them, sign-aligned to the jump."""
    horizons = [0, 30, 60, 120, 300]
    out = []
    for ev, rows in samples.items():
        rows = [r for r in rows if r['wp'] is not None and r['wp'] >= 0 and r['state'] == 'in']
        for a, b in zip(rows, rows[1:]):
            dt = b['ts'] - a['ts']
            if dt <= 0 or dt > 45:
                continue
            dwp = b['wp'] - a['wp']
            if abs(dwp) < 0.05:
                continue
            t = b['home_ticker']
            pre = book.mid(t, a['ts'])
            if pre is None:
                continue
            s = 1 if dwp > 0 else -1
            rec = {'league': b['league'], 'dwp': abs(dwp), 'moves': {}}
            before = book.mid(t, a['ts'] - 60)
            rec['pretrend'] = None if before is None else s * (pre - before)
            for h in horizons:
                m = book.mid(t, b['ts'] + h)
                rec['moves'][h] = None if m is None else s * (m - pre)
            out.append(rec)
    return out, horizons


def backtest(samples, book, results, fee_mult, threshold, cap, stake=5.0, router=False):
    """First trigger per game-side: ESPN's p minus the ask and the taker fee >= threshold.

    router=True approximates what the desk's router would actually let the lane send (review
    2026-09-23): the model is blended 50/50 with the side's mid (sports_live market_weight 0.5);
    a raw gap over 0.15 or a net edge over 0.12 goes to the manual board; the edge less half an
    assumed 0.04 sigma must clear min_edge 0.03; and the side costs <= 0.45 unless that buffered edge
    exceeds 0.10. Still a taker at the ask: maker fills cannot be simulated from this data."""
    trades = []
    for ev, rows in samples.items():
        taken = set()
        for r in rows:
            if r['state'] != 'in' or r['wp'] is None or r['wp'] < 0:
                continue
            for side, ticker, p in (('home', r['home_ticker'], r['wp']), ('away', r['away_ticker'], 1 - r['wp'])):
                if side in taken or not ticker or ticker not in results:
                    continue
                q = book.at(ticker, r['ts'], 30)
                if q is None:
                    continue
                bid, ask = q
                if cap is not None and not router and ask > cap + 1e-9:
                    continue
                mult = fee_mult.get(ticker, 1.0)
                count = math.floor(stake / (ask + mult * 0.07 * ask * (1 - ask)))
                if count < 1:
                    continue
                fee = taker_fee(ask, count, mult)
                if router:
                    mid = 0.5 * (bid + ask)
                    if abs(p - mid) > 0.15:
                        continue
                    edge = (0.5 * p + 0.5 * mid) - ask - fee / count
                    if edge > 0.12:
                        continue
                    buffered = edge - 0.5 * 0.04
                    if buffered < 0.03 or (ask > 0.45 and buffered <= 0.10):
                        continue
                else:
                    edge = p - ask - fee / count
                    if edge < threshold:
                        continue
                won = results[ticker] == 'yes'
                pnl = (count if won else 0) - count * ask - fee
                trades.append({'event': ev, 'league': r['league'], 'side': side, 'p': p, 'ask': ask, 'edge': edge,
                               'count': count, 'cost': count * ask + fee, 'won': won, 'pnl': pnl})
                taken.add(side)
    return trades


def summarize(trades):
    n = len(trades)
    if n == 0:
        return {'n': 0}
    pnl = [t['pnl'] for t in trades]
    mean = statistics.fmean(pnl)
    sd = statistics.stdev(pnl) if n > 1 else 0.0
    se = sd / math.sqrt(n) if n > 1 else float('nan')
    return {'n': n, 'wins': sum(t['won'] for t in trades), 'p_mean': statistics.fmean(t['p'] for t in trades),
            'ask_mean': statistics.fmean(t['ask'] for t in trades), 'total': sum(pnl), 'mean': mean, 'sd': sd,
            't': mean / se if se and se > 0 else float('nan'), 'lower80': mean - 1.2816 * se if n > 1 else float('nan'),
            'fee_mean': statistics.fmean(t['cost'] - t['count'] * t['ask'] for t in trades)}


def swapped_games(samples, book):
    """Games where the desk's ESPN-to-Kalshi mapping had home and away the wrong way round.

    ESPN's home win probability and the Kalshi mid of the ticker the desk called home track the
    same thing, so within a game they move together. A strongly NEGATIVE correlation over a game
    means the ticker was the other team's. (sports_live's matcher could do this before
    2026-09-23: see the comment at market_team in src/strategies/sports_live.cpp.)"""
    out = {}
    for ev, rows in samples.items():
        pairs = []
        for r in rows:
            if r['state'] != 'in' or r['wp'] is None or r['wp'] < 0:
                continue
            m = book.mid(r['home_ticker'], r['ts'], 60)
            if m is not None:
                pairs.append((r['wp'], m))
        if len(pairs) < 10:
            continue
        w, m = [a for a, _ in pairs], [b for _, b in pairs]
        if statistics.pstdev(w) < 1e-6 or statistics.pstdev(m) < 1e-6:
            continue
        r = statistics.correlation(w, m)
        if r < -0.3:
            out[ev] = r
    return out


def corrected(samples, swapped):
    """The samples with the swapped games' home and away tickers exchanged."""
    out = {}
    for ev, rows in samples.items():
        if ev not in swapped:
            out[ev] = rows
            continue
        out[ev] = [dict(r, home_ticker=r['away_ticker'], away_ticker=r['home_ticker']) for r in rows]
    return out


def analyse(path, report):
    db = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    samples = defaultdict(list)
    for ts, ev, lg, st, wp, ht, at in db.execute(
            'SELECT ts, event_ticker, league, state, wp_home, home_ticker, away_ticker FROM ext_wp ORDER BY event_ticker, ts'):
        samples[ev].append({'ts': ts, 'league': lg, 'state': st, 'wp': wp, 'home_ticker': ht, 'away_ticker': at})
    book = Book(db)
    swapped = swapped_games(samples, book)
    as_recorded = samples
    samples = corrected(samples, swapped)
    results = {t: r for t, r in db.execute("SELECT ticker, result FROM markets WHERE result IN ('yes','no')")}
    fee_by_series = {s: (m if m is not None else 1.0) for s, m in db.execute('SELECT ticker, fee_multiplier FROM series')}
    fee_mult = {t: fee_by_series.get(s, 1.0) for t, s in db.execute('SELECT ticker, series_ticker FROM markets')}
    n_games = len(samples)
    n_wp = sum(1 for rows in samples.values() for r in rows if r['wp'] is not None and r['wp'] >= 0)

    jumps, horizons = lead_lag(samples, book)
    lines = []
    w = lines.append
    w('# ESPN win probability against Kalshi: lead/lag and the discrepancy trade')
    w('')
    w(f'Generated {time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime())} by `research/espn_leadlag.py` from the desk\'s '
      f'own recordings: {n_games} games, {n_wp:,} ESPN samples with a fresh win probability, '
      f'{len(results)} game tickers settled. Leagues and dates as recorded (see the extract).')
    w('')
    w('## A mapping bug, found by this study')
    w('')
    w(f'In {len(swapped)} games the desk matched ESPN to Kalshi with home and away swapped: ESPN\'s home win '
      'probability moved OPPOSITE to the Kalshi mid of the ticker the desk called home (within-game correlation '
      + ', '.join(f'{v:.2f}' for v in sorted(swapped.values())) + '). ESPN\'s number was applied to the other team. '
      'Cause: sports_live paired team names in the game\'s order with ticker codes in sorted order, and when the '
      'codes did not match ESPN\'s abbreviations both orientations scored the same and the tie picked one. Fixed '
      '2026-09-23 (each ticker is named from its own market; an ambiguous match is refused). Everything below is '
      'computed with those games corrected; section 2 also shows what the desk would have traded as recorded.')
    w('')
    w('## What the data can resolve')
    w('')
    w('ESPN was sampled every 30 s and the Kalshi book about once per desk cycle (15-25 s). A move is therefore placed '
      'only to within half a minute, and the Kalshi mid "at" a moment can be a cycle old, which makes Kalshi look LATER '
      'than it was. The 0/5/15 s horizons the plan asks for need the Kalshi WebSocket book on game markets, which the '
      'desk does not record for sports yet. What follows can say whether Kalshi was already most of the way there when '
      'ESPN\'s new number first appeared, and whether the trade paid at settlement.')
    w('')
    w('## 1. Lead/lag around ESPN jumps')
    w('')
    w('Each row: ESPN jumps of that size between consecutive samples (<= 45 s apart). "Kalshi at +h" is the Kalshi '
      'home mid at h seconds after the sample that first showed the jump, minus the mid at the sample before it, '
      'in the direction of the jump, as a share of the ESPN jump. 100% means Kalshi moved as far as ESPN did.')
    w('')
    w('| ESPN jump | n | median jump | ' + ' | '.join(f'Kalshi at +{h}s' for h in horizons) +
      ' | share with Kalshi >= half-way at +0s | Kalshi trend in the minute before |')
    w('|' + '---|' * (5 + len(horizons)))
    buckets = [(0.05, 0.10), (0.10, 0.20), (0.20, 1.01)]
    for lo, hi in buckets:
        js = [j for j in jumps if lo <= j['dwp'] < hi]
        if not js:
            w(f'| {lo:.2f}-{hi:.2f} | 0 |' + ' |' * (3 + len(horizons)))
            continue
        cells = []
        for h in horizons:
            fr = [j['moves'][h] / j['dwp'] for j in js if j['moves'][h] is not None]
            cells.append(f'{100 * med(fr):.0f}% (n={len(fr)})' if fr else '-')
        half = [j for j in js if j['moves'][0] is not None]
        share = sum(1 for j in half if j['moves'][0] >= 0.5 * j['dwp']) / len(half) if half else float('nan')
        pre = [j['pretrend'] for j in js if j['pretrend'] is not None]
        w(f'| {lo:.2f}-{min(hi, 1):.2f} | {len(js)} | {fmt(med([j["dwp"] for j in js]))} | ' + ' | '.join(cells) +
          f' | {fmt(100 * share, 0)}% | {fmt(100 * med(pre), 1)}c |')
    w('')
    # Where Kalshi's own move happened, in cents and sign-aligned to ESPN's jump: in the minute
    # BEFORE the ESPN sample preceding the jump (so at least 30 s before ESPN showed it), or in the
    # five minutes after. Same jumps in both, so the two are comparable.
    both = [j for j in jumps if j['pretrend'] is not None and j['moves'][300] is not None]
    before_c = statistics.fmean(j['pretrend'] for j in both) * 100 if both else float('nan')
    after_c = statistics.fmean(j['moves'][300] for j in both) * 100 if both else float('nan')
    espn_c = statistics.fmean(j['dwp'] for j in both) * 100 if both else float('nan')
    big = [j for j in both if j['dwp'] >= 0.10]
    big_before = statistics.fmean(j['pretrend'] for j in big) * 100 if big else float('nan')
    big_after = statistics.fmean(j['moves'][300] for j in big) * 100 if big else float('nan')
    w(f'**Reading.** Over {len(both)} ESPN jumps averaging {fmt(espn_c, 1)}c, Kalshi moved {fmt(before_c, 1)}c in the '
      f'minute before ESPN\'s previous sample and {fmt(after_c, 1)}c in the five minutes after ESPN showed the jump '
      f'(jumps of 10c or more: {fmt(big_before, 1)}c before, {fmt(big_after, 1)}c after). Two things follow. Kalshi '
      'never moves anything like as far as ESPN does: most of an ESPN jump is not followed at all. And for the large '
      'jumps, where the lane would find its biggest "edges", as much or more of Kalshi\'s move happened at least 30 s '
      'BEFORE ESPN published as after it. At this resolution that is Kalshi moving first and ESPN overshooting, which '
      'is the case the plan says makes the discrepancy trade wrong. The Kalshi book here can be a cycle stale, which '
      'hides Kalshi moves inside the last cycle; it cannot create the move measured before the previous ESPN sample.')
    w('')
    w('## 2. The trade: ESPN above the Kalshi ask, held to settlement')
    w('')
    w('Taker at the recorded ask (maker fills cannot be simulated from this data), $5 per trade, the taker fee rounded '
      'up per order, first trigger per game and side, P&L at settlement. The "edge >= ..." rows are the plan\'s raw '
      'trigger on ESPN\'s number; the "router" row approximates what the desk\'s router would let the lane send (the '
      '50/50 blend with the mid, the 0.15 gap and 0.12 edge caps, the uncertainty buffer, min_edge, the cheap-side '
      'rule; see backtest()).')
    w('')
    w('| mapping | trigger | cheap side | n | wins | mean ESPN p | mean ask | P&L | per trade | sd | t | 80% lower | fee/trade |')
    w('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
    variants = [('router', None), (0.08, 0.45), (0.05, 0.45), (0.08, None), (0.05, None), (0.03, None)]
    main = None
    summaries = []
    for label, src in (('corrected', samples), ('as recorded', as_recorded)):
        for thr, cap in variants:
            if thr == 'router':
                s = summarize(backtest(src, book, results, fee_mult, 0, None, router=True))
                trig = 'router (approx.)'
            else:
                s = summarize(backtest(src, book, results, fee_mult, thr, cap))
                trig = f'edge >= {thr:.2f}'
            if label == 'corrected':
                summaries.append((trig, cap, s))
            if main is None and thr != 'router':
                main = s
            if s['n'] == 0:
                w(f'| {label} | {trig} | {"<= %.2f" % cap if cap else "any"} | 0 |' + ' |' * 9)
                continue
            w(f'| {label} | {trig} | {"<= %.2f" % cap if cap else "any"} | {s["n"]} | {s["wins"]} | '
              f'{fmt(s["p_mean"])} | {fmt(s["ask_mean"])} | ${fmt(s["total"])} | ${fmt(s["mean"])} | ${fmt(s["sd"])} | '
              f'{fmt(s["t"])} | ${fmt(s["lower80"])} | ${fmt(s["fee_mean"])} |')
    w('')
    by = defaultdict(list)
    for t in backtest(samples, book, results, fee_mult, 0.08, 0.45):
        by[t['league']].append(t)
    if by:
        w('By league, the plan\'s trigger (edge >= 0.08, ask <= 0.45):')
        w('')
        w('| league | n | wins | mean ESPN p | P&L | per trade |')
        w('|---|---|---|---|---|---|')
        for lg, ts in sorted(by.items()):
            s = summarize(ts)
            w(f'| {lg} | {s["n"]} | {s["wins"]} | {fmt(s["p_mean"])} | ${fmt(s["total"])} | ${fmt(s["mean"])} |')
        w('')
    if main and main['n']:
        calib = main['wins'] / main['n'] - main['p_mean']
        losing = [t for t, _, s in summaries if s.get('n') and s['mean'] < 0]
        winning = [t for t, _, s in summaries if s.get('n') and s['mean'] >= 0]
        router = next((s for t, _, s in summaries if t.startswith('router')), {'n': 0})
        w(f'**Reading.** On the plan\'s trigger (edge >= 0.08, cheap side): {main["n"]} trades, {main["wins"]} won, '
          f'against {fmt(main["p_mean"] * main["n"], 1)} that ESPN\'s probabilities expected and '
          f'{fmt(main["ask_mean"] * main["n"], 1)} that the Kalshi asks implied: ${fmt(main["mean"])} a trade, t '
          f'{fmt(main["t"])}, 80% lower band ${fmt(main["lower80"])}. '
          + (f'The router approximation: {router["n"]} trades, ${fmt(router["mean"])} a trade, t {fmt(router["t"])}. '
             if router.get('n') else 'The router approximation made no trade. ')
          + (f'{len(losing)} of {len(losing) + len(winning)} triggers lose on average'
             + (f'; these do not: {", ".join(winning)}. ' if winning else '. '))
          + 'When ESPN and Kalshi disagree, the outcomes follow Kalshi\'s price more than ESPN\'s number: '
          'conditioning on a large disagreement selects ESPN\'s errors. By the plan\'s own gate ("a reading that is '
          'not negative") the lane does not qualify on this evidence; the live-sizing rule and the lane\'s loss stop '
          'bound what finding that out live costs, 30 settled trades at a time.')
    else:
        w('**Reading.** No trade met the lane\'s rule in the recorded data.')
    w('')
    w('## What would settle the lead/lag question')
    w('')
    w('Record the Kalshi WebSocket ticker for the game markets while ESPN is polled faster (the summary endpoint every '
      '5-10 s for games in progress), then repeat section 1 at 0/5/15 s. The desk already runs a WebSocket quote feed '
      'for the commodity windows (MarketQuoteFeed); pointing a second one at live game markets is the smallest change '
      'that answers this properly.')
    os.makedirs(os.path.dirname(report), exist_ok=True)
    with open(report, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--extract', action='store_true', help='copy the needed rows from the live database (desk stopped)')
    ap.add_argument('--live', default=LIVE)
    ap.add_argument('--db', default=EXTRACT)
    ap.add_argument('--report', default=REPORT)
    a = ap.parse_args()
    if a.extract:
        extract(a.live, a.db)
        return
    if not os.path.exists(a.db):
        sys.exit(f'no extract at {a.db}; run with --extract while the desk is stopped')
    analyse(a.db, a.report)


if __name__ == '__main__':
    main()
