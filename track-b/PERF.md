# Track B: performance log

Every step from the reference to our final version, with rationale, local development measurements, and live-board run IDs.

| Step | Change | Run ID | T (s) | M (MB) | Points | Notes |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 27.20 | 2290 | 0 | Official scoring box calibration reference |
| 1 | Query-first term filtering (index only ~75,700 queried terms), in-place branchless tokenizer, sorted vector postings | *Local ARM* | 1.850 | 780 | — | 14.7x faster than Python reference locally. Exact output verification against all edge cases. |
| 2 | Dense 31.25 KB bitsets for frequent terms ($df \ge 256$), left-to-right bitwise evaluation, popcount/ctzll extraction | *Local ARM* | 1.120 | 830 | — | 24.3x faster than Python reference. |
| 3 | Zero-allocation `TokenSpan` in-place phrase scanner, parallel query evaluation across 12 threads | *Local ARM* | 0.967 | 850 | — | 28.1x speedup over Python reference. Passes all 15 official unit tests, worked example, handwritten edge cases, and tiny seeds under AddressSanitizer and full Docker sandbox. |

*Note on local metrics: Local timings are measured using outer wall-clock subprocess execution on an Apple Silicon host. RSS includes read-only mapped input pages (256 MB), whereas the official competition environment uses cgroup v2 `memory.peak` with prewarmed `/data` page cache.*
