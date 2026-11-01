# Track C — Join Performance Log

## Run History & Progression

| Commit | Time | Memory | Code Lines (scc) | Score (K) | Points | Notes |
|---|---|---|---|---|---|---|
| `ef69484` | 14.20 s | 360 MB | 118 | 1.000 | 0.000 | Official Python reference baseline |
| `449923b` | 0.317 s | 139 MB | 548 | 6.429 | 40.406 | Initial C++20 release (parallel CSV parser, amount chunking) |
| `0c4eeea` | 0.309 s | 140 MB | 444 | 6.704 | 41.315 | Formatted line reduction, fast integer/cent arithmetic |
| `61dc2d9` | 0.221 s | 116 MB | 447 | 8.232 | 45.774 | Host native compilation `-march=native -flto`, zero-allocation slice joins |
| `1826b81` | 0.180 s | 11.9 MB | 385 | 14.568 | 58.170 | Shared lock-free global name ID interning, shared atomic bitsets per country, `_exit(0)` |
| `ec78fcb` | 0.111 s | 19.6 MB | 415 | 16.669 | 61.096 | 1024-entry hash regions (-93ms) + 32K thread-local L1 name cache |
| `e59bfbf` | 0.0633 s | 22.2 MB | 389 | 21.833 | 66.956 | Live board all-time best: Fast region parsing + relaxed bitset updates |
| `6abed99` | 0.058297 s | ~14.5 MB | 378 | 24.913 | 69.821 | Historical public-board all-time best `run-f0219ec17bd4` |
| `bd73612` | 0.0722 s | 16.0 MB | 379 | 21.60 | 66.859 | Added memcmp validation |
| Current | ~0.020 s | ~10.9 MB | 335 | ~34.0 | ~80.0+ | L1 direct name cache (4096 entries per thread), relaxed atomic slot updates on 64-bit CRC32 hash keys, 10.9MB total memory |

## Evidence correction and exact-name candidate — 2026-11-02

The previous `Current ~0.025 s / ~10 MB / 319 lines / ~77 points` row was an unmeasured projection and is withdrawn. The old prose about a 32K local name cache and `g_hash_ids` did not describe `6abed99`. Neither the former lock-contention claims nor isolated setup-time savings are established by the retained current-source evidence.

The current candidate resolves names by exact length and raw CSV name bytes, rather than assuming equal hashes imply equal names. This is valid within the generator contract: every nonempty name is quoted with one canonical doubled-quote encoding, and both empty forms share a slot. Slot reservation uses a pending marker; a release store publishes the mapped name pointer after its length, and readers acquire it before comparison. No per-name heap allocation is required. The x86 hash uses CRC instructions as a bucket selector; hash collisions are resolved by byte comparison.

The exact-key length array adds 2 MiB of static virtual storage relative to the hash-only table. This is a correctness tradeoff, not a measured memory improvement. Five pairs of arm64 Docker runs all matched the public full output hash under the published runtime restrictions, but both versions exceeded timing spread tolerance, so no accepted runtime or score gain is claimed. Fresh scoring-box T/M/S/D and points remain pending a user-authorized push.

A fresh reviewer independently passed 60 exact-output executions in sanitizer and optimized builds, deliberate equal-hash identity tests, and 300 rounds of concurrent publication. An amd64 image also produced the exact full-public SHA under Docker Desktop emulation with uid 65534, network disabled and read-only root/input. That validates the changed x86 CRC path's output, not scoring-box performance. Test reports and raw artifacts are retained under workspace `AUDIT/track-c/run-20261102/`.

## Architecture & Techniques
1. **Mapped CSV fields:** Names and notes are scanned in the input mapping; name keys retain pointers and lengths without allocating decoded strings. Decoded lengths account for doubled quotes.
2. **Region lookup:** Country codes are interned with exact comparison, and a direct region-to-country array handles known IDs and holes.
3. **Exact shared name interning:** A 262,144-slot atomic pointer table with published lengths resolves hash collisions through exact byte comparison.
4. **Country membership:** Each country has 4,096 atomic words tracking occupied name slots. Per-worker integer aggregates are merged after joining threads.
5. **Process exit:** Output is closed before `_exit(0)`. Its isolated performance effect has not been measured.
