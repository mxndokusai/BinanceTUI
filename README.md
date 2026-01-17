# BinanceTUI

Real-time monitoring system for Binance crypto markets with dual-view TUI support for both spread tracking and orderbook visualisation.

This was done to play around with websockets using Boost.Beast and TUI programming using FTXUI, combines spread monitoring via bookTicker streams and orderbook depth via depth streams.

## Build

```bash
cmake -B build -S .
cmake --build build
./build/main config.yaml
```

Sample `config.yaml`

```yaml
refresh_rate_ms: 100
display_tui: true
view_mode: spreads # spreads | orderbook
spreads:
  - name: BTC_Basis
    leg1:
      market: SPOT
      symbol: BTCUSDT
      beta: 1.0
    leg2:
      market: UM_FUTURES
      symbol: BTCUSDT
      beta: -1.0
orderbooks:
  - market: SPOT
    symbol: BTCUSDT
  - market: SPOT
    symbol: ETHUSDT
```

## Details

WebSocket Stream

- bookTicker: individual best bid/ask updates.
- depth: full orderbook depth snapshots.

Threading

- One I/O thread per market type.
- Lock-free atomic operations for price updates.
- Mutex-protected orderbook modifications.

Orderbook Synchronisation [(Binance documentation)](https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams#how-to-manage-a-local-order-book-correctly)

1. Open WebSocket connection to `wss://stream.binance.com:9443/ws/<symbol>@depth`
2. Buffer incoming messages while preparing snapshot
3. Fetch snapshot via `GET /api/v3/depth?symbol=<SYMBOL>&limit=5000`
4. Validate snapshot timing: If snapshot's `lastUpdateId` < first buffered event's `U`, refetch
5. Discard stale events: Remove buffered events where `u <= lastUpdateId`
6. Verify continuity: First valid event must have `U <= lastUpdateId + 1 <= u`
7. Apply updates: Process all subsequent events

For each depth update event:

- Ignore old: If event's `u <= localLastUpdateId`, skip
- Detect gaps: If event's `U > localLastUpdateId + 1`, flag error
- Update prices:
  - If quantity = "0": remove price level
  - Otherwise: insert/update price level
- Advance state: Set `localLastUpdateId = event.u`
