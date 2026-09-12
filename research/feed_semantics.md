# What the Kalshi margin feed actually sends

Established by reading 6,017,346 recorded deltas, 18,620 trade prints and 42,678 ticker frames
from `data/raw/source=kalshi_margin/hour=2026-09-11T14` and the two hours after it. Every claim
below was checked against the tape. Four of them contradict what the code assumed, and each of
those four was silent: the engine produced numbers, they were just numbers about nothing.

## 1. There is no `last_update_reason`

Every `orderbook_delta` on this feed carries exactly:

```
{market_ticker, price, delta, side, ts_ms}
```

Zero of 6,017,346 deltas had a `last_update_reason`. The event-contract sockets do send it, which
is where the assumption came from.

**What it broke.** `Flow.on_delta` classified by reason, and with the reason always absent every
negative delta fell through to `return`. Result: `trade_imbalance`, `cancel_imbalance`,
`aggressive_buy`, `aggressive_sell`, `cancel_bid` and `cancel_ask` were identically zero across
all eight windows — 48 of the 113 feature columns. They would have emitted as clean 0.0 values,
correlated with nothing, and looked like a genuine negative result. The execution simulator was
worse: it required `reason in TRADE_REASONS` to fill a passive order, so every passive mode would
have reported a fill rate of exactly zero, which reads as "passive quoting never fills here"
rather than "the simulator cannot see fills."

**What replaces it.** Trades arrive on their own channel (sid 3):

```
{trade_id, market_ticker, price, count, taker_side, ts_ms}
```

That is now the only source of trade volume. A negative delta is called a cancel when no trade
printed at the same price within 300 ms, and the match rate is reported rather than assumed.

## 2. `taker_side` is the taker's own side

Not the side consumed. Established by matching each trade to negative deltas at the same price
within 300 ms and keeping only exact magnitude matches:

| taker_side | consumed the ask | consumed the bid |
|---|---:|---:|
| `bid` | **3,483** | 180 |
| `ask` | 161 | **3,117** |

19.4:1 in both directions, over 6,941 exact matches.

So `taker_side: "bid"` is an aggressive **buyer** lifting the offer. Reading it the other way
inverts the sign of every trade-imbalance feature and every adverse-selection markout — which
would not have shown up as an error, only as a sign flip on results that were going to be near
zero anyway.

The matcher has to work in both directions. Deltas and trades arrive on separate subscriptions,
so the delta routinely lands *before* its own trade print; matching only trade→delta halves the
sample and skews which pairs survive. A one-directional matcher on this same tape gave 5.6:1 and
6.5:1 — the same answer, four times noisier, and close enough to ambiguous that an automated
check would have refused to call it.

## 3. `seq` is per subscription, not per market

The socket interleaves all 18 markets on sid 1 with one shared counter. Consecutive frames read
XRP 13230548, XRP 13230549, BTC 13230550.

Checking continuity per market flagged **41,799 gaps in 97,940 BTC deltas**. Checked per sid, the
true count is **zero gaps in 6,017,346 deltas**. There was no message loss at all.

**What it broke.** `Book.delta` marked the book stale on any per-market discontinuity and never
cleared it, because this feed does not re-snapshot. Every book would have gone stale within the
first few frames and stayed stale, so `row()` would have returned `None` for the entire tape and
the feature engine would have emitted an empty file — arguably the least harmful of the four,
because it fails visibly.

Continuity is now judged per sid, and a real gap marks **every** market on that subscription
stale, which is also the truth: a lost frame on a shared socket could have belonged to any of
them. Since the feed never re-snapshots, a gap is unrecoverable until the next reconnect — so the
recorder should force one rather than keep streaming into a book it knows is wrong.

## 4. Snapshots are usually full — but the first one of a session was empty

Of 36 `orderbook_snapshot` frames, the 18 from the **second** connect carry real depth (BTC: 245
bid levels, 108 ask). The 18 from the **first** connect of the session carry
`{market_ticker, market_id: ""}` and nothing else.

