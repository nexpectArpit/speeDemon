# Track D: performance log

Every step from the reference to our final version, with rationale, local development measurements, and live-board run IDs.

| Step | Change | Run ID | T (s) | M (MB) | Points | Notes |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 47.00 | 915 | 0 | Official scoring box calibration reference |
| 1 | Baseline C++20 Forward Dijkstra (CSR representation, flat binary heap, visited list reset, $O(1)$ degree-0 pruning) | *Local ARM* | 1.304 | 341 | — | 36x faster than Python reference locally. Exact output verification against all edge cases. |
| 2 | Bidirectional Dijkstra with exact $\text{top}_f + \text{top}_r \ge \mu$ stopping condition and dual forward/reverse CSR | *Local ARM* | 1.250 | 614 | — | 37.6x faster than Python reference. Verified against all handwritten edge cases and tiny seeds. |
| 3 | Parallel multi-threaded graph ingestion & edge decoding across 12 threads | *Local ARM* | 1.163 | 656 | — | 40.4x faster than Python reference. Passes all 14 official unit tests, worked example, 64-bit distances, and tiny/sample seeds under AddressSanitizer and full Docker sandbox. |

*Note on local metrics: Local timings are measured using outer wall-clock subprocess execution on an Apple Silicon host. RSS includes read-only mapped input pages (92 MB), whereas the official competition environment uses cgroup v2 `memory.peak` with prewarmed `/data` page cache.*
