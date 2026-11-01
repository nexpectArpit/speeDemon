# Track C — Join

High-performance C++20 relational CSV join and country aggregation engine.

## Approach
- **Region Mapping:** Parses `regions.csv` into a contiguous direct array lookup (`r2c[region_id] = country_id`) with 1024-entry hash acceleration.
- **Parallel CSV Ingestion:** Splits `records.csv` across 12 worker threads along verified line boundaries (`.[0-9][0-9]\n`).
- **Zero-Allocation Aggregation:**
  - Amounts accumulated directly in integer cents.
  - Names interned via zero-copy byte slices into the read-only memory map.
  - Distinct names per country aggregated across threads using dense bitsets.

## Build & Dependencies
- Standard C++20 compiled with GCC 15 (`-O3 -march=native -flto -DNDEBUG`).
- Zero non-stdlib dependencies ($D = 1.0$).