I initially read the first 18 and concluded the venue always sends an empty snapshot and streams
the book in as deltas. That was a generalisation from one connect, and it was wrong. The empty
snapshots are a race at subscribe time — the socket answered before it had a book to send — not a
property of the protocol. The corrected rule is narrower and matters more: **a book needs a
warm-up only when its snapshot arrived empty.** Charging 60 seconds to every book after every
reconnect would have discarded good rows for no reason.

Two consequences.

**Replay cannot start mid-tape.** Hour 16 contains no snapshot, so replaying it alone produces no
book at all. Any run must start from the hour holding the connect.

**An empty snapshot means a warm-up.** Top of book converges in about 0.1 s, but depth keeps
filling for minutes:

| seconds after connect | BTC bid levels | ask levels |
|---:|---:|---:|
| 0.05 | 1 | 1 |
| 0.84 | 15 | 17 |
| 6.8 | 31 | 39 |
| 54 | 37 | 33 |
| end of hour | 61 | 62 |

An L5 or L10 order-book imbalance computed at second 3 is a number about our subscription, not
about the market. Rows from a book rebuilt this way are suppressed for 60 s; a book that arrived
with a full snapshot is usable immediately. Every row carries `since_connect_s`, `n_bid_levels`
and `n_ask_levels` so a study can demand more rather than trust this threshold.

## 5. The socket reconnects, and the tape is truncated

Hour 14 contains **two** `_connect` events. On each, sequences restart at 1 and every book empties.
Without handling it the seq drops from ~13 million to 1, reads as a gap, and everything stays
stale for the rest of the tape. A reconnect now clears the books, the sid counters and any
simulated resting order — an order cannot be tracked across a discontinuity, so it is retired as
unfilled rather than carried into a book that no longer holds its level.

The final hour's gzip stream was never closed, because the recorder was killed mid-write. Readers
stop cleanly at the truncation and say so, rather than losing the hour or pretending it was whole.

## 6. The clock is behind

`latency_us` is consistently **negative** — median **−33.1 ms** across hour 14, with excursions to
−50 000 µs: the recorder's receive timestamp precedes the exchange's send timestamp. That is a local clock offset, not negative
latency. It matches the +0.107 s offset measured separately. Until it is fixed, no cross-venue
lead-lag result at a horizon under roughly a second is interpretable, because the offset is a
large fraction of the horizon.

## What the ticker channel adds (unused so far)

42,678 frames on sid 2, one per market roughly every 1.5 s, carrying `bid`, `ask`, `bid_size_fp`,
`ask_size_fp`, `last_trade_size_fp`, `volume`, `volume_24h`, `open_interest`, and — worth noting —
`funding_rate`, `reference_price`, `settlement_mark_price` and `liquidation_mark_price`, each
with its own `ts_ms`. `seq` is null on this channel, so it carries no continuity guarantee.

The mark and liquidation prices are a second, independent read on fair value that costs nothing
to record and is not currently used anywhere.

## How this was missed, and what now prevents it

`live_data_audit.py` passed. It checked counts, gaps, staleness and price alignment, and it was
verified on synthetic data — synthetic data I generated to match what the code assumed the feed
looked like. A test built from an assumption cannot falsify that assumption. All five properties
above survived it.

`feed_audit.py` replaces the assumptions with measurements taken from the tape:

- counts `last_update_reason` rather than expecting it, and says which classification path is live
- **re-derives** the `taker_side` orientation from trade/delta magnitude matches, both directions,
  and fails if the margin is under 2:1 or the sample under 200 — so if Kalshi flips the convention
  the audit reports it rather than the code silently inverting
- checks sequence continuity per sid, and prints what the per-market check would have claimed,
  so the difference stays visible
- separates empty snapshots from full ones and reports the warm-up curve
- counts connects and names truncated files
- reports the clock offset as a fail condition, not a footnote

The rule that generalises: an audit written against what the code expects will pass. It has to be
written against what arrived.
