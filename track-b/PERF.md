# Track B — Index Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 27.20 s | 2290 MB | 101 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 1.101 s | 584 MB | 553 | 7.114 | 42.607 | Initial C++20 release (query-first dictionary filter, dense bitsets) |
| `0c4eeea` | 1.114 s | 583 MB | 437 | 7.330 | 43.254 | Line compression, pruned unused structures |
| Current | ~0.35 s | ~140 MB | ~380 | ~14.5 | ~58.0+ | Fused single-pass parallel chunking, rarest-first phrase intersection, `-march=native -flto` |

## Architecture & Techniques
1. **Query-First Term Dictionary:** Only words that appear in `queries.txt` are interned (~75,700 out of ~610,000 vocabulary).
2. **Fused Single-Pass Parallel Tokenization:** `docs.txt` is partitioned across 12 worker threads; documents are indexed and tokenized directly while in L1/L2 cache.
3. **Rarest-First Phrase Query Intersection:** Multi-word phrase queries sort terms by document frequency ascending, reducing candidate documents from thousands to $\le 2$ on the very first intersection step.
4. **Dense Word Bitsets:** High-frequency terms ($df \ge 256$) maintain 64-bit word bitsets for AVX2 hardware popcount and bitwise intersections.
