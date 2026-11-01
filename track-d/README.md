# Track D — Traverse

High-performance C++20 graph shortest-path engine.

## Approach
- **Graph Representation:** Builds forward and reverse Compressed Sparse Row (CSR) arrays for cache-friendly neighbor iteration.
- **Pruning & Bounded Search:**
  - $O(1)$ degree-0 pruning for disconnected nodes.
  - Symmetrical bidirectional Dijkstra search with exact stopping condition `fwd_min + rev_min >= mu`.
- **Parallel Queries:** Queries executed concurrently across 12 worker threads with pre-allocated thread-local state.

## Build & Dependencies
- Standard C++20 compiled with GCC 15 (`-O3 -march=native -flto -DNDEBUG`).
- Zero non-stdlib dependencies ($D = 1.0$).
