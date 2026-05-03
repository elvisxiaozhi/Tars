# Step 2.28 - Multi-Crypto Parallel Strategy

## Scope

- Extend the hourly crypto strategy from BTC-only to configurable multi-coin monitoring.
- Add BTC/ETH/SOL/XRP/DOGE/BNB/HYPE default dry-run monitoring with per-coin thresholds.
- Keep live mode on the same execution path, guarded by per-coin `live_enabled`.
- Update strategy documentation, logs, trade journal fields, and dashboard/API compatibility.

## Implementation

- `BinanceFeed` now supports `fetch(coin, binance_symbol)` for BTC/ETH/SOL and future symbols.
- `BinanceFeed` caches 1h klines for 60 seconds while still refreshing spot prices each tick.
- `MarketFeed` can query multiple hourly gamma slugs in one loop and keep only current-hour markets.
- `MarketFeed` uses a shorter scan timeout and a 60-second per-slug gamma retry cooldown after failures.
- The main loop skips order-book refreshes outside the entry window unless a market has an open position.
- Open positions outside the entry window refresh only the held token's order book.
- `Strategy` now keeps per-coin slope state and supports:
  - `TREND`
  - `REVERSAL`
  - `QUIET_REVERSION`
- `RiskManager` now enforces:
  - one entry per coin per candle
  - max global entries per hour
  - max global open positions
  - max quiet-reversion entries per hour
- `TradeRecord` and `Position` now carry `coin` and `regime`.
- Dashboard `/api/status` exposes `coins[]` and `markets[]`; the HTML renders a multi-market table.

## Defaults

- `strategy.crypto_symbols`: `["BTC", "ETH", "SOL", "XRP", "DOGE", "BNB", "HYPE"]` in `config.example.json`.
- LIVE default:
  - BTC enabled
  - ETH/SOL/XRP/DOGE/BNB/HYPE disabled until dry-run data confirms fill/exit quality

## Verification

- `rtk cmake --build build` passed.
- `rtk proxy git diff --check` passed.
- `rtk ctest --test-dir build --output-on-failure` ran, but this build directory has no registered CTest tests.
