# Track A — Ingest Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 32.05 s | 228 MB | 62 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.070 s | 72 MB | 332 | 20.589 | 65.682 | First C++20 release (mmap, L1 FNV-1a interning, two-pass counting sort) |
| `0c4eeea` | 0.068 s | 68 MB | 245 | 22.115 | 67.234 | Statement pruning, eliminated redundant buffers |
| `61dc2d9` | 0.066 s | 68 MB | 245 | 22.475 | 67.585 | Host native compilation `-march=native -flto` |
| `1826b81` | 0.087 s | 61 MB | 258 | 19.852 | 64.890 | Investigated contiguous partition; added instant `_exit(0)` kernel teardown |
| `e59bfbf` | 0.0547 s | 55.6 MB | 307 | 24.869 | 69.783 | Live board all-time best: Fast method skip + per-shard sorting |
| `6abed99` (board latest) | 0.075523 s | ~51.5 MiB | 306 | 21.497 | 66.619 | `run-eaa6c51a9d34`; correctness gates passed, but slower than the previous best |
| `e59bfbf` (current recovery candidate) | 0.054663 s | 55,668,736 B | 307 | 24.869 | 69.783 | `run-daf4a5657af7`; previous all-time board best, exact parser restored for revalidation |

## Evidence correction — 2026-11-02

The earlier `Current ~0.035 s / ~40 MB / 296 lines / ~76 points` row was a projection with no reproducible participant run record. It is withdrawn and must not be cited as a measured result. The `6abed99` latest board run was 0.075523 s and 66.618887 points; the earlier `e59bfbf` run remains the best measured Track A result in the public feed. A paired native arm64 comparison also favored `e59bfbf` (middle-three subprocess means 0.065624 s versus 0.072630 s), but those local measurements are not official scoring-box T/M. Full exact-output validation on `6abed99` passed 16 participant executions; see workspace `../../research/track-a/evidence/validation-6abed99-20261102.json`. Docker/cgroup scoring gates were unavailable, and no board run exists for this recovery candidate yet.

After restoring the exact `e59bfbf` source in the isolated recovery worktree, a second paired native batch again produced the exact full-public output hash in all ten runs. The middle-three subprocess means were 0.075204 s for the `6abed99` binary and 0.068164 s for the recovered binary. This confirms the direction on this arm64 host; it is not a substitute for the official timer, compiler, or cgroup memory measurement. The recovered source also passed 16 participant executions including unseen full seed 23; see `../../research/track-a/evidence/validation-recovered-e59-worktree-20261102.json` and `paired-native-recovered-e59-6ab-20261102.json` in the workspace research artifacts.

The earlier architecture description saying the pre-sized pool eliminated the intermediate record vector was inaccurate for `e59bfbf` and is withdrawn. The current description below matches the source's `raw` vector and hour-scatter implementation.

## Commit-time independent checks — 2026-11-02

A fresh auditor independently passed 15 guarded ASan/UBSan executions, including distinct rank boundaries around 64, all 72 hours, all 1,842 catalogue endpoints, and worker counts 1, 2, 3, 11 and 12. Both sources built offline using the pinned Dockerfile. Twenty paired full-public Docker executions passed the exact output hash, uid 65534, read-only root/input, no network, resource limits, and filesystem-diff checks. The arm64 image is 101,172,380 bytes. The official arm64 timer was used, but recovery timings exceeded its spread tolerance in both batches; no accepted container speed result or cgroup peak-memory result is claimed.

An independent five-pair native follow-up did **not** reproduce the earlier speed direction: middle-three means were 0.070219 s for `6abed99` and 0.077889 s for recovery. All ten output hashes matched, but cold launches and short-run variability limit interpretation. Local timing evidence is mixed. This commit restores the exact source with the better historical board result; a fresh scoring-box run is required before claiming recovered points. Evidence is preserved outside the submission checkout under workspace `AUDIT/track-a/run-20261102/`.

## Architecture & Techniques
1. **Zero-Copy Parsing:** Memory-mapped read-only input buffer (`mmap`), parsing timestamps with fixed-offset math `hi(const char* t)`.
2. **Per-Thread Hour Scatter:** Each worker parses into a reserved `raw` record vector, counts records by hour, computes hour offsets, then scatters into its contiguous `recs` pool. This retains an intermediate vector but gives later processing hour-contiguous ranges.
3. **Endpoint Group Scatter:** Count records by endpoint group within each hour, compute prefix offsets, then scatter latencies into contiguous per-group slices before selection.
4. **Exact Percentiles:** Direct `std::sort` for groups of at most 64 records; repeated `std::nth_element`/`max_element` operations for larger groups.
5. **Process Exit (`_exit(0)`):** Exit after explicitly closing the output descriptor, without running C++ container destructors. Its individual timing effect has not been isolated.
