# Margin API: what production actually returns

Probed 2026-09-11 13:46 UTC against `https://external-api.kalshi.com/trade-api/v2` with `KXAAVEPERP`.

The `/margin/*` paths were read from Kalshi's documentation index, but a documented
endpoint is a claim and a response is evidence. This file is the evidence. Re-run
`scripts/perps_api_probe.py` and diff when something breaks.

**14 of 16 endpoints returned 200.**

## Not available

| endpoint | status | error |
|---|---|---|
| `/margin/exchange_status` | 404 | 404 page not found |
| `/margin/account_api_limits` | 404 | 404 page not found |

## Summary

| endpoint | status | rows | ms | note |
|---|---|---|---|---|
| `/margin/markets` | 200 | 20 | 90 | the market list; also the source of mark, reference, leverage, contract_size |
| `/margin/enabled` | 200 | - | 63 | documented production-access check |
| `/margin/exchange_status` | 404 | - | 61 | trading hours / maintenance |
| `/margin/account_api_limits` | 404 | - | 60 | this account rate-limit tier |
| `/margin/risk_parameters` | 200 | - | 65 | initial-margin multiplier, both liquidation thresholds |
| `/margin/fee_tier_rates` | 200 | 6 | 67 | ACTUAL maker/taker rates for this account |
| `/margin/fee_tiers` | 200 | - | 69 | tier table |
| `/margin/markets/KXAAVEPERP` | 200 | - | 67 | single market |
| `/margin/markets/KXAAVEPERP/orderbook` | 200 | - | 65 | REST book snapshot |
| `/margin/markets/KXAAVEPERP/candlesticks` | 200 | 60 | 80 | THE historical bar endpoint; check bid/ask/price OHLC are all present |
| `/margin/trades` | 200 | 5 | 65 | historical prints; check taker_side and cursor |
| `/margin/funding_rates/historical` | 200 | 378 | 89 | funding history |
| `/margin/funding_rates/estimate` | 200 | - | 62 | next funding estimate |
| `/margin/balance` | 200 | 4 | 79 | account equity |
| `/margin/positions` | 200 | 3 | 72 | open positions |
| `/margin/risk` | 200 | 0 | 71 | per-position liquidation prices |

## Candlestick history depth for KXAAVEPERP

Doubling walk-back, one hourly window per step.

| days ago | hourly bars |
|---|---|
| 1 | 24 |
| 2 | 23 |
| 4 | 24 |
| 8 | 24 |
| 16 | 0 |

Deepest window with data: **8 days ago**. History is per-contract: Kalshi
launched perps in late May 2026 and listed the first crypto contracts in early June,
while gold and silver listed 2026-09-10, so a newly listed market will show far less.
`reports/perps_data_audit.md` reports it per ticker.

## /margin/trades pagination

```
{
  "pages": 3,
  "ids": 300,
  "unique": 300,
  "looped": false
}
```

A `looped: true` means the cursor stopped advancing and paging must be bounded by
`min_ts` / `max_ts` rather than trusted to terminate.

## Schemas and samples

### `/margin/markets`

- status **200**, 90ms
- params: `{"status": "active"}`
- rate-limit headers: none returned

**schema**

```json
{
  "markets": [
    {
      "ask": "str(numeric)",
      "asset_class": "str",
      "bid": "str(numeric)",
      "contract_size": "str(numeric)",
      "exchange_index": "int",
      "fractional_trading_enabled": "bool",
      "leverage_estimate": "float",
      "leverage_estimates": {
        "1000": "float",
        "10000": "float",
        "100000": "float",
        "1000000": "float"
      },
      "liquidation_mark_price": {
        "price": "str(numeric)",
        "ts_ms": "int"
      },
      "long_leverage_estimates": {
        "1000": "float",
        "10000": "float",
        "100000": "float",
        "1000000": "float"
      },
      "open_interest": "str(numeric)",
      "open_interest_notional_value_dollars": "str(numeric)",
      "price": "str(numeric)",
      "reference_price": {
        "price": "str(numeric)",
        "ts_ms": "int"
      },
      "schedule": "null",
      "settlement_mark_price": {
        "price": "str(numeric)",
        "ts_ms": "int"
      },
      "short_leverage_estimates": {
        "1000": "float",
        "10000": "float",
        "100000": "float",
        "1000000": "float"
      },
      "status": "str",
      "tick_size": "str(nume
  ... truncated
```

