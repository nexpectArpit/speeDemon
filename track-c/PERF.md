# Track C — Join Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 14.20 s | 360 MB | 118 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.317 s | 139 MB | 548 | 6.429 | 40.406 | Initial C++20 release (parallel CSV parser, amount chunking) |
| `0c4eeea` | 0.309 s | 140 MB | 444 | 6.704 | 41.315 | Formatted line reduction, fast integer/cent arithmetic |
| `61dc2d9` | 0.221 s | 116 MB | 447 | 8.232 | 45.774 | Host native compilation `-march=native -flto`, zero-allocation slice joins (+4.46 pts) |
| Upcoming | ~0.025 s | ~12 MB | 297 | ~38.0 | ~79.0+ | Shared lock-free global name ID interning, shared atomic bitsets per country, AVX2 `memchr` quote scanning, `_exit(0)` |

## Architecture & Techniques
1. **Zero Heap Allocations:** Zero `std::string` allocations for names or notes throughout the entire program.
2. **Shared Lock-Free Name Interning:** A single 262,144-entry lock-free hash table (`g_hash_keys` and `g_hash_ids`, 3 MB) gives all 12 threads identical global `name_id`s with zero inter-thread synchronization overhead.
3. **Atomic Country Bitsets:** Each country maintains an atomic 2048-word bitset (`g_country_bits`, 4 MB total) tracking distinct name IDs directly during the initial scan, completely eliminating the post-processing name deduplication and merge phase.
4. **AVX2 `memchr` Quote Scanning:** Field scanning leverages vector instructions to jump 32 bytes per instruction across text fields.
5. **Instant Kernel Exit (`_exit(0)`):** Bypasses glibc unmapping of 200 MB memory mapped pages and destructor overhead.
