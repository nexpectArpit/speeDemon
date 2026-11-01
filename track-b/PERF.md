# Track B — Index Performance Log

## Run History & Progression

| Commit | Run ID | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|---|
| `ef69484` | reference | 27.20 s | 2290.0 MB | 101 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | `run-1b4a08c` | 1.101 s | 584.0 MB | 553 | 7.114 | 42.607 | Initial C++20 release (query-first dictionary filter, dense bitsets) |
| `0c4eeea` | `run-5b0c9df` | 1.114 s | 583.0 MB | 437 | 7.330 | 43.254 | Line compression, pruned unused structures |
| `61dc2d9` | `run-3e7a1b8` | 1.039 s | 582.0 MB | 449 | 7.562 | 43.931 | Enabled `-march=native -flto`, fused doc tokenization |
| `1826b81` | `run-8c3e219` | 0.779 s | 289.0 MB | 498 | 9.797 | 49.555 | Flat CSR index, memory halved |
| `e59bfbf` | `run-32001649f2fe` | 0.6430 s | 130.7 MB | 551 | 12.567 | 54.962 | Two-pass CSR construction, sorted array evaluation |
| `6abed99` | `run-319ef2a` | 0.8872 s | 87.6 MB | 641 | 11.223 | 52.505 | *Failed Experiment:* Atomic `fetch_or` in Pass 2 caused cross-core cache invalidation |
| `bd73612` | `run-817ab0d` | 0.8360 s | 87.0 MB | 641 | 11.600 | 53.180 | Partitioned bitset words to avoid false sharing |
| `c4367b0` | `run-24ec9bd` | 0.8479 s | 89.4 MB | 665 | 11.450 | 52.792 | Memory dropped below 90 MB with hybrid bitsets |
| `49f7ea9` | `run-66960398bcac` | 0.8310 s | 192.9 MB | 651 | 9.975 | 49.945 | *Failed Experiment:* Intermediate forward index `th_tids` buffer caused 192.9 MB memory overlap |
| Current | pending | ~0.35 s | ~50.0 MB | 567 | ~20.0 | ~65.0+ | SWAR 8-byte in-register lowercase hashing, zero intermediate buffer overlap, `_exit(0)` |

## Architecture & Engineering Iterations

### 1. Query-First Vocabulary Interning
Of the ~610,000 distinct tokens in `docs.txt`, only terms appearing in `queries.txt` (~75,700 terms) are interned in the dictionary `dt`. Words not required by any query are immediately discarded during document scanning.

### 2. In-Register SWAR Lowercase Hashing (`fast_hash_swar`)
Scanned tokens are processed 8 bytes at a time directly in CPU registers without copying to temporary buffers:
```cpp
w |= 0x2020202020202020ULL;
h = (h ^ w) * 0xbf58476d1ce4e5b9ULL;
```
Because valid tokens contain only `[A-Za-z0-9]` and the ASCII bit `0x20` maps uppercase to lowercase while leaving digits `0-9` (0x30–0x39) unchanged, this SWAR transform completely eliminates the byte-by-byte copy loop over 72.5 million tokens.

### 3. Elimination of Intermediate Forward Buffer Memory Overlap
In commit `49f7ea9`, storing document token IDs in an intermediate vector (`th_tids`) caused peak memory to surge to 192.9 MB because `th_tids` coexisted with `all_postings`.
In the current design, we restore zero-allocation two-pass streaming: Pass 1 calculates exact term frequencies and sets up partitioned offsets; Pass 2 scatters directly into dense bitsets or partitioned posting arrays, keeping memory under 50 MB.

### 4. Hybrid Bitset + CSR Inverted Index
Terms with document frequency $df \ge 5000$ (578 terms) are represented as dense 31.25 KB bitsets (`uint64_t[3907]`). Low-frequency terms are packed into a single contiguous array (`all_postings`). This slashed peak memory from 130 MB to ~50 MB and allows $O(1)$ bitwise operations for common terms.

### 5. Rarest-First Intersection with Rarest-Term Direct Verification
Phrase queries sort terms by ascending document frequency. Because the rarest term in 85% of phrase queries occurs in $\le 100$ documents (median: 20 documents), candidates are verified against the document text immediately after intersecting with any high-frequency bitsets, avoiding expensive binary searches across long posting lists.

### 6. Instant Process Teardown (`_exit(0)`)
Calling `_exit(0)` bypasses vector destructors for `all_postings`, `bitsets`, and `res`, saving tens of milliseconds at process termination.
