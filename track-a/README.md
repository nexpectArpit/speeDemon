# Track A: Ingest

The task, the input files, and the exact output format are specified in the [Track A specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/a-ingest.md).

## Approach

Our solution is implemented in standalone C++20 (compiled with GCC 15 and executed on Debian 13 slim), optimized for the Falkenstein dedicated hardware (6 P-cores, 12 hardware threads, 8 GB RAM):

1. **Memory-Mapped Input & Direct Calendar Offsets**:
   * We map `/data/requests.log` read-only via `mmap`. The competition harness prewarms the input in the page cache, so no redundant allocations or intermediate line copies are made.
   * Timestamps strictly cover the 72-hour period from `2026-10-30T18:00:00Z` to `2026-11-02T17:59:59Z`. We extract the month, day, and hour directly from fixed byte offsets to compute a 0-indexed hour integer $[0, 71]$ with branchless arithmetic, avoiding general-purpose calendar/datetime parsing.

2. **Parallel Chunking with Open-Addressing Interning**:
   * Input is divided across worker threads at newline boundaries (`\n`).
   * Each worker parses its slice into thread-local pre-reserved record buffers bucketed by hour.
   * Endpoint URLs (1 to 125 bytes) are interned on the fly using a fast open-addressing hash table with FNV-1a hashing and linear probing (load factor $\le 25\%$), ensuring almost every lookup hits on the first probe in L1 cache.
   * Status codes are checked by checking if `status[0] == '5'`. Latencies are parsed via simple base-10 accumulation.

3. **Global Endpoint Consolidation & Canonical Ordering**:
   * Discovered endpoints across all worker dictionaries are unified and sorted alphabetically once.
   * Each worker builds a direct 16-bit lookup mapping its local endpoint IDs to global sorted canonical IDs.

4. **Independent Parallel Hour Processing & Linear Percentile Selection**:
   * The 72 hours are processed in parallel across workers via atomic work-stealing (`hour_counter.fetch_add(1)`).
   * For each hour, records from all workers are gathered using a two-pass counting sort (histogram + prefix offset scatter) to produce contiguous latency slices for each canonical endpoint in sorted order.
   * Percentiles (nearest-rank p50, p95, p99) are computed in exact integer arithmetic:
     * For small groups ($N \le 32$, which represent over 80% of groups), elements are sorted directly with `std::sort`.
     * For large groups ($N > 32$), we apply `std::nth_element` in linear $O(N)$ time, avoiding unnecessary sorting of non-percentile elements.
   * Error ppm is computed using 64-bit integer floor: `(errors * 1000000ULL) / count`.

5. **Direct Zero-Copy Output Formatting**:
   * Each hour worker formats its lines directly into an independent output buffer using fast integer-to-string routines without `std::string` reallocations.
   * Output buffers are written sequentially to `/out/summary.txt`, ensuring guaranteed total order (hour ascending, then endpoint unsigned byte ascending).

## Dependencies

None. Standard C++20 and POSIX system APIs only. Runtime dependencies count = 0 ($D = 1.0$).

## With another 24 hours

1. **AVX2 SIMD Line & Delimiter Scanning**: Accelerate newline and space scanning using AVX2 vector instructions (`_mm256_cmpeq_epi8` / `_mm256_movemask_epi8`).
2. **Profile-Guided Optimization (PGO)**: Train two-stage offline GCC profile generation on synthetic generator seeds at build time.
3. **Core Pinning / NUMA Affinity**: Pin thread workers directly to hardware threads 0–11.
