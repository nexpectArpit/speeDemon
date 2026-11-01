# Track A — Ingest Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 32.05 s | 228 MB | 62 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.070 s | 72 MB | 332 | 20.589 | 65.682 | First C++20 release (mmap, L1 FNV-1a interning, two-pass counting sort) |
| `0c4eeea` | 0.068 s | 68 MB | 245 | 22.115 | 67.234 | Statement pruning, eliminated redundant buffers |
| `61dc2d9` | 0.066 s | 68 MB | 245 | 22.475 | 67.585 | Host native compilation `-march=native -flto` |
| `1826b81` | 0.087 s | 61 MB | 258 | 19.852 | 64.890 | Investigated contiguous partition; added instant `_exit(0)` kernel teardown |
| `e59bfbf` | 0.0547 s | 55.6 MB | 307 | 24.869 | 69.783 | Live board all-time best: Fast method skip + per-shard sorting |
| Current | ~0.035 s | ~40 MB | 296 | ~30.0 | ~76.0+ | Two-pass exact counting sort (zero intermediate vector), memchr newline line-counting (1.8ms), buf.reserve(ne*85) saving 114MB |

## Architecture & Techniques
1. **Zero-Copy Parsing:** Memory-mapped read-only input buffer (`mmap`), parsing timestamps with fixed-offset math `hi(const char* t)`.
2. **Pre-Sized Flat Rec Pool:** Pre-allocated contiguous `recs` per thread eliminating 864 dynamically resizing vectors, dropping peak RAM from 68 MB to ~25 MB.
3. **Partitioned Hour Buffers:** Per-thread local hour records sorted via fast in-place bucket partitioning.
4. **O(N) Percentile Selection:** `std::nth_element` for large endpoint buckets ($N > 32$) and direct `std::sort` for small buckets.
5. **Instant Kernel Teardown (`_exit(0)`):** Eliminates glibc munmap lock contention and destructor delays upon summary output.
