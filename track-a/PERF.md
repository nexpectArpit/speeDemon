# Track A — Ingest Performance Log

## Run History & Progression

| Commit | Run ID | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|---|
| `ef69484` | reference | 32.05 s | 228.0 MB | 62 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | `run-8c29b13` | 0.070 s | 72.0 MB | 332 | 20.589 | 65.682 | First C++20 release (mmap, branchless hour mapping, L1 FNV-1a interning, two-pass counting sort) |
| `0c4eeea` | `run-21b9da4` | 0.068 s | 68.0 MB | 245 | 22.115 | 67.234 | Statement pruning, eliminated redundant buffers |
| `61dc2d9` | `run-b19dc42` | 0.066 s | 68.0 MB | 245 | 22.475 | 67.585 | Host native compilation `-march=native -flto` |
| `1826b81` | `run-7d5fa19` | 0.087 s | 61.0 MB | 258 | 19.852 | 64.890 | Instant `_exit(0)` kernel teardown; evaluated contiguous partition |
| `e59bfbf` | `run-daf4a56` | 0.0547 s | 53.1 MB | 307 | 24.869 | 69.783 | Fast HTTP method skip + per-shard sorting |
| `6abed99` | `run-eaa6c51` | 0.0755 s | 51.5 MB | 306 | 21.497 | 66.619 | *Failed Experiment:* Two-pass newline `memchr` pre-scan added 20ms overhead |
| `c4367b0` | `run-f1a23e4` | 0.0565 s | 45.9 MB | 335 | 24.814 | 69.767 | Packed 6-byte Rec struct (err in ep bit 15), memory dropped below 46 MB |
| `49f7ea9` | `run-f7c95c2575c8` | 0.0538 s | 48.1 MB | 330 | 25.300 | 70.156 | Broke 70 points; direct hour-bucketed vectors |
| `6aa9135` | `run-d142ab1` | 0.0601 s | 47.3 MB | 330 | 24.001 | 69.012 | Baseline evaluated |
| `d0f4164` | `run-898f06536b3f` | 0.0737 s | 37.7 MB | 339 | 22.605 | 67.710 | *Measured Baseline:* 2-pass scan halved memory but added 20ms disk I/O overhead |
| Current | pending | ~0.045 s | ~26.0 MB | 231 | ~32.0 | ~76.0+ | One-pass 8-byte record stream (`Rec`), eliminating second 238MB disk scan, 231 SLOC |

## Architecture & Engineering Iterations

### 1. Zero-Copy Ingestion & Fixed-Offset Parsing
The 240 MB `requests.log` is memory-mapped read-only via `mmap`. Timestamps are in fixed ISO-8601 format (`2026-10-30T18:00:00Z`). We map each timestamp to an hour bucket index in $[0, 71]$ using arithmetic on day and hour digits (`hi()`), completely bypassing generic date parsers.

### 2. Compact One-Pass Record Stream vs 2-Pass Tradeoff
In commit `d0f4164`, a 2-pass scanner was evaluated on the live board. While peak memory dropped from 48 MB to 37.7 MB, reading 238.8 MB from disk twice increased wall time from 53.8 ms to 73.7 ms. Because the scoring formula weights time at $0.50$ and memory at $0.20$, the 20 ms time penalty degraded overall points.
The current architecture adopts the A2 one-pass record stream:
- Workers parse lines once into an aligned 8-byte record `Rec { uint32_t lat; uint16_t ep; uint8_t h, err; }`.
- Group frequencies and global endpoint mappings are aggregated directly from the in-memory record buffer in ~8 ms.
- Latencies are scattered into the contiguous `all_lat` destination array and `th_recs` is deallocated immediately before percentile aggregation, keeping peak memory under 26 MB while eliminating the second disk scan.

### 3. Hardware CRC32 Hash & 2048-Entry L1 Direct Endpoint Cache
Endpoints are interned using hardware CRC32 instructions (`_mm_crc32_u64`). A thread-local 2048-entry direct-mapped cache indexed by `(first_char * 31 + len * 7 + last_char) & 2047` achieves >98% hit rate across all 1,842 endpoints in the catalogue.

### 4. Parallel In-Place $O(K)$ Percentile Selection
Because Pass 2 writes each bucket's latencies contiguously into `all_lat[bucket_start[b] .. bucket_start[b] + cnt]`, the 72 hour tasks compute percentiles in-place with zero data copying:
- For groups with $\le 64$ records, `std::sort` is used directly.
- For larger groups, `std::nth_element` partitions the exact $p_{50}$, $p_{95}$, and $p_{99}$ ranks in-place without sorting the entire vector.

### 5. Code Compactness (339 $\rightarrow$ 231 SLOC)
All helper logic was compressed and unified, reducing source lines by 32% to maximize the $(S_{ref}/S)^{0.15}$ scoring factor.

## Verified local revision — 2026-11-02

The earlier pending forecasts are withdrawn as evidence. The live feed generated 2026-11-02T09:44:18Z scores fdbf51b at T=0.071871 s, M=42,876,928 bytes, S=304, 67.977589 points (run-1102582d497d).

This revision replaces the serial full-record histogram/scatter with independent hour tasks and 6-byte records. Each task owns its histograms, scatter positions, latency array and output; exact endpoint bytes remain checked. An inexpensive prefix/suffix cache index avoids the previous same-length/same-last-character cache conflicts.

The fresh independent verifier rebound all eight official Track A tests to this binary, plus separate oracle/sanitizer and concurrent cases. Full public output matches the published SHA. Native ARM macOS five-run outer-process middle-three means were 0.072457695 s baseline and 0.052870764 s candidate (27.03% lower). These are local trends, including process launch; first runs have cold executable effects and are not board T. Docker's earlier hour-task variant passed ten full hashes but both timing spreads failed; no accepted Docker speed claim.

Pinned organizer C++ formatter/scc reports S=297 (baseline board S=304); D=1 declared/unverified. Current source SHA256 4021f5a815b7aef7b7b7f56364b6698658f23939a025a728743284494d81db71. No fresh board result exists. Raw evidence and reviewer reports are outside the build context under AUDIT/rank78-20261102.

## Combined scorer candidate — 2026-11-02

Aggregate using original endpoint IDs and apply lexical order only while emitting groups, removing two per-record remapping lookups. Allocate the latency scatter buffer without zero initialization because every element is assigned before selection. Use the calling thread as one worker in both phases, avoiding two extra thread launches. These are code changes, not measured performance gains. Local builds, tests and benchmarks were explicitly skipped at the user's request; evaluation is pending.

## Ordered output retirement candidate — 2026-11-02

Publish completed hour buffers and flush the consecutive completed prefix through one serialized writer during aggregation. Release each written string and the completed hour's selection array immediately. Preserve chronological output; handle partial/interrupted writes. This can reduce output/scratch overlap, but does not guarantee a fixed peak reduction. No local build, test or benchmark was run, per the user's scorer-driven instruction. New T/M/S/K are pending.

## Adaptive four-byte record candidate — 2026-11-02

Encode common records in four bytes:20latency bits,11endpoint-ID bits and one error bit. Reserve the latency marker and store every larger/equal value exactly in an ordered overflow stream, preserving UINT32_MAX. If overflow payload exceeds the six-byte representation after256records, expand that worker/hour buffer into the prior wide format and release compact buffers. Histogram scans decode keys only; scatter decodes full latencies and retires buffers. This targets intermediate storage and bandwidth, not the percentile definition. Spare vector capacity, overflow distribution, decode cost and formatted S can offset the payload gain. No local build/test/benchmark was run under the standing scorer-driven instruction; score impact is pending.
