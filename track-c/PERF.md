# Track C: performance log

Every step from the reference to our final version, with rationale, local development measurements, and live-board run IDs.

| Step | Change | Run ID | T (s) | M (MB) | Points | Notes |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 14.20 | 185 | 0 | Official scoring box calibration reference |
| 1 | Single-threaded C++20 baseline (`mmap`, flat `region_to_country` lookup, zero-copy CSV parsing, exact cents) | *Local ARM* | 0.980 | 240 | — | 14.5x faster than Python reference locally. Exact output verification against all edge cases. |
| 2 | Parallel chunking & reduction (amount boundary detection, thread-local worker states, dynamic bitset country union) | *Local ARM* | 0.316 | 365 | — | 3.1x faster than step 1 (45x over Python reference). Verified on full seed 1 dataset. |
| 3 | Index-based 8-byte hash entries, dynamic country scaling, ASan & UBSan hardening, bounded chunk scanning | *Local ARM* | 0.242 | 360 | — | 58.6x speedup over Python reference. Passes all 14 official unit tests, worked example, adversarial rules, and tiny/sample seeds under AddressSanitizer and full Docker sandbox. |

*Note on local metrics: Local timings are measured using outer wall-clock subprocess execution on an Apple Silicon host. RSS includes read-only mapped input pages (200 MB), whereas the official competition environment uses cgroup v2 `memory.peak` with prewarmed `/data` page cache.*