**sample** (ids redacted)

```json
{
  "markets": [
    {
      "ask": "1.2547",
      "asset_class": "Crypto",
      "bid": "1.2533",
      "contract_size": "0.010000",
      "exchange_index": 0,
      "fractional_trading_enabled": false,
      "leverage_estimate": 3.2512,
      "leverage_estimates": {
        "1000": 3.2389,
        "10000": 3.1918,
        "100000": 2.9792,
        "1000000": 2.2047
      },
      "liquidation_mark_price": {
        "price": "1.2539",
        "ts_ms": 1789134397170
      },
      "long_leverage_estimates": {
        "1000": 3.5532,
        "10000": 3.4967,
        "100000": 3.2431,
        "1000000": 2.346
      },
      "open_interest": "83948.00",
      "open_interest_notional_value_dollars": "105220.423200",
      "price": "1.2534",
      "reference_price": {
        "price": "1.2547",
        "ts_ms": 1789134397000
      },
      "schedule": null,
      "settlement_mark_price": {
        "price": "1.2534",
        "ts_ms": 1789134393663
      },
      "short_leverage_estimates": {
        "1000": 3.239,
        "10000": 3.192,
        "100000": 2.9793,
        "1000000": 2.2048
      },
      "status": "active",
      "tick_size": "0.0001",
      "ticker": "KXAAVEPERP",
     
  ... truncated
```

### `/margin/enabled`

- status **200**, 63ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "enabled": "bool"
}
```

**sample** (ids redacted)

```json
{
  "enabled": true
}
```

### `/margin/exchange_status`

- status **404**, 61ms
- params: `{}`
- rate-limit headers: none returned
- error: `404 page not found`

### `/margin/account_api_limits`

- status **404**, 60ms
- params: `{}`
- rate-limit headers: none returned
- error: `404 page not found`

### `/margin/risk_parameters`

- status **200**, 65ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "initial_margin_multiplier": {
    "KXAAVEPERP": "float",
    "KXADAPERP": "float",
    "KXBCHPERP": "float",
    "KXBNBPERP": "float",
    "KXBTCPERP": "float",
    "KXDOGEPERP": "float",
    "KXDOTPERP": "float",
    "KXETHPERP": "float",
    "KXGOLDPERP": "float",
    "KXHBARPERP": "float",
    "KXHYPEPERP": "float",
    "KXKSHIBPERP": "float",
    "KXLINKPERP": "float",
    "KXLTCPERP": "float",
    "KXNEARPERP": "float",
    "KXSILVERPERP": "float",
    "KXSOLPERP": "float",
    "KXSUIPERP": "float",
    "KXVVVPERP": "float",
    "KXWLDPERP": "float",
    "KXXLMPERP": "float",
    "KXXRPPERP": "float",
    "KXZECPERP": "float"
  },
  "liquidation_margin_ratio_threshold": "int",
  "queue_entry_margin_ratio_threshold": "float"
}
```

**sample** (ids redacted)

```json
{
  "initial_margin_multiplier": {
    "KXAAVEPERP": 1.3,
    "KXADAPERP": 1.3,
    "KXBCHPERP": 1.3,
    "KXBNBPERP": 1.3,
    "KXBTCPERP": 1.3,
    "KXDOGEPERP": 1.3,
    "KXDOTPERP": 1.3,
    "KXETHPERP": 1.3,
    "KXGOLDPERP": 1.3,
    "KXHBARPERP": 1.3,
    "KXHYPEPERP": 1.3,
    "KXKSHIBPERP": 1.3,
    "KXLINKPERP": 1.3,
    "KXLTCPERP": 1.3,
    "KXNEARPERP": 1.3,
    "KXSILVERPERP": 1.3,
    "KXSOLPERP": 1.3,
    "KXSUIPERP": 1.3,
    "KXVVVPERP": 1.3,
    "KXWLDPERP": 1.3,
    "KXXLMPERP": 1.3,
    "KXXRPPERP": 1.3,
    "KXZECPERP": 1.3
  },
  "liquidation_margin_ratio_threshold": 1,
  "queue_entry_margin_ratio_threshold": 0.91
}
```

