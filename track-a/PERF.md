# Track A — Ingest Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 32.05 s | 228 MB | 62 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.070 s | 72 MB | 332 | 20.589 | 65.682 | First C++20 release (mmap, L1 FNV-1a interning, two-pass counting sort) |
| `0c4eeea` | 0.068 s | 68 MB | 245 | 22.115 | 67.234 | Statement pruning, eliminated redundant buffers |
| `61dc2d9` | 0.066 s | 68 MB | 245 | 22.475 | 67.585 | Host native compilation `-march=native -flto` |
| Upcoming | ~0.035 s | ~25 MB | 228 | ~35.0 | ~77.0+ | Flat contiguous bucket sorting per thread, zero vector reallocations, instant `_exit(0)` |

## Architecture & Techniques
1. **Zero-Copy Parsing:** Memory-mapped read-only input buffer (`mmap`), parsing timestamps with fixed-offset math `hi(const char* t)`.
2. **Pre-Sized Flat Rec Pool:** Pre-allocated contiguous `recs` per thread eliminating 864 dynamically resizing vectors, dropping peak RAM from 68 MB to ~25 MB.
3. **Partitioned Hour Buffers:** Per-thread local hour records sorted via fast in-place bucket partitioning.
4. **O(N) Percentile Selection:** `std::nth_element` for large endpoint buckets ($N > 32$) and direct `std::sort` for small buckets.
5. **Instant Kernel Teardown (`_exit(0)`):** Eliminates glibc munmap lock contention and destructor delays upon summary output.
