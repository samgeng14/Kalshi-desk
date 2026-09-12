# Kalshi perpetuals: where the basis actually dislocates

`python3 scripts/perp_basis.py`. Measured on 154,068 recorded quotes across 20 instruments,
2026-09-08 to 2026-09-11. Three days. Read the last section before sizing anything.

## A bug first: the desk was recording a sentinel as a price

Kalshi sends perp prices as fixed-point integers scaled by 10^4 and uses INT64_MAX to mean "no
order on this side". Parsed as a number that is **922337203685477.6**, and it went straight into
`perp_quotes` as an ask. It made the basis read 6.8e16 bps on a dozen instruments, and
`r.price = m->ask` in the entry path would have posted an offer at that price. The existing
`stop_bps` guard happened to reject anything that extreme, which is the only reason no order was
sent on it. Rejected at the parse now: a side price above 1e9 is absent, not a quote.

410 rows on NEAR, 408 on DOGE, 391 on BCH and so on down to 23 on GOLD were affected.

## The rule was pointed at the two instruments where it can never fire

`algo_markets` was `["KXBTCPERP", "KXETHPERP"]` with a 25 bps entry. Over the window:

| instrument | observations | reached 25 bps | basis sd | spread |
|---|---:|---:|---:|---:|
| KXBTCPERP | 8,779 | 4.40% | 9.4 bps | 0.4 bps |
| KXETHPERP | 8,584 | 0.48% | 3.0 bps | 0.4 bps |

Those percentages are misleading on their own: counted as **distinct excursions** rather than
per quote, BTC produced fewer than five in three days and ETH none worth the name. The basis on
the two most liquid instruments is small and persistent, not oscillating.

The dislocations are on the thin alts, which is unsurprising: a basis is an arbitrage that
somebody has to be paid to close, and nobody is being paid much to close BTC.

## What clears the round trip

Entry rests at the touch, exit crosses, so the round trip is maker 5 bps + taker 12 = **17 bps**.
That line removes most of the gross edge.

| instrument | threshold | trades | gross | net | 95% CI on net | per day |
|---|---:|---:|---:|---:|---|---:|
| **KXVVVPERP** | **30 bps** | **34** | **+107.7** | **+90.7** | **[+14.3, +184.2]** | **14** |
| KXVVVPERP | 20 bps | 106 | +32.6 | +15.6 | [−8.7, +47.8] | 45 |
| KXZECPERP | 20 bps | 22 | +42.8 | +25.8 | [−13.2, +81.4] | 9 |

One instrument, one threshold, with the whole interval above zero.

## Three ways this could have been an illusion, and what each check showed

**P&L measured on the basis instead of the price.** The basis closes either because the perp moved
or because the index did, and only the first pays. Scoring the basis made ZEC look like it lost
2.9 bps at a 10 bps threshold when the price it actually held made +15.3. Everything above is
priced on the perp.

**A direction bet wearing a basis signal.** Three days of falling crypto would make "short the rich
perp" print money for the wrong reason. Split by side:

| instrument | index over the window | shorts | longs |
|---|---:|---:|---:|
| KXVVVPERP | **+67 bps** | +11.3 | +15.5 |
| KXZECPERP | **−639 bps** | +16.1 | +14.2 |
| KXKSHIBPERP | −546 bps | +255.7 (n=8) | +14.6 |

Both sides pay, and the side fighting the index pays: ZEC's longs made money while its index fell
6.4%. That is the check this passes most convincingly.

**Counting quotes as trades.** Consecutive quotes above the threshold are one dislocation. Counted
per quote, VVV's 34 excursions become several hundred "samples" that share an outcome.

## Before sizing this up

- **34 trades over three days on one instrument.** The interval is above zero and also enormous
  ([+14, +184]); that width is the sample telling you it does not know the size of the effect.
- **The entry assumes a maker fill at the touch that nobody promised.** This is exactly where the
  same assumption already failed elsewhere on this desk: on 34 real maker fills in event markets,
  the half-spread captured (0.0078) and the drift against the fill (−0.0102 at 60s) cancelled to
  nothing. The fills you get are the ones someone wanted to give you.
- **KXVVVPERP turns over $2.0M a day and offers 2x leverage.** It is the thinnest kind of market
  in the set, which is *why* the basis dislocates there. Size that is comfortable in BTC is not
  comfortable here, and margin turns a wrong backtest into a liquidation rather than a loss.
- Funding is **zero on 18 of the 20 instruments** (only BTC is nonzero, at 0.00017). There is no
  carry trade here; the basis is the only thing to trade.

The honest next step is not more leverage, it is more excursions: run it at one contract, record
the fills, and compare the fill rate and post-fill drift against what this assumed.

## Sizing arithmetic (2026-09-11)

A perp **price is dollars per contract already**. `contract_size` is the underlying per contract,
not a notional multiplier: VVV marks at $2.4426 a contract with `contract_size` 0.1, so a contract
is 0.1 VVV and the token is $24.43. Notional is `count x price`, which is what `PerpsDesk::rails`
computes. Multiplying price by `contract_size` to "get notional" is wrong by 1/contract_size and
in VVV's case understates it 10x.

