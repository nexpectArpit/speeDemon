# Track B: Index

The task, the input files, and the exact output format are specified in the [Track B specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

Our solution is implemented in standalone standard C++20 (compiled with GCC 15 and executed on Debian 13 slim), optimized for the Falkenstein dedicated hardware (6 P-cores, 12 hardware threads, 8 GB RAM):

1. **Query-First Term Dictionary & Target-Filtered Indexing**:
   * Both `queries.txt` (~1.8 MB) and `docs.txt` (~254 MB) are memory-mapped read-only via `mmap`.
   * Because queries are available from the start, we read and parse all 50,000 queries before touching the corpus. All query terms are interned into a fast open-addressing hash table (`TermDict`, FNV-1a, load factor $\approx 28\%$).
   * Out of ~610,000 distinct words in the corpus, only ~75,700 distinct words are ever queried. Any token in `docs.txt` that is not present in the query dictionary is discarded on the fly, reducing index construction memory and time by over $85\%$.

2. **In-Place Branchless Byte Tokenizer**:
   * Tokenization conforms strictly to the specification grammar: maximal runs of ASCII bytes `[a-z0-9]` after converting `[A-Z]` to `[a-z]`. Every other byte ($\ge 0x80$, punctuation, whitespace, tabs) acts as a separator.
   * Document boundary lines are scanned in parallel across 12 worker threads. Each thread tokenizes its assigned range of documents and collects term appearances. Because document chunks across workers are strictly disjoint and ordered ($0 \le d_0 < d_1 < \dots < N-1$), concatenating worker postings yields strictly ascending document IDs with zero sorting overhead.

3. **Bitset & Vector Inverted Index Hybrid**:
   * Posting lists are stored as dense 32-bit integer vectors `std::vector<uint32_t>`.
   * For frequent terms ($df \ge 256$), we precompute 250,000-bit dense bitsets ($31.25$ KB per term).
   * In `BOOL` queries (`AND`, `OR`, `NOT` strictly left-to-right without operator precedence), operations between frequent terms execute as 3,907 64-bit vector instructions (less than $1.5 \ \mu\text{s}$ per operation).
   * Answers are extracted directly from bitsets via `__builtin_popcountll` and `__builtin_ctzll`.

4. **Zero-Allocation Phrase Verification**:
   * For `PHRASE "t1 ... tk"`, candidate documents are identified by intersecting the term posting lists, starting from the rarest anchor term ($df \le 250$). Over $98\%$ of phrases narrow down to $\le 5$ candidate documents.
   * Candidates are verified in-place by scanning the mmapped document text with `TokenSpan` structs pointing directly into the mapped file buffer, avoiding string copies and heap reallocations.

5. **Parallel Query Answering**:
   * The 50,000 queries are answered in parallel across 12 worker threads using atomic work-stealing (`query_counter.fetch_add(1)`).
   * Results are formatted into pre-reserved buffers and written in a single sequential I/O to `/out/results.txt`.

## Dependencies

None. Standard C++20 and POSIX system APIs only. Runtime dependencies count = 0 ($D = 1.0$).

## With another 24 hours

1. **AVX-512 / AVX2 SIMD Bitset Operations**: Vectorize 512-bit / 256-bit bitwise AND/OR/AND-NOT operations during boolean query evaluation.
2. **Compact Delta / StreamVByte Compression**: Compress small posting lists for rare terms to optimize L2/L3 cache residency.
3. **Huge Pages & Pre-Fault Optimization**: Advise kernel on memory access patterns using `madvise(MADV_WILLNEED | MADV_HUGEPAGE)`.
