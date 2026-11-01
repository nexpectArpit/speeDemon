# Track C: Join

The task, the input files, and the exact output format are specified in the [Track C specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).

## Approach

Our solution is implemented in standalone standard C++20 (compiled with GCC 15 and executed on Debian 13 slim), optimized for the Falkenstein dedicated hardware (6 P-cores, 12 hardware threads, 8 GB RAM) and hardened against all adversarial CSV edge cases:

1. **Direct Memory-Mapped Inputs & Dynamic Region Array Lookup**:
   * Both `regions.csv` and `records.csv` are mapped read-only via `mmap`, avoiding file I/O overhead and duplicate buffer memory.
   * Region mappings (`region_id` $\to$ `country`) are stored in a contiguous flat array `std::vector<uint16_t> region_to_country` initialized to 1,000,000 entries and dynamically resized if larger region IDs are encountered. This gives true $O(1)$ lookup with zero hashing overhead or branch mispredictions.
   * Regions not in the lookup table (or $> 10^6$) automatically map to `_UNMATCHED`.

2. **Invariant-Preserving Parallel Record Boundary Splitting**:
   * Because `records.csv` contains quoted fields with embedded newlines (`\n` and `\r\n`), naive newline splitting is invalid.
   * However, the generator and grammar guarantee that every record terminates with the `amount` field, matching `\.[0-9]{2}(\r?\n|\z)`. Notes are restricted to lowercase words and single spaces/LFs without digits or decimal points.
   * We divide the file into chunk boundaries by searching for `\n` preceded by `.[0-9]{2}` and followed by an optional BOM (`\xef\xbb\xbf`) and integer `id`.
   * For small inputs ($< 64$ KB) or edge-case test vectors, execution gracefully falls back to single-threaded processing.

3. **In-Place Streaming CSV Parser with Zero Heap Allocations**:
   * Field parsing processes `id`, `name`, `note`, `region_id`, and `amount` with explicit bounds checking (`cursor < end`) and zero string allocations.
   * Quotes and escaped quotes (`""`) are unescaped into a thread-local reusable buffer without allocating new `std::string` objects.
   * Mid-file BOM markers at record boundaries are detected and skipped; BOM bytes embedded inside quoted strings are preserved as literal data per the specification.
   * Exact money arithmetic is maintained in 64-bit integer cents (supporting negative values down to $-0.01$ and $0.00$), preventing floating-point inaccuracies.

4. **Index-Based Hash Interning & Bitset Cardinality Reduction**:
   * Each thread maintains an index-based open-addressing hash table (FNV-1a, 8-byte entries) mapping distinct names to thread-local IDs. Because entries reference names by vector indices rather than raw pointers, table stability is 100% memory-safe under arbitrary vector resizing.
   * Each thread marks `(country_id, local_name_id)` in thread-local dynamic bitsets (`std::vector<uint64_t>`).
   * Discovered names across all shards are mapped into a unified global dictionary.
   * Global distinct name counting per country is computed in parallel across countries: bitwise OR across shard bitsets and `__builtin_popcountll` gives the exact distinct name count in microseconds.

5. **Canonical Sorting & Direct Formatting**:
   * Active countries (those with count $> 0$) are sorted bytewise ascending. `_UNMATCHED` naturally sorts after all 3-letter country codes (`_` is ASCII 0x5F, `Z` is 0x5A).
   * Aggregates are serialized directly into a pre-reserved buffer using custom fast integer and cents formatting routines, then written in a single `fwrite` to `/out/aggregates.txt`.

## Dependencies

None. Standard C++20 and POSIX system APIs only. Runtime dependencies count = 0 ($D = 1.0$).

## With another 24 hours

1. **AVX2 / SWAR Parsing of Amount and Delimiters**: Use SIMD vector instructions to scan for commas, quotes, and newlines in 32-byte chunks.
2. **Compile-Time Two-Stage Profile-Guided Optimization (PGO)**: Warm branch predictors and instruction layout using synthetic generator workloads.
3. **Huge Pages & Pre-Fault Optimization**: Advise kernel on memory access patterns using `madvise(MADV_WILLNEED | MADV_HUGEPAGE)`.
