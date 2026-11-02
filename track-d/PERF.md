# Track D — Traverse Performance Log

## Run History & Progression

| Commit | Run ID | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|---|
| `ef69484` | reference | 47.00 s | 656.0 MB | 55 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | `run-291b8fa` | 1.662 s | 529.0 MB | 405 | 5.245 | 35.986 | Initial C++20 release (dual forward/reverse CSR, degree-0 pruning) |
| `0c4eeea` | `run-f19da4c` | 1.800 s | 531.0 MB | 313 | 5.235 | 35.945 | Compact binary heap, pruned unused buffers |
| `61dc2d9` | `run-7a3e91b` | 1.434 s | 528.0 MB | 313 | 5.871 | 38.437 | Host native compilation `-march=native -flto` (+2.45 pts) |
| `1826b81` | `run-1b8ef3a` | 1.651 s | 531.0 MB | 313 | 5.468 | 36.890 | Instant `_exit(0)` kernel teardown |
| `e59bfbf` | `run-d18ea21` | 0.7392 s | 268.1 MB | 319 | 9.339 | 48.515 | 2-pass atomic CSR without 110 MB `lre` buffer |
| `6abed99` | `run-ca7c0b3` | 0.7102 s | 269.0 MB | 319 | 9.522 | 48.936 | Baseline evaluation |
| `c4367b0` | `run-bf8dcf6fac61` | 0.6393 s | 166.6 MB | 352 | 10.883 | 51.838 | Dynamic sparse distance table (`DS`), memory slashed by 110 MB |
| `49f7ea9` | `run-b9303ab05d10` | 1.0965 s | 433.0 MB | 366 | 6.825 | 41.705 | *Failed Experiment:* 2-level 16 KB pages caused memory explosion & page-fault storm on unreachable tail queries |
| `6aa9135` | `run-2b740ef` | 0.4416 s | 201.9 MB | 331 | 12.598 | 55.014 | 20-byte packed Slot DS, read-only probe filters |
| `d0f4164` | `run-5b871c9` | 0.4377 s | 201.0 MB | 331 | 12.665 | 55.130 | 🚀 **ALL-TIME BEST:** Traversal accelerated to 0.438s |
| Current | pending | ~0.35 s | ~180.0 MB | 240 | ~14.5 | ~58.0+ | Standard C++20 `std::atomic_ref` synchronization, guaranteed vector capacity release via swap, 240 SLOC |

## Architecture & Engineering Iterations

### 1. Dual CSR Graph Representation with Standard C++20 `std::atomic_ref`
The 92 MB directed graph (1.74M nodes, 5M edges) is ingested into dual forward and reverse Compressed Sparse Row (CSR) arrays (`fo`, `fe` and `ro`, `re`).
Degrees and write offsets are updated using standard C++20 `std::atomic_ref<uint32_t>`, completely eliminating non-standard `reinterpret_cast` operations while maintaining atomic memory operations:
```cpp
std::atomic_ref<uint32_t>(fo[u]).fetch_add(1, std::memory_order_relaxed);
```

### 2. $O(1)$ Degree-0 Early Pruning
Any query where `out_degree[s] == 0` or `in_degree[t] == 0` is guaranteed unreachable on a directed graph and terminates immediately in $O(1)$ time with distance `-1`.

### 3. Bidirectional Dijkstra with Bounded Stopping
Forward and reverse searches expand from $s$ and $t$ using 4-ary min-heaps (`HP`). Search terminates at the exact mathematical boundary when the sum of current heap tops exceeds the best-seen path:
$$\min(heap_{fwd}) + \min(heap_{rev}) \ge \mu$$

### 4. Compact 20-byte Packed Slot DS with Guaranteed Memory Reclamation
Distances are stored in an open-addressed hash table with packed 20-byte slots (`Slot { uint64_t f, r; uint32_t k; }`).
- Before inserting neighbor nodes, read-only probes `dist.gf(v)` and `dist.gr(u)` filter out non-improving edges.
- When an unusually large query expands `tb` beyond 65,536 slots, `DS::clear()` swaps `tb` with a freshly sized 4096-entry vector (`std::vector<Slot>(4096).swap(tb)`), guaranteeing that the high-water capacity is returned to the OS.
- Source lines are compressed to 240 SLOC.

## Verified local queue/resource revision — 2026-11-02

Prior pending projections are not evidence. The feed generated09:44:18Z measures fdbf51b at0.433167s /202,014,720bytes /S329 /55.447198points (run-bc0a754325a6).

This revision retains exact bidirectional Dijkstra and replaces the4-ary queues with64-bit monotone radix queues. Each direction has its own monotone last key; zero edges and stale keys remain supported. The correct graph mmap base is now unmapped, and a large state reset releases both table and touched-list ownership (allocator return to OS remains implementation-specific).

Bounded independent verification:3731 oracle queries +120 unseen-generator queries,133492 radix multiset operations across20 rounds, reset/reuse and successful munmap probes. Full-public paired native checks all match published SHA. Excluding one explicit warmup per binary, native ARM outer-process middle-three means are0.443881986s baseline and0.378738514s candidate (14.68% lower); baseline spread fails, candidate passes. This is a local trend only, not accepted scoring-box T or a new board score. Evidence AUDIT/rank78-20261102; source SHA efd446e7d22e295ad215447c1ad4e37b005977f16093410eb3eec338b68f4a7f.
