#!/usr/bin/env python3
"""Write the fake exchange's synthetic tape fixture: tests/fake_exchange/fixtures/natgas_tape.csv.gz.

A made-up natural gas book in exactly the format of the old desk's archive day files
(scripts/archive_quotes.py --table commodity_book_events), so the fake's tape reader is tested on
the format real archives use without putting real data in the repo. Seeded: running it again
writes the same bytes (gzip mtime 0).

What it holds, 25 minutes from 2026-10-06 15:30:00.5Z:
  - window 1, KXNATGAS15M-26OCT061145-45 (closes 15:45:00Z): its snapshot, then deltas;
  - the close: window 1's empty snapshot (as recorded 2026-09-24), then the two `ok` replies to
    our delete and add, each taking a seq number, then window 2's snapshot ~9.5 s later;
  - window 2, KXNATGAS15M-26OCT061200-00 (closes 16:00:00Z): deltas;
  - one delta lost on the way (its seq number missing; the book is wrong until the next snapshot);
  - a reconnect at 15:52Z: a `connect` row, `subscribed`, seq starting again at 1, window 2
    re-seeded by its snapshot, and window 1 (closed) sent again as an empty snapshot with
    market_id "" (as recorded at 05:20:39Z 2026-09-24);
  - a `dropped` marker row; a delta our own order caused (client_order_id in its frame);
  - most deltas trimmed (raw_json NULL, as scripts/backfill_book_columns.py --trim leaves them),
    some whole, and a few in the pre-2026-09-24 shape (price and delta 0 in the columns, the frame
    intact), which the reader must take from raw_json;
  - frames ending in the venue's newline, so quoted CSV fields span lines.

It also writes natgas_tape.expect.json: row counts by kind, the true final book of window 2 (the
generator's own, not a replay of the file) and the seq gaps, for the C++ test to check against.

  python3 tests/fake_exchange/make_tape.py
"""
import csv, gzip, io, json, os, random
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'fixtures', 'natgas_tape.csv.gz')
EXPECT = os.path.join(HERE, 'fixtures', 'natgas_tape.expect.json')
COLS = ['id', 'ts_received_ms', 'ts_exchange_ms', 'ticker', 'kind', 'sequence', 'side', 'price', 'delta', 'raw_json']
W1, W2 = 'KXNATGAS15M-26OCT061145-45', 'KXNATGAS15M-26OCT061200-00'
IDS = {W1: '0d6c2f8a-6f1e-4c3a-9a51-2b6f0e1d4c11', W2: '7b3e91c4-2d5a-4f08-8c6e-91d2a4b7e522'}


def ms(s):
    return int(datetime.strptime(s, '%Y-%m-%dT%H:%M:%S.%fZ').replace(tzinfo=timezone.utc).timestamp() * 1000)


T_START, CLOSE_1, T_RECONNECT, T_END = (ms('2026-10-06T15:30:00.500Z'), ms('2026-10-06T15:45:00.000Z'),
                                        ms('2026-10-06T15:52:00.000Z'), ms('2026-10-06T15:55:00.000Z'))


def dollars(cents_tenths):          # tenths of a cent -> "0.5100"
    return f'{cents_tenths / 1000:.4f}'


def count(hundredths):              # hundredths of a contract -> "30.00"
    return f'{hundredths / 100:.2f}'


