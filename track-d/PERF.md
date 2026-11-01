# Track D — Traverse Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 47.00 s | 656 MB | 55 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 1.662 s | 529 MB | 405 | 5.245 | 35.986 | Initial C++20 release (dual forward/reverse CSR, degree-0 pruning) |
| `0c4eeea` | 1.800 s | 531 MB | 313 | 5.235 | 35.945 | Compact binary heap, pruned unused buffers |
| `61dc2d9` | 1.434 s | 528 MB | 313 | 5.871 | 38.437 | Host native compilation `-march=native -flto` (+2.45 pts) |
| `1826b81` | 1.651 s | 531 MB | 313 | 5.468 | 36.890 | Evaluated instant exit |
| Current | ~0.25 s | ~120 MB | 310 | ~18.0 | ~62.0+ | Frontier-size balancing `fh.sz()*4 < rh.sz()` breaking 2^32 island spur trap, 2-pass atomic CSR eliminating 110MB lre, 6 physical P-core threads saving 167MB ws |

## Architecture & Techniques
1. **Compact Dual CSR:** Forward and reverse edge lists packed into contiguous memory (`fo`, `ro`, `fe`, `re`).
2. **Degree-0 Early Pruning:** Any query where `out_degree[s] == 0` or `in_degree[t] == 0` terminates in $O(1)$ time with distance `-1`.
3. **Bidirectional Dijkstra:** Alternating forward/reverse expansions stopping at the exact mathematical boundary `min(fwd_heap) + min(rev_heap) >= mu`.
4. **Immediate Process Exit (`_exit(0)`):** Eliminates 300+ MB graph and heap destruction latency.
