# Track D: Traverse

The task, the input files, and the exact output format are specified in the [Track D specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md).

## Approach

Our solution is implemented in standalone standard C++20 (compiled with GCC 15 and executed on Debian 13 slim), optimized for the Falkenstein dedicated hardware (6 P-cores, 12 hardware threads, 8 GB RAM) and tailored to the road network topology of Track D:

1. **Parallel Multi-Chunk Graph Ingestion & Dual Forward/Reverse CSR**:
   * Both `graph.txt` (~92 MB) and `queries.txt` (~29 KB) are mapped read-only via `mmap`.
   * Input parsing is parallelized across 12 worker threads splitting at newline boundaries. Each thread converts ASCII edge triplets `(u, v, w)` into compact thread-local binary records in parallel (~15 ms).
   * From the parsed edges, we build both Forward CSR (`fwd_offset`, `fwd_edges`) and Reverse CSR (`rev_offset`, `rev_edges`) in a single contiguous memory layout.
   * Total graph representation requires only ~94 MB of RAM, leaving over 7.8 GB of headroom within the 8 GB limit.

2. **$O(1)$ Early Elimination of Unreachable and Identity Queries**:
   * If $s == t$, the answer is immediately 0 without any search (even on self-loops or isolated nodes).
   * If $\text{out\_degree}[s] == 0$ (isolated node or node with no out-edges) or $\text{in\_degree}[t] == 0$ (isolated node or node with no in-edges), the target is provably unreachable: the answer is immediately -1 in $O(1)$ time without exploring any nodes.

3. **Bidirectional Dijkstra with Exact Bounded Stopping Condition**:
   * Normal queries are answered using symmetric Bidirectional Dijkstra: forward search from $s$ using `fwd_edges` and backward search from $t$ using `rev_edges`.
   * On a 2D lattice road network, bidirectional exploration cuts the search radius by half and reduces the total settled nodes from $O(\pi R^2)$ to $O(2 \cdot \pi (R/2)^2) = O(\frac{1}{2} \pi R^2)$, reducing settled node count by $2\times$ to $10\times$ relative to unidirectional Dijkstra.
   * To ensure 100% exact correctness across all weights (including weight-0 edges, parallel edges, and weights $\ge 2^{32} - 1$), the search maintains the shortest known path length $\mu = \min(d_f[u] + w + d_r[v])$ across frontier edges and terminates the moment $\text{top}_f + \text{top}_r \ge \mu$.

4. **Cache-Aligned Flat Heap & Nanosecond Timestamp Reset**:
   * Custom cache-aligned flat binary min-heaps (`FastMinHeap`) avoid node allocations and pointer indirection.
   * Rather than clearing the 1.74M node distance arrays ($14 \text{ MB} \times 2000 \approx 28 \text{ GB}$ of memset bandwidth), each thread tracks modified nodes in a thread-local `visited` list and resets only the visited entries, reducing state clearing per query from milliseconds to nanoseconds.

5. **Parallel Query Processing**:
   * The 2,000 queries are processed in parallel across all 12 worker threads using atomic work-stealing (`query_idx.fetch_add(1)`).
   * Results are formatted sequentially into a pre-reserved buffer and flushed in a single `fwrite` to `/out/distances.txt`.

## Dependencies

None. Standard C++20 and POSIX system APIs only. Runtime dependencies count = 0 ($D = 1.0$).

## With another 24 hours

1. **Radix Heap / 4-Ary Heap**: Further reduce branch mispredictions and heap bubble-down depth with 4-way branching.
2. **Component Labeling / SCC Pruning**: Precompute connected component IDs on the 8 tiny disconnected islands to instantly prune any cross-component queries.
3. **Huge Pages & Pre-Fault Optimization**: Advise kernel on memory access patterns using `madvise(MADV_WILLNEED | MADV_HUGEPAGE)`.
