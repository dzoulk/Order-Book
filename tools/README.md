# tools/

Optional LOBSTER sample-data loader, not yet built. The synthetic
order-flow generator ended up living directly in `bench/bench_main.cpp`
instead of here, since it doesn't need to exist as a separate tool when
nothing else consumes its output yet. See [docs/DESIGN.md](../docs/DESIGN.md).
