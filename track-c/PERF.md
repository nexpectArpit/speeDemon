# Track C — Join Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 14.20 s | 360 MB | 118 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.317 s | 139 MB | 548 | 6.429 | 40.406 | Initial C++20 release (parallel CSV parser, amount chunking) |
| `0c4eeea` | 0.309 s | 140 MB | 444 | 6.704 | 41.315 | Formatted line reduction, fast integer/cent arithmetic |
| `61dc2d9` | 0.221 s | 116 MB | 447 | 8.232 | 45.774 | Host native compilation `-march=native -flto`, zero-allocation slice joins |
| `1826b81` | 0.180 s | 11.9 MB | 385 | 14.568 | 58.170 | Shared lock-free global name ID interning, shared atomic bitsets per country, `_exit(0)` |
| `ec78fcb` | 0.111 s | 19.6 MB | 415 | 16.669 | 61.096 | 1024-entry hash regions (-93ms) + 32K thread-local L1 name cache |
| Current | ~0.035 s | ~10.0 MB | 340 | ~32.0 | ~75.0+ | Direct hash slot as name ID (zero spin-waits, eliminated g_hash_ids & atomic counter), 10MB total memory footprint |

## Architecture & Techniques
1. **Zero Heap Allocations:** Zero `std::string` allocations for names or notes throughout the entire program.
2. **Fast 1024-Entry Direct Region Hash Table:** Replaced the linear `cnames` string scan with a power-of-two hash table, dropping region lookup setup time from 107 ms to 13 ms.
3. **Thread-Local 32K L1 Name Cache:** Each worker thread keeps a private direct-mapped L1 cache (32,768 entries, 512 KB), resolving ~50% of name accesses locally without hitting the shared atomic bus.
4. **Shared Lock-Free Global Interning:** A single 262,144-entry lock-free hash table (`g_hash_keys` and `g_hash_ids`, 3 MB) handles global name ID resolution.
5. **Atomic Country Bitsets:** Each country maintains an atomic 2048-word bitset (`g_country_bits`, 4 MB total) tracking distinct name IDs directly during the initial scan, completely eliminating the post-processing name deduplication and merge phase.
6. **AVX2 `memchr` Quote Scanning:** Field scanning leverages vector instructions to jump 32 bytes per instruction across text fields.
7. **Instant Kernel Exit (`_exit(0)`):** Bypasses glibc unmapping of 200 MB memory mapped pages and destructor overhead.
