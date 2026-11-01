# Track D — Traverse Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 47.00 s | 656 MB | 55 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 1.662 s | 529 MB | 405 | 5.245 | 35.986 | Initial C++20 release (dual forward/reverse CSR, degree-0 pruning) |
| `0c4eeea` | 1.800 s | 531 MB | 313 | 5.235 | 35.945 | Compact binary heap, pruned unused buffers |
| `61dc2d9` | 1.434 s | 528 MB | 313 | 5.871 | 38.437 | Host native compilation `-march=native -flto` (+2.45 pts) |
| `1826b81` | 1.651 s | 531 MB | 313 | 5.468 | 36.890 | Evaluated instant exit |
| `e59bfbf` | 0.7392 s | 268.1 MB | 319 | 9.339 | 48.515 | Live board all-time best: 2-pass atomic CSR without 110MB lre |
| `6abed99` | 0.710223 s | 268,976,128 B | 319 | 9.522 | 48.936 | Historical public run `run-ca7c0b3a0042`; no sparse-workspace candidate score yet |

## Sparse distance workspace — 2026-11-02

The old `Current ~0.25 s / ~120 MB / 284 lines / ~62 points` row was a projection and is withdrawn. The previous six query workers eagerly initialized two uint64 distance arrays for every graph node: 167,089,152 bytes at full scale, even for short searches. The candidate uses a dynamically growing, exact-node-keyed table per worker, stores both directional distances together, and clears only occupied slots between queries. It imposes no assumed search-radius or visited-node cap. Growth rehashes all active states, preserving both distances.

A fresh auditor passed 131 ASan/UBSan executions and 116,264 exact answers, including random-graph Dijkstra/Floyd oracles, deep/wide growth and reset/reinsert stress. Two paired full-public arm64 Docker batches produced the exact output hash in all 20 executions. The second batch's maximum local in-container cgroup peaks were 362,053,632 B for baseline and 265,453,568 B for candidate (about 27% lower). These include the local timer/container accounting and are not scoring-box M. Candidate process time was lower in each paired run, but baseline middle-three spread exceeded tolerance in both batches, so no accepted T or new points are claimed. Raw samples, source identities and methods are retained outside the submission checkout in workspace `AUDIT/track-d/run-20261102/`.

The official static-axis script, using the exact pinned C++ formatter and scc binaries in a C++-only tools image, measured S=319 for baseline and S=352 for candidate; D=1 remains declared/unverified as before. The candidate's amd64 image also matched the exact public-full SHA under restricted Docker Desktop emulation. These checks establish source-size cost and output correctness, not a new scoring-box result.

## Architecture & Techniques
1. **Compact Dual CSR:** Forward and reverse edge lists packed into contiguous memory (`fo`, `ro`, `fe`, `re`).
2. **Degree-0 Early Pruning:** Any query where `out_degree[s] == 0` or `in_degree[t] == 0` terminates in $O(1)$ time with distance `-1`.
3. **Bidirectional Dijkstra:** Alternating forward/reverse expansions stopping at the exact mathematical boundary `min(fwd_heap) + min(rev_heap) >= mu`.
4. **Sparse query state:** Exact open-addressed node keys, uint64 forward/reverse distances, dynamic growth, and occupied-slot reset. Query worker count is at most six; CPU placement is controlled externally by the harness.
5. **Process Exit (`_exit(0)`):** Output is explicitly closed before exiting without C++ container destruction. Its isolated timing effect is unmeasured.
