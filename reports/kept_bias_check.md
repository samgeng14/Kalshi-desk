# Is `kept` the right target? No.

Top 10% of held-out entries by the conditional model, against a random selection of the same size (1,946 entries each).

## The decomposition

| term | model | random | model advantage |
|---|---:|---:|---:|
| median quoted spread | 18.3 | 8.2 | +10.1 |
| half-spread credited at POST | **+8.43** | +4.39 | +4.04 |
| markout 1s (what you own) [-5.94, -5.15] | -5.51 | -3.86 | **-1.65** |
| markout 5s (what you own) [-5.41, -4.26] | -4.80 | -4.02 | **-0.79** |
| markout 10s (what you own) [-6.05, -4.64] | -5.33 | -4.24 | **-1.09** |
| markout 60s (what you own) [-9.25, -5.93] | -7.56 | -5.09 | **-2.47** |
| kept 1s = half + markout | +2.64 | -0.03 | +2.68 |
| kept 5s = half + markout | +3.35 | -0.19 | +3.54 |
| kept 10s = half + markout | +2.82 | -0.42 | +3.24 |
| kept 60s = half + markout | +0.59 | -1.26 | +1.86 |

## What this says

The model beats random on `kept`. Every bit of that comes from the first row: it selects states whose quoted spread is +10.1 bps wider, worth +4.04 bps of post-time half-spread credit.

On the markout -- what the position is actually worth once you hold it -- the model is **worse than random at every horizon**: 1s -5.51 vs -3.86, 5s -4.80 vs -4.02, 10s -5.33 vs -4.24, 60s -7.56 vs -5.09.

So the selection is not finding good fills. It is finding wide spreads, and wide spreads on this venue carry worse adverse selection -- the execution study measured that correlation at **-0.905** and it is the reason this happens. `kept` adds a term the model can inflate to a term the model makes worse, and the cancellation leaves a residue that reads as alpha.

Both halves of that were already measured here, in two reports, and were not put together until the round trip forced actual fill prices onto both legs.

## The rule this leaves

- For a ONE-SIDED passive entry, the honest value is the **markout**: the mid once you hold the position, against the price you paid. Nothing else.
- `half_spread_at_post` is only earned if you fill without the mid moving, and the reason you fill is that it moved. It is not a credit, it is a hope.
- A two-sided maker who fills on BOTH sides does capture a spread, and for that strategy a spread term is legitimate. That is a different strategy from the one that was tested.
- Any future target that references a price from before the fill has to be checked this way before it is optimised against, because a model will find the reference before it finds the edge.

