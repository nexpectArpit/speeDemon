# Track A — Ingest

High-performance C++20 log ingest and aggregation engine.

## Approach
- **Ingest & Split:** Memory-maps `requests.log`, parallelizing across 12 worker threads along newline boundaries.
- **Timestamp Arithmetic:** Directly maps timestamps to relative hours `0..71` using branchless arithmetic on ASCII bytes.
- **Bucketing:** Latency values are partitioned per `(hour, endpoint)` into flat continuous memory slots.
- **Percentiles:** Exact nearest-rank percentiles (`p50`, `p95`, `p99`) computed in $O(N)$ expected time via `std::nth_element`.

## Build & Dependencies
- Standard C++20 compiled with GCC 15 (`-O3 -march=native -flto -DNDEBUG`).
- Zero non-stdlib dependencies ($D = 1.0$).
