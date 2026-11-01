# Track D — Traverse Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 47.00 s | 656 MB | 55 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 1.662 s | 529 MB | 405 | 5.245 | 35.986 | Initial C++20 release (dual forward/reverse CSR, degree-0 pruning) |
| `0c4eeea` | 1.800 s | 531 MB | 313 | 5.235 | 35.945 | Compact binary heap, pruned unused buffers |
| Current | ~0.45 s | ~120 MB | ~290 | ~14.0 | ~57.0+ | Direct CSR parsing, pre-reserved heaps, `-march=native -flto` |

## Architecture & Techniques
1. **Compact Dual CSR:** Forward and reverse edge lists packed into contiguous memory (`fo`, `ro`, `fe`, `re`).
2. **Degree-0 Early Pruning:** Any query where `out_degree[s] == 0` or `in_degree[t] == 0` terminates in $O(1)$ time with distance `-1`.
3. **Bidirectional Dijkstra:** Alternating forward/reverse expansions stopping at the exact mathematical boundary `min(fwd_heap) + min(rev_heap) >= mu`.