### `/margin/fee_tier_rates`

- status **200**, 67ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "fee_tier_rates": [
    {
      "fee_schedule": "str",
      "maker_fee_rate": "float",
      "taker_fee_rate": "float",
      "tier": "int"
    }
  ]
}
```

**sample** (ids redacted)

```json
{
  "fee_tier_rates": [
    {
      "fee_schedule": "kalshi_prime",
      "maker_fee_rate": 0.0002,
      "taker_fee_rate": 0.0012,
      "tier": 0
    },
    {
      "fee_schedule": "kalshi_prime",
      "maker_fee_rate": 0.00015,
      "taker_fee_rate": 0.001,
      "tier": 1
    }
  ]
}
```

### `/margin/fee_tiers`

- status **200**, 69ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "maker_fee_rates": {
    "KXAAVEPERP": "float",
    "KXADAPERP": "float",
    "KXBCHPERP": "float",
    "KXBNBPERP": "float",
    "KXBTCPERP": "float",
    "KXDOGEPERP": "float",
    "KXDOTPERP": "float",
    "KXETHPERP": "float",
    "KXGOLDPERP": "float",
    "KXHBARPERP": "float",
    "KXHYPEPERP": "float",
    "KXKSHIBPERP": "float",
    "KXLINKPERP": "float",
    "KXLTCPERP": "float",
    "KXNEARPERP": "float",
    "KXSILVERPERP": "float",
    "KXSOLPERP": "float",
    "KXSUIPERP": "float",
    "KXVVVPERP": "float",
    "KXWLDPERP": "float",
    "KXXLMPERP": "float",
    "KXXRPPERP": "float",
    "KXZECPERP": "float"
  },
  "taker_fee_rates": {
    "KXAAVEPERP": "float",
    "KXADAPERP": "float",
    "KXBCHPERP": "float",
    "KXBNBPERP": "float",
    "KXBTCPERP": "float",
    "KXDOGEPERP": "float",
    "KXDOTPERP": "float",
    "KXETHPERP": "float",
    "KXGOLDPERP": "float",
    "KXHBARPERP": "float",
    "KXHYPEPERP": "float",
    "KXKSHIBPERP": "float",
    "KXLINKPERP": "float",
    "KXLTCPERP": "float",
    "KXNEARPERP": "float",
    "KXSILVERPERP": "float",
    "KXSOLPERP": "float",
    "KXSUIPERP": "float",
    "KXVVVPERP": "float",
    "KXWLDPERP": "float",
    "K
  ... truncated
```

**sample** (ids redacted)

