# Track B — Index

High-performance C++20 inverted index and boolean/phrase query engine.

## Approach
- **Query-First Indexing:** Scans `queries.txt` first to populate a fixed-size hash table of queried vocabulary. Unqueried words are bypassed during document ingestion.
- **Parallel Single-Pass Indexing:** Maps and tokenizes documents concurrently across 12 threads without intermediate text buffers.
- **Query Processing:**
  - **Boolean queries:** Evaluated strictly left-to-right via bitset logic and hardware POPCNT.
  - **Phrase queries:** Terms are intersected in ascending document-frequency order before verifying token sequence.

## Build & Dependencies
- Standard C++20 compiled with GCC 15 (`-O3 -march=native -flto -DNDEBUG`).
- Zero non-stdlib dependencies ($D = 1.0$).
