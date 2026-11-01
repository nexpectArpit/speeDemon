# Track C — Join Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 14.20 s | 360 MB | 118 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.317 s | 139 MB | 548 | 6.429 | 40.406 | Initial C++20 release (parallel CSV parser, amount chunking) |
| `0c4eeea` | 0.309 s | 140 MB | 444 | 6.704 | 41.315 | Formatted line reduction, fast integer/cent arithmetic |
| Current | ~0.08 s | ~25 MB | ~390 | ~18.5 | ~63.0+ | Zero string allocations, 1024-entry country hash, 1-to-1 string slice hashing, `-march=native -flto` |

## Architecture & Techniques
1. **Zero String Allocations:** Names are stored as zero-copy slices pointing directly into the memory-mapped `records.csv` file.
2. **1-to-1 Bijection Property:** Because CSV quoting in the generator is 1-to-1 with unescaped names, distinct names are deduplicated using raw slices, and unescaped lengths are computed simply by subtracting escape occurrences without memory copying.
3. **1024-Entry Direct Country Hash Table:** `regions.csv` is parsed in ~33 ms by replacing linear scans with a power-of-two hash table.
4. **Partitioned Bitset Name Deduplication:** Bitset arrays merge thread-local name observations into global distinct counts per country using hardware `POPCNT`.