```json
{
  "maker_fee_rates": {
    "KXAAVEPERP": 0.0002,
    "KXADAPERP": 0.0002,
    "KXBCHPERP": 0.0002,
    "KXBNBPERP": 0.0002,
    "KXBTCPERP": 0.0002,
    "KXDOGEPERP": 0.0002,
    "KXDOTPERP": 0.0002,
    "KXETHPERP": 0.0002,
    "KXGOLDPERP": 1.2e-05,
    "KXHBARPERP": 0.0002,
    "KXHYPEPERP": 0.0002,
    "KXKSHIBPERP": 0.0002,
    "KXLINKPERP": 0.0002,
    "KXLTCPERP": 0.0002,
    "KXNEARPERP": 0.0002,
    "KXSILVERPERP": 1.2e-05,
    "KXSOLPERP": 0.0002,
    "KXSUIPERP": 0.0002,
    "KXVVVPERP": 0.0002,
    "KXWLDPERP": 0.0002,
    "KXXLMPERP": 0.0002,
    "KXXRPPERP": 0.0002,
    "KXZECPERP": 0.0002
  },
  "taker_fee_rates": {
    "KXAAVEPERP": 0.0012,
    "KXADAPERP": 0.0012,
    "KXBCHPERP": 0.0012,
    "KXBNBPERP": 0.0012,
    "KXBTCPERP": 0.0012,
    "KXDOGEPERP": 0.0012,
    "KXDOTPERP": 0.0012,
    "KXETHPERP": 0.0012,
    "KXGOLDPERP": 7.2e-05,
    "KXHBARPERP": 0.0012,
    "KXHYPEPERP": 0.0012,
    "KXKSHIBPERP": 0.0012,
    "KXLINKPERP": 0.0012,
    "KXLTCPERP": 0.0012,
    "KXNEARPERP": 0.0012,
    "KXSILVERPERP": 7.2e-05,
    "KXSOLPERP": 0.0012,
    "KXSUIPERP": 0.0012,
    "KXVVVPERP": 0.0012,
    "KXWLDPERP": 0.0012,
    "KXXLMPERP": 0.0012,
    "KXXRPPERP": 0.0
  ... truncated
```

### `/margin/markets/KXAAVEPERP`

