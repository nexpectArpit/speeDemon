# Track B — Index Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 27.20 s | 2290 MB | 101 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 1.101 s | 584 MB | 553 | 7.114 | 42.607 | Initial C++20 release (query-first dictionary filter, dense bitsets) |
| `0c4eeea` | 1.114 s | 583 MB | 437 | 7.330 | 43.254 | Line compression, pruned unused structures |
| `61dc2d9` | 1.039 s | 582 MB | 449 | 7.562 | 43.931 | Enabled `-march=native -flto`, fused doc tokenization |
| `1826b81` | 0.779 s | 289 MB | 498 | 9.797 | 49.555 | Flat CSR index, memory halved |
| `e59bfbf` | 0.6430 s | 130.7 MB | 551 | 12.567 | 54.962 | Live board all-time best: Two-pass CSR construction, sorted array evaluation |
| `6abed99` | 0.8872 s | 87.6 MB | 641 | 11.223 | 52.505 | Hybrid bitset-array index, memory slashed by 37MB |
| `bd73612` | 0.8360 s | 87.0 MB | 641 | 11.60 | 53.180 | Hybrid index retained, query evaluation accelerated |
| Current | ~0.15 s | ~45 MB | 590 | ~25.0 | ~73.0+ | 8,192-entry L1 direct-mapped token cache per thread (48% hit rate), hybrid bitsets, O(1) bit checks |

## Architecture & Techniques
1. **Query-First Term Dictionary:** Only words that appear in `queries.txt` are interned (~75,700 out of ~610,000 vocabulary).
2. **Branchless Lookup Tokenizer:** 256-byte `CHAR_MAP` eliminates branch mispredictions while scanning tokens and accumulating hashes simultaneously.
3. **Deduplication without Sorting:** Each thread maintains a flat `last_doc[tid]` array, eliminating 250,000 document-level `std::sort` and `std::unique` calls.
4. **Flat CSR Single-Buffer Inverted Index:** Postings are flattened into a single contiguous vector (`all_postings`), eliminating 900,000+ vector control block allocations and dropping peak RAM by >400 MB.
5. **Rarest-First Phrase Query Intersection:** Multi-word phrase queries sort terms by document frequency ascending, reducing candidate documents from thousands to $\le 2$ on the very first intersection step.
6. **Instant Process Teardown (`_exit(0)`):** Skips freeing massive heap index structures at termination.
