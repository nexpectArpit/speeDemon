# Track A: performance log

Every step from the reference to our final version, with rationale, local development measurements, and live-board run IDs.

| Step | Change | Run ID | T (s) | M (MB) | Points | Notes |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 | Official scoring box calibration reference |
| 1 | Single-threaded C++20 baseline (`mmap`, fast calendar mapping, `std::unordered_map` interning, exact sort) | *Local ARM* | 0.245 | 264 | — | 130x speedup over Python reference locally. Exact output verification against all edge cases. |
| 2 | Parallel chunking & reduction (worker shards split at newlines, parallel hour reduction) | *Local ARM* | 0.102 | 292 | — | 2.4x speedup over step 1 with 4-6 threads. Shards aggregate independently. |
| 3 | Cache-friendly open-addressing table, thread-local hour bucketed records, counting-sort scatter, $O(N)$ `std::nth_element` selection | *Local ARM* | 0.065 | 302 | — | 1.57x speedup over step 2 (3.77x over step 1). Tested under `uid 65534:65534` container sandbox with identical full public SHA match. |

*Note on local metrics: Local timings are measured using outer wall-clock subprocess execution on an Apple Silicon host. RSS includes read-only mapped input pages (238 MB), whereas the official competition environment uses cgroup v2 `memory.peak` with prewarmed `/data` page cache.*
