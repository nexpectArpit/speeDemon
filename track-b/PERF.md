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
| `6aa9135` | `run-7e1892c` | 0.8520 s | 90.5 MB | 650 | 11.355 | 52.760 | SWAR in-register lowercase hashing, zero buffer overlap |
| `d0f4164` | `run-04e8d26` | 0.8308 s | 89.7 MB | 650 | 11.519 | 53.071 | *Measured Baseline:* Memory strictly bounded at 89.7 MB |
| Current | pending | ~0.40 s | ~85.0 MB | 567 | ~18.0 | ~63.0+ | Dense-anchor fallback fix, standard `std::atomic_ref` bitmap synchronization, `_exit(0)` |

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

### 3. Dense-Anchor Phrase Robustness & Candidate Enumeration
In earlier iterations, `get_pos` returned `nullptr` if the rarest term in a phrase was dense ($df \ge 5000$). For phrases containing only dense terms, this caused candidate generation to fail.
The current implementation detects dense anchor terms and unpacks candidate doc IDs directly from the term's bitset words via `__builtin_ctzll` before candidate verification, guaranteeing correctness across all phrase inputs.

### 4. Standard `std::atomic_ref` Bitmap Synchronization
Instead of reinterpreting ordinary integer arrays as atomic pointers, bitmap words are updated using standard C++20 `std::atomic_ref<uint64_t>`:
```cpp
std::atomic_ref<uint64_t>(bitsets[bs_id][id >> 6]).fetch_or(1ULL << (id & 63), std::memory_order_relaxed);
```
This guarantees portable synchronization without invoking undefined behavior.

### 5. Instant Process Teardown (`_exit(0)`)
Calling `_exit(0)` bypasses vector destructors for `all_postings`, `bitsets`, and `res`, saving tens of milliseconds at process termination.

## Final local iteration — 2026-11-02

The prior pending projection is withdrawn as measured evidence. Feed generated09:44:18Z records fdbf51b at0.853362s /94,298,112bytes /S664 /52.700144points (run-e9dc9e940c5f).

This candidate keeps exact normalized query bytes in a growable dictionary. Short-token cache keys encode all normalized bytes directly; long tokens verify exemplar bytes and lengths. AVX2 scans token boundaries in32-byte blocks on supported x86 builds, with an exact scalar fallback. Document partitions align to64-ID bitmap words, giving one writer per word and allowing ordinary OR. Dense phrase anchors retain ordered bitmap unpacking. Dense output sums use weighted masked popcounts, enumerating at most the first10 IDs for dense words.

The first NEON/always-hashed candidate was rejected after a slower native trend. Final source SHA bab7087e9622edc5b015c09a39eedee48c0ebe5fd191cb9b6dfe3e95980e8228 passes60 focused independent optimized/sanitizer regression executions plus dictionary-growth/collision, cache, tokenizer and dense-sum units. Official check.py offline image build/allowlist/sandbox and full-public output SHA pass. Native paired timing is noisy and not accepted scoring evidence; no new board points are claimed. Artifacts AUDIT/rank78-20261102 outside build contexts.

## One-pass indexing experiment — 2026-11-02

The first corpus scan now records each distinct queried-term/document hit per worker. Index construction consumes those hits instead of rescanning and rehashing the full document corpus. This directly removes the duplicated tokenization and dictionary lookup identified as B-P01. Per-worker document ranges remain aligned to64 IDs, preserving exclusive ownership of bitmap words. Official `check.py b --data data/b --expected f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927` passed. Checker wall time was4.32s including container startup; this is not scorer T. Peak memory and leaderboard T/M/S/points are not yet measured, so this is a correctness-verified optimization candidate, not a confirmed score gain.

The one-pass B change is included in the combined four-track candidate. Its earlier public-hash check remains the only new execution evidence. The later A/C/D changes have not been built or tested locally, at the user's explicit request. No combined score gain is claimed.

## Restore bounded construction — 2026-11-02

User-supplied board results for d1865fc: T≈0.895s, M≈281.5MB, S752,47.029points. Remove the intermediate64-bit hit spool and restore the direct two-pass index construction from1dcbdb5. This avoids retaining every queried-term/document pair alongside the final index. Exact dictionary identity, aligned single-writer bitmap ranges, dense-anchor enumeration and word-level result aggregation remain. The earlier commit scored52.239points on the supplied board snapshot; this is historical evidence, not a guaranteed score for this combined commit. No new local execution was run, as requested.

## Bounded compressed replay candidate — 2026-11-02

Cache the first pass's distinct queried-term/document memberships as variable-byte IDs with per-document delimiters, under a fixed48MiB total allocation budget divided among workers. Cache only complete document prefixes: roll back a partial document when the budget fills. During construction, replay the cached prefix directly and tokenize only the uncached suffix. Release each worker's cache before scanning its suffix. This retains exact sparse/dense index semantics and avoids the unbounded64-bit hit spool. The cache cap is not a total-process memory cap; final index and other state coexist with it. Extra encoding work, increased peak memory and formatted source must be repaid by lower scorer T. No local build, test or benchmark was run; score impact is pending.

## Scorer rejection of55a8300 — 2026-11-02

User-supplied result: 51.392points versus 52.043 on bf0d8c4, delta -0.651. Restore the bf0d8c4 implementation while retaining Track A's accepted ordered-output change. Acceptance uses scorer K/points with correctness gates; no new local tests or benchmarks were run. These rounded observations do not isolate every cause or estimate run variance.

## Adaptive block index candidate — 2026-11-02

Replace whole-term32-bit sparse postings and global bitmap/list transitions with65,536-document blocks. Each term/block stores sorted16-bit offsets below4,096 members, otherwise a bitmap. Boolean expressions execute left-to-right within each block, using explicit AVX2 AND/OR/AND-NOT for dense operands and a scalar tail/fallback. Phrase candidates still use rarest-first membership intersection and exact raw-token verification. Full bitmaps for every query term are not allocated.

Construction retains two corpus scans to allocate the final index exactly, with no intermediate hit spool or replay cache. Worker-local block counters become sparse write cursors; document ranges remain64-aligned for exclusive bitmap-word writes. Sparse payload is2bytes per posting; a dense block costs at most8,192bytes for at least4,096members, so posting payload is at most2times total distinct queried memberships. For the profiled22,240,530members this bound is44,481,060bytes, excluding metadata, counters, dictionaries and query scratch. This is an allocation model, not measured peak RSS.

The previous restored B baseline scored52.043points in the user-supplied bf0d8c4 screenshot. No local build, correctness check or benchmark was run for this rewrite, per the user's instruction. Scorer correctness and T/M/S/K are pending. A/C/D implementations are retained.

## Bounded document-grouped phrase candidate — 2026-11-02

Phrase membership candidates are accumulated up to a global cap of262,144 query/document pairs. Accepted candidates are grouped by document; each selected document is tokenized once for all assigned phrases, then matching `(query,document)` pairs are sorted back into query order for exact count/sum/first-ten output. Queries that exceed the shared candidate cap use the original exact per-document phrase verifier. The block index and its construction are unchanged. Candidate storage and hit storage are each bounded by the cap; sorting and per-worker token-boundary scratch add overhead.

This is an unmeasured candidate. No local build, test or benchmark was run under the scorer-driven workflow. Correctness, time, memory, formatted line count, K and board rank are pending the board result; no score gain is claimed.
