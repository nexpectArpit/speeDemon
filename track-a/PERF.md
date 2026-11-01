# Track A — Ingest Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 32.05 s | 228 MB | 62 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.070 s | 72 MB | 332 | 20.589 | 65.682 | First C++20 release (mmap, L1 FNV-1a interning, two-pass counting sort) |
| `0c4eeea` | 0.068 s | 68 MB | 245 | 22.115 | 67.234 | Statement pruning, eliminated redundant buffers |
| Current | ~0.048 s | ~25 MB | ~200 | ~25.5 | ~70.0+ | Enabled `-march=native -flto`, two-pass flat CSR latency accumulation |

## Architecture & Techniques
1. **Zero-Copy Parsing:** Memory-mapped read-only input buffer (`mmap`), parsing timestamps with fixed-offset math `hi(const char* t)`.
2. **L1 Cache FNV-1a Interning:** Fixed 8192-entry power-of-two direct-mapped hash table resolving endpoints without heap allocations.
3. **Partitioned Hour Buffers:** Per-thread local hour records merged into contiguous endpoint buckets for cache-friendly percentile calculation.
4. **O(N) Percentile Selection:** `std::nth_element` for large endpoint buckets ($N > 32$) and direct `std::sort` for small buckets.