class Book:
    """A two-sided book in tenths of a cent: YES bids and NO bids, never crossed."""

    def __init__(self, rng, mid):
        self.rng = rng
        self.yes, self.no = {}, {}
        for d in range(1, 13):
            self.yes[(mid - d * 10) // 10 * 10] = self.size()
            self.no[(1000 - mid - d * 10) // 10 * 10] = self.size()
        for p in range(10, 100, 10):  # the deep cheap levels the recording shows
            self.yes[p] = self.yes.get(p, 0) + rng.choice([45000, 90000, 154000, 277420])
            self.no[p] = self.no.get(p, 0) + rng.choice([45000, 90000, 171800, 277920])

    def size(self):
        r = self.rng.random()
        if r < 0.05:
            return self.rng.choice([7, 20, 460])          # fractional contracts happen
        return self.rng.choice([100, 200, 500, 1000, 1500, 2000, 5000, 30000])

    def best(self, side):
        lv = self.yes if side == 'yes' else self.no
        return max(lv) if lv else 0

    def fits(self, side, p):        # a level here would not cross the other side
        other = self.best('no' if side == 'yes' else 'yes')
        return 0 < p < 1000 and p + other < 1000

    def change(self, side, p, d):
        lv = self.yes if side == 'yes' else self.no
        v = lv.get(p, 0) + d
        assert v >= 0, (side, p, d)
        if v == 0:
            lv.pop(p, None)
        else:
            lv[p] = v

    def step(self):
        """One change the venue might send: (side, price, delta)."""
        rng = self.rng
        side = rng.choice(['yes', 'no'])
        lv = self.yes if side == 'yes' else self.no
        best = self.best(side)
        r = rng.random()
        if r < 0.55 and lv:               # a size change near the touch
            near = sorted(lv)[-6:]
            p = rng.choice(near)
            if rng.random() < 0.5:
                d = self.size()
            else:
                d = -min(lv[p], self.size())
            return side, p, d
        if r < 0.80 and self.fits(side, best + 10):   # improve the touch by a cent
            return side, best + 10, self.size()
        if r < 0.95 and len([p for p in lv if p > 100]) > 3:   # the touch leaves
            return side, best, -lv[best]
        p = best - 10 * rng.randint(1, 5)
        return (side, p, self.size()) if p > 100 else (side, best, self.size())


def main():
    rng = random.Random(20261006)
    rows, nid = [], [0]
    kinds, lost = {}, []

    def row(t, ticker, kind, seq, side=None, price=None, delta=None, raw=None, ex=None):
        nid[0] += 1
        kinds[kind] = kinds.get(kind, 0) + 1
        rows.append([nid[0], t, ex, ticker, kind, seq, side, price, delta, raw])

    def frame(obj):                 # the venue's frame, with the newline it sends
        return json.dumps(obj, separators=(',', ':')) + '\n'

    def snapshot(t, ticker, book, seq, market_id=None):
        msg = {'market_ticker': ticker, 'market_id': IDS[ticker] if market_id is None else market_id}
        if book is not None and book.yes:
            msg['yes_dollars_fp'] = [[dollars(p), count(q)] for p, q in sorted(book.yes.items())]
        if book is not None and book.no:
            msg['no_dollars_fp'] = [[dollars(p), count(q)] for p, q in sorted(book.no.items())]
        row(t, ticker, 'orderbook_snapshot', seq, raw=frame({'type': 'orderbook_snapshot', 'sid': 2, 'seq': seq, 'msg': msg}))

    def delta(t, ticker, side, p, d, seq, style, cid=None):
        ex = t - rng.randint(15, 45)    # the venue's clock, 15-45 ms before our receipt
        msg = {'market_ticker': ticker, 'market_id': IDS[ticker], 'price_dollars': dollars(p),
               'delta_fp': count(d), 'side': side,
               'ts': datetime.fromtimestamp(ex / 1000, timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f') + 'Z',
               'ts_ms': ex}
        if cid:
            msg['client_order_id'] = cid
        raw = frame({'type': 'orderbook_delta', 'sid': 2, 'seq': seq, 'msg': msg})
        if style == 'trimmed':          # the columns hold it all; the frame is dropped
            row(t, ticker, 'orderbook_delta', seq, side, p / 1000, d / 100, None, ex)
        elif style == 'old':            # before 2026-09-24: zeros in the columns, the frame intact
            row(t, ticker, 'orderbook_delta', seq, side, 0.0, 0.0, raw, ex)
        else:
            row(t, ticker, 'orderbook_delta', seq, side, p / 1000, d / 100, raw, ex)

    def reply(t, typ, seq, ident, tickers):
        row(t, None, typ, seq, raw=frame({'id': ident, 'sid': 2, 'seq': seq, 'type': typ,
                                          'msg': {'market_tickers': tickers}}))

    seq = 0
    row(T_START - 400, None, 'connect', None, raw=json.dumps({'subscribed': [W1]}))
    row(T_START - 300, None, 'subscribed', None, raw=frame({'id': 2, 'type': 'subscribed',
                                                            'msg': {'channel': 'orderbook_delta', 'sid': 2}}))
    book1 = Book(rng, 470)
    seq += 1
    snapshot(T_START, W1, book1, seq)
    t, book, ticker = T_START, book1, W1
    book2 = None
    reconnected = closed = False
    n = 0
    while True:
        t += max(1, int(rng.expovariate(1 / 300.0)))
        if not closed and t >= CLOSE_1:
            # the close: an empty snapshot for window 1, our delete and add (each an ok with a seq),
            # then window 2's first book
            seq += 1
            snapshot(CLOSE_1 + 87, W1, None, seq)
            seq += 1
            reply(CLOSE_1 + 120, 'ok', seq, 3, [W1, W2])
            seq += 1
            reply(CLOSE_1 + 140, 'ok', seq, 4, [W2])
            book2 = Book(rng, 520)
            seq += 1
            snapshot(CLOSE_1 + 9564, W2, book2, seq)
            book, ticker, closed = book2, W2, True
            t = CLOSE_1 + 9600
            continue
        if closed and not reconnected and t >= T_RECONNECT:
            row(T_RECONNECT, None, 'connect', None, raw=json.dumps({'subscribed': [W1, W2]}))
            row(T_RECONNECT + 60, None, 'subscribed', None,
                raw=frame({'id': 1, 'type': 'subscribed', 'msg': {'channel': 'orderbook_delta', 'sid': 2}}))
            seq = 1
            snapshot(T_RECONNECT + 180, W1, None, seq, market_id='')   # the closed window, sent again
            seq += 1
            snapshot(T_RECONNECT + 181, W2, book2, seq)               # the truth again
            reconnected = True
            t = T_RECONNECT + 200
            continue
        if t >= T_END:
            break
        side, p, d = book.step()
        book.change(side, p, d)
        seq += 1
        n += 1
        if closed and not reconnected and n % 997 == 0 and not lost:
            lost.append(seq)            # lost on the way: the number is spent, the change unseen
            continue
        style = 'whole' if n % 50 == 0 else 'old' if n % 211 == 0 else 'trimmed'
        cid = '9f1c2a7e-4b3d-4e8f-a6c5-0d2e1f3a4b5c' if n == 400 else None
        if cid:
            style = 'whole'
        delta(t, ticker, side, p, d, seq, style, cid)
        if n == 1500:
            row(t + 1, None, 'dropped', None, raw=json.dumps({'rows': 0, 'note': 'a marker the recorder writes'}))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    text = io.StringIO()
    w = csv.writer(text)   # the archive's own dialect: QUOTE_MINIMAL, \r\n
    w.writerow(COLS)
    for r in rows:
        w.writerow(r)
    raw = text.getvalue().encode()
    with open(OUT, 'wb') as f:
        with gzip.GzipFile(filename='', mode='wb', fileobj=f, mtime=0, compresslevel=9) as g:
            g.write(raw)
    expect = {
        'rows': len(rows), 'kinds': kinds, 'lost_seq': lost,
        'first_ms': rows[0][1], 'last_ms': rows[-1][1],
        'final': {W2: {'yes': [[dollars(p), count(q)] for p, q in sorted(book2.yes.items())],
                       'no': [[dollars(p), count(q)] for p, q in sorted(book2.no.items())]}},
        'close_ms': {W1: CLOSE_1, W2: ms('2026-10-06T16:00:00.000Z')},
    }
    with open(EXPECT, 'w') as f:
        json.dump(expect, f, indent=1, sort_keys=True)
        f.write('\n')
    print(f'{OUT}: {len(rows)} rows, {len(raw) / 1e3:.0f} kB of CSV, {os.path.getsize(OUT) / 1e3:.0f} kB gzip')
    print(f'kinds {kinds}; lost seq {lost}')


if __name__ == '__main__':
    main()
