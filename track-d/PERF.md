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

## Combined scorer candidate — 2026-11-02

Store radix queue items in12 bytes instead of a16-byte pair. Prune relaxations whose tentative distance cannot improve the known path upper bound, and reuse one distance-table lookup for comparison and update. Both directions retain the same shortest-path termination rule and exact64-bit distances. These are code changes, not measured performance gains. Local builds, tests and benchmarks were explicitly skipped at the user's request; evaluation is pending.

## Worker-owned degree counting candidate — 2026-11-02

Replace shared atomic degree increments with per-worker byte counters, reduce them into the global forward/reverse degrees, and release counter ownership before CSR edge allocation. Record every byte-counter wrap and add256 during reduction, preserving high-degree vertices exactly. At n=1,740,512 and12 workers the counter payload is41,772,288bytes, excluding the construction prototype and overflow metadata. CSR scatter still uses atomic cursors. This exchanges temporary memory and a reduction pass for removing two shared atomic updates per input edge. Allocator return to the OS and a score gain are not guaranteed. No local build, test or benchmark was run; scorer results are pending.

## Scorer rejection of55a8300 — 2026-11-02

User-supplied result: 55.173points versus 56.437 on bf0d8c4, delta -1.264. Restore the bf0d8c4 implementation while retaining Track A's accepted ordered-output change. Acceptance uses scorer K/points with correctness gates; no new local tests or benchmarks were run. These rounded observations do not isolate every cause or estimate run variance.

## Compact CSR edge candidate — 2026-11-02

Store common edges in six bytes (uint32 destination,uint16 weight). Reserve65535 as an exception marker and retain every original weight>=65535 in per-direction sorted edge-index/uint32-weight tables, including65535 itself. Query relaxations use the exact exception value when marked, retaining64-bit distances. Worker-owned exception lists merge after construction. At five million edges per direction the edge-array payload falls from80MB to60MB before exceptions; this is not a measured20MB peak-RSS reduction. Many exceptions or packed-access overhead can erase the gain. Prior degree-counter experiment remains reverted. No local build/test/benchmark was run, as requested; scorer correctness/T/M/S/K are pending. Baseline supplied bf0d8c4 D result:56.437points.

## Scorer rejection of40ed9a5 — 2026-11-02

User-supplied screenshot: 56.302points versus 57.248 on e6669a4, delta -0.946. Restore the e6669a4 source. A/B code was unchanged between these commits yet its measured times varied, so this observation is not a variance-controlled attribution. B adaptive block storage and A ordered output are retained. No local checks were run.

## Decimal parsing and queue retention candidate — 2026-11-02

On SSE2 x86 builds, classify up to eight bytes for long decimal fields and reduce their digit pairs/groups with integer arithmetic; continue scalar parsing for additional digits and bounded tails. Other targets retain scalar parsing. Keep full uint32 edge values and64-bit distances. At query clear, retain at most2MiB of vector capacity per radix frontier and release excess bucket ownership; active searches can still exceed that amount and allocator RSS release is implementation-dependent. Shared CSR construction atomics and distance-table reset policy remain. This targets loading instruction cost and cumulative queue high-water retention. No local build/test/benchmark was run; score impact is pending.

## Restore prior measured best source — 2026-11-02

The D implementation on65015f5 scored56.585170 points (366.292ms,212,185,088bytes,S380). Restored `main.cpp` fromfe691810, previously scored57.336883 points (358.589ms,201,469,952bytes,S347). The previous version is faster, smaller and shorter in that recorded run; its approximate log-loss versus65015f5 is `(56.585170-57.336883)*ln(10)/50=-0.0346`. Timing variation is material, so the board must confirm the restore. No local test was run.
