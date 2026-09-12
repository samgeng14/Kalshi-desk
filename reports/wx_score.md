# Weather probability quality: model versus the price

Generated 2026-09-12 23:06:28Z. Negative `raw - market` means the model's own number beat the mid.

354 observations, 149 contracts, **31 settled event-days**, 6 distinct dates, 6 cities. Skipped: 748 horizons with a one-sided book, 8 with no quote yet.


## Headline

| | Brier | vs market |
|---|---|---|
| raw `weather_nws` | 0.1707 | **+0.0202** |
| Kalshi mid | 0.1504 | - |
| desk fair (w=0.50) | 0.1492 | -0.0012 |

`raw - market` 95% CI, clustered by event-day (31 clusters): **[-0.0218, +0.0621]**

`raw - market` 95% CI, clustered by DATE (6 clusters): [-0.0137, +0.0691]

`desk - market` 95% CI, clustered by event-day: [-0.0232, +0.0198]


The date-clustered interval is the honest one: every city on a given date sits under one synoptic pattern, so six dates is closer to the number of independent observations here than thirty-four event-days is, and far closer than 354 rows. It is also too few clusters for a bootstrap interval to be trusted, which is the finding rather than a caveat.


### By horizon

| horizon | n | Brier(raw) | Brier(market) | Brier(desk) | raw - market | desk - market |
|---|---|---|---|---|---|---|
| T-24h | 146 | 0.1561 | 0.1200 | 0.1293 | **+0.0361** | +0.0093 |
| T-12h | 107 | 0.1929 | 0.1682 | 0.1674 | **+0.0247** | -0.0009 |
| T-6h | 35 | 0.1883 | 0.1562 | 0.1519 | **+0.0321** | -0.0043 |
| T-3h | 27 | 0.1629 | 0.2011 | 0.1712 | **-0.0381** | -0.0298 |
| T-1h | 24 | 0.1566 | 0.1620 | 0.1512 | **-0.0054** | -0.0108 |
| T-30m | 15 | 0.1493 | 0.1971 | 0.1651 | **-0.0478** | -0.0320 |

### By city

| city | n | Brier(raw) | Brier(market) | Brier(desk) | raw - market | desk - market |
|---|---|---|---|---|---|---|
| KXHIGHAUS | 67 | 0.1934 | 0.1665 | 0.1710 | **+0.0269** | +0.0045 |
| KXHIGHCHI | 64 | 0.1537 | 0.1140 | 0.1235 | **+0.0396** | +0.0095 |
| KXHIGHLAX | 37 | 0.1898 | 0.2086 | 0.1791 | **-0.0188** | -0.0295 |
| KXHIGHMIA | 59 | 0.1746 | 0.1473 | 0.1490 | **+0.0273** | +0.0017 |
| KXHIGHNY | 60 | 0.1995 | 0.1455 | 0.1624 | **+0.0540** | +0.0170 |
| KXHIGHPHIL | 67 | 0.1243 | 0.1443 | 0.1240 | **-0.0200** | -0.0202 |

## Calibration of the raw model

| predicted | n | mean predicted | observed frequency |
|---|---|---|---|
| 0-5% | 39 | 0.023 | 0.051 |
| 5-10% | 31 | 0.074 | 0.129 |
| 10-20% | 76 | 0.147 | 0.224 |
| 20-30% | 110 | 0.254 | 0.218 |
| 30-50% | 88 | 0.359 | 0.386 |
| 50-70% | 7 | 0.546 | 0.429 |
| 70-101% | 3 | 0.917 | 0.667 |

## Market-weight race (section 17)

Brier of `(1-w)*raw + w*market` at each weight. w=1.00 is the market alone: any weight that does not beat it is contributing no independent information.


| w | Brier | vs market |
|---|---|---|
| 0.00 | 0.1707 | +0.0202 |
| 0.25 | 0.1571 | +0.0067 |
| 0.50 | 0.1492 | -0.0012 |
| 0.75 | 0.1470 | -0.0034 |
| 1.00 | 0.1504 | +0.0000 |

This is an in-sample sweep over every settled day the desk has. It is not a train/validation split, because there are not enough days to cut one, and it must not be read as a tuned weight.
