# tools/

`orderbook_itch_replay` replays real NASDAQ order flow through `OrderBook`
(v1) and `FastOrderBook` (v2), as a real-data complement to the synthetic
flow in `bench/`. See the header comment in
[itch_replay_main.cpp](itch_replay_main.cpp) for exactly how each ITCH
message type maps to an engine operation, and
[docs/HISTORY.md](../docs/HISTORY.md) for why this exists.

Not built by default (`ORDERBOOK_BUILD_TOOLS` is `OFF`); it needs sample
data this repo doesn't ship.

## Getting sample data

NASDAQ publishes full trading-day ITCH 5.0 files for free, for example:

```
curl -o tools/itch_data/sample.gz \
  'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz'
```

Each full day is several GB gzipped; a `curl -r 0-<bytes>` range request
on the same URL grabs a partial prefix instead, which is enough for a
representative single-symbol replay (that's what the numbers in the
README and `docs/HISTORY.md` are based on). Then decompress:

```
gunzip -k tools/itch_data/sample.gz
```

`tools/itch_data/` is gitignored; nothing under it is checked in.

## Building and running

```
cmake --preset tools
cmake --build --preset tools
./build/tools/tools/orderbook_itch_replay tools/itch_data/sample.bin [SYMBOL]
```

`SYMBOL` defaults to `AAPL`. The tool scans the file's Stock Directory
records to resolve the symbol to NASDAQ's internal locate code, replays
every message for that locate through both engines, and prints
throughput for each.

A fixed price band (see `kPriceBandMin`/`kPriceBandMax` in
`itch_replay_main.cpp`) filters out the small fraction of real orders
placed at deliberately extreme marketable-limit prices; the tool reports
how many ops it skipped for that reason on every run.
