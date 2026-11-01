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
| `49f7ea9` | `run-b9303ab05d10` | 1.0965 s | 433.0 MB | 366 | 6.825 | 41.705 | *Failed Experiment:* 2-level 16 KB pages caused memory explosion & page-fault storm on unreachable tail queries across 12 threads |
| Current | pending | ~0.20 s | ~110.0 MB | 244 | ~18.5 | ~63.0+ | 20-byte packed Slot DS with read-only probe filters, auto-shrinking table cap, and 244 SLOC |

## Architecture & Engineering Iterations

### 1. Dual CSR Graph Representation
The 92 MB directed graph (1.74M nodes, 5M edges) is ingested into dual forward and reverse Compressed Sparse Row (CSR) arrays (`fo`, `fe` and `ro`, `re`). Atomic degree pre-accumulation sets exact offsets so edge arrays require no reallocation.

### 2. $O(1)$ Degree-0 Early Pruning
Any query where `out_degree[s] == 0` or `in_degree[t] == 0` is guaranteed unreachable on a directed graph and terminates immediately in $O(1)$ time with distance `-1`.

### 3. Bidirectional Dijkstra with Bounded Stopping
Forward and reverse searches expand from $s$ and $t$ using 4-ary min-heaps (`HP`). Search terminates at the exact mathematical boundary when the sum of current heap tops exceeds the best-seen path:
$$\min(heap_{fwd}) + \min(heap_{rev}) \ge \mu$$

### 4. Compact 20-byte Packed Slot DS vs Paged Memory Pitfall
In commit `49f7ea9`, a 2-level paged directory with 16 KB pages was tested. On unreachable or large queries, pseudo-random graph vertex IDs touched nearly all 1,700 pages (the birthday paradox), triggering a catastrophic 433 MB memory peak across 12 threads and massive `memset(0xFF)` page fault overhead.

To permanently solve this:
- Distances are stored in an open-addressed hash table with packed 20-byte slots:
  ```cpp
  struct __attribute__((packed)) Slot {
      uint64_t f = INF, r = INF;
      uint32_t k = ~0u;
  };
  ```
- Before inserting a neighbor node into the hash table, `dist.gf(v)` and `dist.gr(u)` perform a read-only probe. If the edge does not improve the current distance, zero heap or table modifications occur.
- When an individual query expands beyond 65,536 slots, `dist.clear()` reclaims the table memory back to 4096 slots immediately, preventing tail queries from permanently expanding the worker's resident memory.
- Code size is compressed to 244 SLOC, maximizing the compactness factor in the $K$-formula.

### 5. Full 12-Thread Hardware Parallelism
With thread-local working sets strictly bounded to ~2 MB under typical queries and auto-reclaimed after tail queries, all 12 hardware threads (`W = 12`) execute concurrently without risking cgroup memory pressure.