- status **200**, 67ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "market": {
    "ask": "str(numeric)",
    "asset_class": "str",
    "bid": "str(numeric)",
    "contract_size": "str(numeric)",
    "exchange_index": "int",
    "fractional_trading_enabled": "bool",
    "leverage_estimate": "float",
    "leverage_estimates": {
      "1000": "float",
      "10000": "float",
      "100000": "float",
      "1000000": "float"
    },
    "liquidation_mark_price": {
      "price": "str(numeric)",
      "ts_ms": "int"
    },
    "long_leverage_estimates": {
      "1000": "float",
      "10000": "float",
      "100000": "float",
      "1000000": "float"
    },
    "open_interest": "str(numeric)",
    "open_interest_notional_value_dollars": "str(numeric)",
    "price": "str(numeric)",
    "reference_price": {
      "price": "str(numeric)",
      "ts_ms": "int"
    },
    "schedule": "null",
    "settlement_mark_price": {
      "price": "str(numeric)",
      "ts_ms": "int"
    },
    "short_leverage_estimates": {
      "1000": "float",
      "10000": "float",
      "100000": "float",
      "1000000": "float"
    },
    "status": "str",
    "tick_size": "str(numeric)",
    "ticker": "str",
    "title": "str",
    "volume": "str(numeric)",
    "volume_24h
  ... truncated
```

**sample** (ids redacted)

```json
{
  "market": {
    "ask": "1.2552",
    "asset_class": "Crypto",
    "bid": "1.2527",
    "contract_size": "0.010000",
    "exchange_index": 0,
    "fractional_trading_enabled": false,
    "leverage_estimate": 3.2512,
    "leverage_estimates": {
      "1000": 3.2389,
      "10000": 3.1918,
      "100000": 2.9792,
      "1000000": 2.2047
    },
    "liquidation_mark_price": {
      "price": "1.2543",
      "ts_ms": 1789134398733
    },
    "long_leverage_estimates": {
      "1000": 3.5532,
      "10000": 3.4967,
      "100000": 3.2431,
      "1000000": 2.346
    },
    "open_interest": "83948.00",
    "open_interest_notional_value_dollars": "105220.423200",
    "price": "1.2534",
    "reference_price": {
      "price": "1.2550",
      "ts_ms": 1789134398000
    },
    "schedule": null,
    "settlement_mark_price": {
      "price": "1.2534",
      "ts_ms": 1789134398674
    },
    "short_leverage_estimates": {
      "1000": 3.239,
      "10000": 3.192,
      "100000": 2.9793,
      "1000000": 2.2048
    },
    "status": "active",
    "tick_size": "0.0001",
    "ticker": "KXAAVEPERP",
    "title": "0.01 AAVE",
    "volume": "3472649.00",
    "volume_24h": "431437.00",
    "volume_24h
  ... truncated
```

### `/margin/markets/KXAAVEPERP/orderbook`

- status **200**, 65ms
- params: `{"depth": 10}`
- rate-limit headers: none returned

**schema**

```json
{
  "orderbook": {
    "asks": [
      [
        "str(numeric)"
      ]
    ],
    "bids": [
      [
        "str(numeric)"
      ]
    ]
  }
}
```

**sample** (ids redacted)

```json
{
  "orderbook": {
    "asks": [
      [
        "1.2585",
        "61328.00"
      ],
      [
        "1.2578",
        "71555.00"
      ]
    ],
    "bids": [
      [
        "1.2509",
        "52920.00"
      ],
      [
        "1.2511",
        "80.00"
      ]
    ]
  }
}
```

### `/margin/markets/KXAAVEPERP/candlesticks`

- status **200**, 80ms
- params: `{"start_ts": 1789130797, "end_ts": 1789134397, "period_interval": 1}`
- rate-limit headers: none returned

**schema**

```json
{
  "candlesticks": [
    {
      "ask": {
        "close": "str(numeric)",
        "high": "str(numeric)",
        "low": "str(numeric)",
        "open": "str(numeric)"
      },
      "bid": {
        "close": "str(numeric)",
        "high": "str(numeric)",
        "low": "str(numeric)",
        "open": "str(numeric)"
      },
      "end_period_ts": "int",
      "open_interest": "str(numeric)",
      "open_interest_notional_value_dollars": "str(numeric)",
      "price": {
        "close": "null",
        "high": "null",
        "low": "null",
        "mean": "null",
        "open": "null",
        "previous": "str(numeric)"
      },
      "volume": "str(numeric)",
      "volume_notional_value_dollars": "str(numeric)"
    }
  ],
  "ticker": "str"
}
```

**sample** (ids redacted)

```json
{
  "candlesticks": [
    {
      "ask": {
        "close": "1.2500",
        "high": "1.2514",
        "low": "1.2461",
        "open": "1.2461"
      },
      "bid": {
        "close": "1.2484",
        "high": "1.2493",
        "low": "1.2443",
        "open": "1.2446"
      },
      "end_period_ts": 1789130820,
      "open_interest": "79836.00",
      "open_interest_notional_value_dollars": "99276.066000",
      "price": {
        "close": null,
        "high": null,
        "low": null,
        "mean": null,
        "open": null,
        "previous": "1.2435"
      },
      "volume": "0.00",
      "volume_notional_value_dollars": "0.000000"
    },
    {
      "ask": {
        "close": "1.2525",
        "high": "1.2535",
        "low": "1.2479",
        "open": "1.2500"
      },
      "bid": {
        "close": "1.2507",
        "high": "1.2510",
        "low": "1.2449",
        "open": "1.2484"
      },
      "end_period_ts": 1789130880,
      "open_interest": "79836.00",
      "open_interest_notional_value_dollars": "99755.082000",
      "price": {
        "close": "1.2495",
        "high": "1.2495",
        "low": "1.2495",
        "mean": "1.2495",
        "open": "1.2495",
 
  ... truncated
```

### `/margin/trades`

- status **200**, 65ms
- params: `{"ticker": "KXAAVEPERP", "limit": 5}`
- rate-limit headers: none returned

**schema**

```json
{
  "cursor": "str",
  "trades": [
    {
      "count": "str(numeric)",
      "created_time": "str(iso8601)",
      "price": "str(numeric)",
      "taker_side": "str",
      "ticker": "str",
      "trade_id": "str"
    }
  ]
}
```

**sample** (ids redacted)

```json
{
  "cursor": "EhIKEAci0hRgCa2AFxstM7ZvrLIaDAi4h5DVBhD434_jAg",
  "trades": [
    {
      "count": "8.00",
      "created_time": "2026-09-11T13:46:26.037859Z",
      "price": "1.2534",
      "taker_side": "ask",
      "ticker": "KXAAVEPERP",
      "trade_id": "<redacted>"
    },
    {
      "count": "130.00",
      "created_time": "2026-09-11T13:42:54.720553Z",
      "price": "1.2509",
      "taker_side": "bid",
      "ticker": "KXAAVEPERP",
      "trade_id": "<redacted>"
    }
  ]
}
```

### `/margin/funding_rates/historical`

- status **200**, 89ms
- params: `{"start_ts": 1788529597, "end_ts": 1789134397}`
- rate-limit headers: none returned

**schema**

```json
{
  "funding_rates": [
    {
      "funding_rate": "float",
      "funding_time": "str(iso8601)",
      "mark_price": "str(numeric)",
      "market_ticker": "str"
    }
  ]
}
```

**sample** (ids redacted)

```json
{
  "funding_rates": [
    {
      "funding_rate": -0.0001166282614699,
      "funding_time": "2026-09-11T12:00:00Z",
      "mark_price": "1.2213",
      "market_ticker": "KXAAVEPERP"
    },
    {
      "funding_rate": -0.0001542504925559,
      "funding_time": "2026-09-11T12:00:00Z",
      "mark_price": "0.2022",
      "market_ticker": "KXADAPERP"
    }
  ]
}
```

### `/margin/funding_rates/estimate`

- status **200**, 62ms
- params: `{"ticker": "KXAAVEPERP"}`
- rate-limit headers: none returned

**schema**

```json
{
  "computed_time": "str(iso8601)",
  "funding_rate": "float",
  "mark_price": "str(numeric)",
  "market_ticker": "str",
  "next_funding_time": "str(iso8601)"
}
```

**sample** (ids redacted)

```json
{
  "computed_time": "2026-09-11T13:46:38.674371Z",
  "funding_rate": -0.0001131902210977,
  "mark_price": "1.2534",
  "market_ticker": "KXAAVEPERP",
  "next_funding_time": "2026-09-11T20:00:00Z"
}
```

### `/margin/balance`

- status **200**, 79ms
- params: `{"compute_available_balance": "true"}`
- rate-limit headers: none returned

**schema**

```json
{
  "settled_funds": "str(numeric)",
  "subaccount_balances": [
    {
      "account_equity": "str(numeric)",
      "available_balance": "str(numeric)",
      "initial_margin": "str(numeric)",
      "maintenance_margin": "str(numeric)",
      "position_value": "str(numeric)",
      "resting_orders_margin": "str(numeric)",
      "subaccount": "int"
    }
  ]
}
```

**sample** (ids redacted)

```json
{
  "settled_funds": "49.8649",
  "subaccount_balances": "<redacted>"
}
```

### `/margin/positions`

- status **200**, 72ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "positions": [
    {
      "entry_price": "str(numeric)",
      "fees": "str(numeric)",
      "is_portfolio": "bool",
      "margin_used": "str(numeric)",
      "market_ticker": "str",
      "position": "str(numeric)",
      "subaccount": "int",
      "unrealized_pnl": "str(numeric)"
    }
  ]
}
```

**sample** (ids redacted)

```json
{
  "positions": [
    {
      "entry_price": "0.0000",
      "fees": "0.0000",
      "is_portfolio": false,
      "margin_used": "0.0000",
      "market_ticker": "KXETHPERP",
      "position": "0.00",
      "subaccount": "<redacted>",
      "unrealized_pnl": "0.0000"
    },
    {
      "entry_price": "0.0000",
      "fees": "0.0000",
      "is_portfolio": false,
      "margin_used": "0.0000",
      "market_ticker": "KXBTCPERP",
      "position": "0.00",
      "subaccount": "<redacted>",
      "unrealized_pnl": "0.0000"
    }
  ]
}
```

### `/margin/risk`

- status **200**, 71ms
- params: `{}`
- rate-limit headers: none returned

**schema**

```json
{
  "positions": [],
  "total_maintenance_margin": "str(numeric)",
  "total_position_notional": "str(numeric)"
}
```

**sample** (ids redacted)

```json
{
  "positions": [],
  "total_maintenance_margin": "0.0000",
  "total_position_notional": "0.0000"
}
```