| contracts | notional | leverage on $49.86 | rails verdict |
|---|---|---|---|
| 12 | $29.31 | 0.59x | passes |
| 13 | $31.76 | 0.64x | **rejected**, over the $30 order cap |
| 100 | $244.26 | 4.90x | **rejected**, order cap and the 2x leverage cap |

At 12 contracts the measured +90.7 bps net is ~$0.27 a trade and the 60 bps stop is ~$0.18, so the
$3.00 daily stop is about 17 stopped trades.

## The config does not drive a live database

`PerpsDesk` seeds `algo_state_` from `params.perps.algo` and then lets the saved `perps_algo:v1`
row overwrite it:

```cpp
json settings = algo_state_["settings"];                  // from config
if (parsed.contains("settings")) settings.update(parsed["settings"]);   // the DB wins
```

So on any database that has run the rule once, editing `config/desk.json` changes nothing. As of
2026-09-11 the saved row held `contracts: 1`, `entry_bps: 25`, `markets: [KXBTCPERP, KXETHPERP]` --
three days of paper against the two instruments where the basis does not revert. Push settings
through `POST /api/perps/algo` (`scripts/perps_go_live.sh`), and push them **before** `paper:false`:
the rule is already `running`, so flipping the mode first goes live on whatever the DB still holds.

Three days of paper produced 38 entries, all KXBTCPERP, all `canceled` with "paper entry never
filled", $0.00 realized. The paper fill model requires the touch to trade *through* the posted
price; a real maker fill happens *at* it. Paper understates the fill rate, so the fill-rate
question in the section above is still open -- the paper run did not answer it.

---

# Correction, 2026-09-11: the VVV result does not reproduce

Everything above this line was measured on a partial window. Re-run over all 154,135 stored perp
quotes (2.4 days, every instrument), scoring the rule the way `src/perps.cpp` actually executes it:
maker in at the touch, taker out, 17 bps round trip, and the **22 bps entry floor the code enforces
whatever `entry_bps` says**:

```cpp
double needed = fee_bps(ticker,false) + fee_bps(ticker,true) + 5.0;   // 5 + 12 + 5
if (std::fabs(basis) < std::max(entry_bps, needed)) continue;
```

That floor matters: an `entry_bps` of 15 or 20 is silently raised to 22, so any backtest run below
22 bps is scoring trades the desk will never take.

| instrument | trades | net bps | 95% CI | |
|---|---|---|---|---|
| KXAAVEPERP | 6 | +153.6 | [+62.5, +244.8] | clears |
| KXSUIPERP | 7 | +214.2 | [+4.7, +428.3] | clears, interval 90x wide |
| KXNEARPERP | 7 | +18.6 | [+5.5, +32.0] | clears |
| KXZECPERP | 18 | +6.2 | [−15.7, +28.6] | spans zero |
| KXWLDPERP | 9 | +22.5 | [−4.4, +51.5] | spans zero |
| **KXVVVPERP** | **87** | **−2.4** | **[−18.1, +13.5]** | **negative** |

**KXVVVPERP is the instrument with by far the most observations and it has no edge.** The earlier
+90.7 bps over 34 trades was the artefact; 87 trades on the full window say −2.4. Sizing a live
position on that number would have been sizing on noise.

The three that clear have six or seven trades each out of sixteen instruments tested. Three
clearing at 95% is roughly what testing sixteen things produces by chance, and the two with the
largest point estimates have the widest intervals, which is the signature of one lucky trade in a
small sample rather than an effect. A 22 bps entry with a 13 bps spread, as on AAVE, is a basis
excursion of less than two spreads: most of what is being measured there is the spread itself.

## What the numbers do support

155 entries fired across the whole book in 2.4 days. That is ~65 a day, so two weeks of running
every instrument is ~900 real fills -- enough to separate these instruments properly, which 2.4
days cannot. The rule is therefore configured to **cover the book at a size that collects fills**,
not to press an edge that has not been established:

- 15 instruments, `entry_bps` 22 (the floor, so the setting is honest about what it does)
- `notional_dollars` 15, not a contract count: a WLD contract is $0.40 and a ZEC contract is $11,
  so one contract count cannot mean one risk across the book
- `max_total_notional` 45 -- three concurrent positions, 0.90x on $49.86
- `daily_loss_stop` 4.00 -- a 60 bps stop costs $0.09, so about 44 stopped trades

BTC, ETH, GOLD and SILVER quote inside 0.5 bps and their basis reached 22 bps five times in 2.4
days between them. They are watched, never traded.

The number to watch is not P&L. It is the **fill rate** on the post-only entries and the drift in
the 60 seconds after a fill. The paper run answered neither: 38 paper entries over three days, all
KXBTCPERP, every one `canceled` with "paper entry never filled", because the paper model requires
the touch to trade *through* the posted price while a real maker fill happens *at* it.
