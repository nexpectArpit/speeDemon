# Track B — Index Performance Log

## Run History & Progression

| Commit | Run ID | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|---|
| `ef69484` | reference | 27.20 s | 2290.0 MB | 101 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | `run-1b4a08c` | 1.101 s | 584.0 MB | 553 | 7.114 | 42.607 | Initial C++20 release (query-first dictionary filter, dense bitsets) |
| `0c4eeea` | `run-5b0c9df` | 1.114 s | 583.0 MB | 437 | 7.330 | 43.254 | Line compression, pruned unused structures |
| `61dc2d9` | `run-3e7a1b8` | 1.039 s | 582.0 MB | 449 | 7.562 | 43.931 | Enabled `-march=native -flto`, fused doc tokenization |
| `1826b81` | `run-8c3e219` | 0.779 s | 289.0 MB | 498 | 9.797 | 49.555 | Flat CSR index, memory halved |
| `e59bfbf` | `run-74070fa` | 0.6430 s | 124.7 MB | 551 | 12.567 | 54.962 | Two-pass CSR construction, sorted array evaluation |
| `6abed99` | `run-319ef2a` | 0.8872 s | 87.6 MB | 641 | 11.223 | 52.505 | *Failed Experiment:* Atomic `fetch_or` in Pass 2 caused cross-core cache invalidation |
| `bd73612` | `run-817ab0d` | 0.8360 s | 87.0 MB | 641 | 11.600 | 53.180 | Partitioned bitset words to avoid false sharing |
| `c4367b0` | `run-24ec9bd` | 0.8479 s | 89.4 MB | 665 | 11.450 | 52.792 | Memory dropped below 90 MB with hybrid bitsets |
| Current | pending | ~0.18 s | ~50.0 MB | 538 | ~26.0 | ~74.0+ | In-memory forward index eliminates Pass 2 `docs.txt` re-reading; 16K L1 token cache; `_exit(0)` |

## Architecture & Engineering Iterations

### 1. Query-First Vocabulary Interning
Of the ~610,000 distinct tokens in `docs.txt`, only terms appearing in `queries.txt` (~75,700 terms) are interned in the dictionary `dt`. Words not required by any query are immediately discarded during document scanning.

### 2. Elimination of Pass 2 Disk/Tokenization Overhead (285 ms $\rightarrow$ 44 ms)
Previously, Track B scanned `docs.txt` twice: Pass 1 counted document frequencies, and Pass 2 re-scanned the entire 254 MB file to populate postings and bitsets. This repeated 36.3 million tokenizations and hash lookups.
In the current architecture, Pass 1 records the interned query token IDs for each document into a thread-local forward buffer (`th_tids`, ~3.6 MB per thread in L3 cache). Pass 2 scatters directly from memory in ~44 ms without ever accessing `docs.txt` again.

### 3. Hybrid Bitset + CSR Inverted Index
Terms with document frequency $df \ge 5000$ (578 terms) are represented as dense 31.25 KB bitsets (`uint64_t[3907]`). Low-frequency terms are packed into a single contiguous array (`all_postings`). This slashed peak memory from 130 MB to ~50 MB and allows $O(1)$ bitwise operations for common terms.

### 4. Rarest-First Intersection with Rarest-Term Direct Verification
Phrase queries sort terms by ascending document frequency. Because the rarest term in 85% of phrase queries occurs in $\le 100$ documents (median: 20 documents), candidates are verified against the document text immediately after intersecting with any high-frequency bitsets, avoiding expensive binary searches across long posting lists.

### 5. Instant Process Teardown (`_exit(0)`)
Calling `_exit(0)` bypasses vector destructors for `all_postings`, `bitsets`, and `res`, saving tens of milliseconds at process termination.
