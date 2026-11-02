#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct MF {
    const char* d = nullptr;
    size_t s = 0;
    MF(const std::string& p) {
        int fd = open(p.c_str(), O_RDONLY);
        struct stat st{}; fstat(fd, &st); s = st.st_size;
        d = s ? (const char*)mmap(nullptr, s, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        close(fd);
    }
};

static constexpr auto make_char_map() {
    std::array<uint8_t, 256> m{};
    for (int c = 0; c < 256; ++c) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) m[c] = c;
        else if (c >= 'A' && c <= 'Z') m[c] = c + 32;
    }
    return m;
}
static constexpr auto CHAR_MAP = make_char_map();

static constexpr size_t TS = 262144;
static constexpr uint32_t TM = TS - 1;

inline uint64_t fast_hash_swar(const char* p, size_t len) {
    uint64_t h = 0x517cc1b727220a95ULL ^ (len * 0x9e3779b97f4a7c15ULL);
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        w |= 0x2020202020202020ULL;
        h = (h ^ w) * 0xbf58476d1ce4e5b9ULL;
        i += 8;
    }
    if (i < len) {
        uint64_t rem = 0;
        memcpy(&rem, p + i, len - i);
        uint64_t mask = (len - i == 8) ? ~0ULL : ((1ULL << ((len - i) * 8)) - 1);
        rem = (rem | 0x2020202020202020ULL) & mask;
        h = (h ^ rem) * 0x94d049bb133111ebULL;
    }
    return (h ^ (h >> 32)) | (uint64_t(len) << 56) | 1ULL;
}

inline bool same_word(const char* a, const char* b, size_t length) {
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8); memcpy(&y, b + i, 8);
        if ((x | 0x2020202020202020ULL) != (y | 0x2020202020202020ULL)) return false;
    }
    for (; i < length; ++i) if ((uint8_t(a[i]) | 32) != (uint8_t(b[i]) | 32)) return false;
    return true;
}

struct DE { uint64_t h64 = 0; uint32_t id = 0, key = 0; };
struct Dict {
    std::vector<DE> tb{TS};
    std::vector<char> keys;
    uint32_t count = 0;
    void grow() {
        auto old = std::move(tb);
        tb = std::vector<DE>(old.size() * 2);
        for (const auto& e : old) if (e.id) {
            uint32_t x = uint32_t(e.h64) & uint32_t(tb.size() - 1);
            while (tb[x].id) x = (x + 1) & uint32_t(tb.size() - 1);
            tb[x] = e;
        }
    }
    bool equal(const DE& e, const char* s, size_t n) const {
        return (e.key >> 24) == n && same_word(keys.data() + (e.key & 0xffffffu), s, n);
    }
    uint32_t in(std::string_view s) {
        uint64_t h = fast_hash_swar(s.data(), s.size());
        uint32_t mask = uint32_t(tb.size() - 1), x = uint32_t(h) & mask;
        while (tb[x].id) {
            if (tb[x].h64 == h && equal(tb[x], s.data(), s.size())) return tb[x].id - 1;
            x = (x + 1) & mask;
        }
        if ((count + 1) * 2 > tb.size()) { grow(); return in(s); }
        uint32_t id = count++;
        // At most50000 queries x8 terms x32 bytes fits a24-bit arena offset.
        uint32_t key = uint32_t(keys.size()) | (uint32_t(s.size()) << 24);
        keys.insert(keys.end(), s.begin(), s.end());
        tb[x] = {h, id + 1, key};
        return id;
    }
    uint32_t fn_fast(uint64_t h, const char* s, size_t n) const {
        uint32_t mask = uint32_t(tb.size() - 1), x = uint32_t(h) & mask;
        while (tb[x].id) {
            if (tb[x].h64 == h && equal(tb[x], s, n)) return tb[x].id - 1;
            x = (x + 1) & mask;
        }
        return ~0u;
    }
};

struct TokenCache {
    struct Entry { uint64_t key = 0; uint32_t tid = ~0u, offset = 0; };
    std::vector<Entry> entries{16384};
    std::vector<uint8_t> lengths = std::vector<uint8_t>(16384);
    uint32_t lookup(const Dict& dict, const char* base, const char* s, size_t n) {
        uint64_t key = 0;
        if (n <= 8) {
            memcpy(&key, s, n);
            key |= 0x2020202020202020ULL;
            if (n < 8) key &= (1ULL << (8 * n)) - 1;
        } else key = fast_hash_swar(s, n);
        uint32_t index = uint32_t(((key ^ (key >> 32)) * 0x9e3779b97f4a7c15ULL) >> 50);
        auto& e = entries[index];
        if (e.key == key && lengths[index] == n && (n <= 8 || same_word(base + e.offset, s, n))) return e.tid;
        uint64_t h = n <= 8 ? fast_hash_swar(s, n) : key;
        uint32_t tid = dict.fn_fast(h, s, n);
        e = {key, tid, uint32_t(s - base)};
        lengths[index] = uint8_t(n);
        return tid;
    }
};

template<class F> inline void for_each_token(const char* p, const char* end, F consume) {
#if defined(__AVX2__)
    const char* start = p;
    uint32_t previous = 0;
    while (size_t(end - p) >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)p);
        __m256i lower = _mm256_or_si256(v, _mm256_set1_epi8(32));
        __m256i letters = _mm256_and_si256(_mm256_cmpgt_epi8(lower, _mm256_set1_epi8('a' - 1)), _mm256_cmpgt_epi8(_mm256_set1_epi8('z' + 1), lower));
        __m256i digits = _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8('0' - 1)), _mm256_cmpgt_epi8(_mm256_set1_epi8('9' + 1), v));
        uint32_t mask = uint32_t(_mm256_movemask_epi8(_mm256_or_si256(letters, digits)));
        uint32_t changes = mask ^ ((mask << 1) | previous);
        while (changes) {
            unsigned bit = __builtin_ctz(changes);
            if ((mask >> bit) & 1) start = p + bit;
            else consume(start, size_t(p + bit - start));
            changes &= changes - 1;
        }
        previous = mask >> 31;
        p += 32;
    }
    for (; p < end; ++p) {
        uint32_t current = CHAR_MAP[uint8_t(*p)] != 0;
        if (current != previous) {
            if (current) start = p;
            else consume(start, size_t(p - start));
            previous = current;
        }
    }
    if (previous) consume(start, size_t(end - start));
#else
    while (p < end) {
        while (p < end && !CHAR_MAP[uint8_t(*p)]) ++p;
        if (p == end) break;
        const char* start = p;
        while (p < end && CHAR_MAP[uint8_t(*p)]) ++p;
        consume(start, size_t(p - start));
    }
#endif
}

enum class Op : uint8_t { AND, OR, NOT };

template<Op O> inline void apply_words(uint64_t* out, const uint64_t* rhs, size_t words) {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 4 <= words; i += 4) {
        __m256i a = _mm256_loadu_si256((const __m256i*)(out + i));
        __m256i b = _mm256_loadu_si256((const __m256i*)(rhs + i));
        __m256i r;
        if constexpr (O == Op::AND) r = _mm256_and_si256(a, b);
        else if constexpr (O == Op::OR) r = _mm256_or_si256(a, b);
        else r = _mm256_andnot_si256(b, a);
        _mm256_storeu_si256((__m256i*)(out + i), r);
    }
#endif
    for (; i < words; ++i) {
        if constexpr (O == Op::AND) out[i] &= rhs[i];
        else if constexpr (O == Op::OR) out[i] |= rhs[i];
        else out[i] &= ~rhs[i];
    }
}

struct QItem {
    bool ph = false;
    std::vector<uint32_t> t;
    std::vector<Op> op;
    std::vector<std::string_view> rw;
};

struct DocRef { const char* p; uint32_t l; };

struct PostingBlock {
    void* data = nullptr;
    uint32_t count = 0, capacity = 0;
    bool bitmap() const { return capacity == ~0u; }
    const uint16_t* small() const { return (const uint16_t*)data; }
    const uint64_t* bits() const { return (const uint64_t*)data; }
    void add(uint16_t doc, uint32_t words) {
        if (bitmap()) ((uint64_t*)data)[doc >> 6] |= 1ULL << (doc & 63);
        else if (count + 1 >= words * 4) {
            auto* bits = (uint64_t*)calloc(words, sizeof(uint64_t));
            if (!bits) _exit(1);
            const auto* old = small();
            for (uint32_t i = 0; i < count; ++i) bits[old[i] >> 6] |= 1ULL << (old[i] & 63);
            bits[doc >> 6] |= 1ULL << (doc & 63);
            free(data); data = bits; capacity = ~0u;
        } else {
            if (count == capacity) {
                uint32_t next = std::min<uint32_t>(capacity ? capacity * 2 : 8, words * 4);
                void* grown = realloc(data, size_t(next) * sizeof(uint16_t));
                if (!grown) _exit(1);
                data = grown; capacity = next;
            }
            ((uint16_t*)data)[count] = doc;
        }
        ++count;
    }
};


template<class F> void par(size_t W, F f) {
    std::vector<std::thread> th;
    for (size_t i = 1; i < W; ++i) th.emplace_back(f, i);
    f(0);
    for (auto& x : th) x.join();
}

int main(int ac, char** av) {
    const char* dd = ac >= 2 ? av[1] : "/data", *od = ac >= 3 ? av[2] : "/out";
    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        size_t v;
        auto r = std::from_chars(s, s + strlen(s), v);
        if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 16) W = v;
    }

    MF qf(std::string(dd) + "/queries.txt");
    const char* qp = qf.d, * qe = qp + qf.s;
    Dict dt;
    std::vector<QItem> qs;
    qs.reserve(50000);

    while (qp < qe) {
        while (qp < qe && (*qp == '\n' || *qp == '\r')) ++qp;
        if (qp >= qe) break;
        const char* ls = qp;
        while (qp < qe && *qp != '\n') ++qp;
        std::string_view l(ls, qp - ls);
        if (qp < qe) ++qp;

        if (l.starts_with("BOOL ")) {
            QItem q; size_t p = 5; bool eo = false;
            while (p < l.size()) {
                size_t sp = l.find(' ', p);
                if (sp == std::string_view::npos) sp = l.size();
                std::string_view w = l.substr(p, sp - p);
                if (eo) {
                    q.op.push_back(w == "AND" ? Op::AND : w == "OR" ? Op::OR : Op::NOT);
                    eo = false;
                } else {
                    q.t.push_back(dt.in(w));
                    eo = true;
                }
                p = sp + 1;
            }
            qs.push_back(std::move(q));
        } else if (l.starts_with("PHRASE \"")) {
            QItem q; q.ph = true;
            size_t p = 8, qe_ = l.rfind('"');
            if (qe_ == std::string_view::npos) qe_ = l.size();
            while (p < qe_) {
                size_t sp = l.find(' ', p);
                if (sp == std::string_view::npos || sp > qe_) sp = qe_;
                std::string_view w = l.substr(p, sp - p);
                q.t.push_back(dt.in(w));
                q.rw.push_back(w);
                p = sp + 1;
            }
            qs.push_back(std::move(q));
        }
    }

    size_t NT = dt.count;
    MF df(std::string(dd) + "/docs.txt");
    const char* dp = df.d, * de = dp + df.s;
    const char* last = de;
    while (last > dp && *(last - 1) == '\n') --last;
    while (last > dp && *(last - 1) != '\n') --last;
    uint32_t ND = 0;
    while (last < de && *last >= '0' && *last <= '9') ND = ND * 10 + (*last++ - '0');
    ND++;
    W = std::min<size_t>(W, ND);
    size_t NW = (ND + 63) / 64;
    std::vector<DocRef> dr(ND);

    std::vector<const char*> bn(W + 1, de);
    bn[0] = dp;
    for (size_t i = 1; i < W; ++i) {
        const char* p = dp + df.s * i / W;
        while (p > dp && p < de && *(p - 1) != '\n') ++p;
        while (p < de) {
            const char* q = p;
            uint32_t id = 0;
            while (q < de && *q >= '0' && *q <= '9') id = id * 10 + (*q++ - '0');
            if (!(id & 63)) break;
            const char* nl = (const char*)memchr(p, '\n', de - p);
            p = nl ? nl + 1 : de;
        }
        bn[i] = p;
    }

    static constexpr uint32_t BLOCK_SHIFT = 15, DENSE = 2048;
    static constexpr size_t BLOCK_DOCS = size_t(1) << BLOCK_SHIFT;
    static constexpr size_t BLOCK_WORDS = BLOCK_DOCS / 64;
    size_t NB = (size_t(ND) + BLOCK_DOCS - 1) >> BLOCK_SHIFT;
    par(W, [&](size_t wid) {
        const char* sc = bn[wid], *end = bn[wid + 1];
        while (sc < end) {
            uint32_t id = 0;
            while (*sc >= '0' && *sc <= '9') id = id * 10 + (*sc++ - '0');
            ++sc;
            const char* text = sc;
            const char* nl = (const char*)memchr(sc, '\n', end - sc);
            if (!nl) nl = end;
            dr[id] = {text, uint32_t(nl - text)};
            sc = nl < end ? nl + 1 : end;
        }
    });
    std::vector<PostingBlock> blocks(NT * NB);
    std::atomic<size_t> next_block{0};
    par(W, [&](size_t) {
        std::vector<uint32_t> last_doc(NT, ~0u);
        TokenCache cache;
        for (size_t b = next_block.fetch_add(1); b < NB; b = next_block.fetch_add(1)) {
            uint32_t base = uint32_t(b << BLOCK_SHIFT);
            uint32_t end = uint32_t(std::min<size_t>(ND, size_t(base) + BLOCK_DOCS));
            uint32_t words = (end - base + 63) / 64;
            PostingBlock* local = blocks.data() + b * NT;
            for (uint32_t id = base; id < end; ++id) {
                const auto& doc = dr[id];
                for_each_token(doc.p, doc.p + doc.l, [&](const char* token, size_t length) {
                    if (length > 32) return;
                    uint32_t term = cache.lookup(dt, dp, token, length);
                    if (term == ~0u || last_doc[term] == id) return;
                    last_doc[term] = id;
                    local[term].add(uint16_t(id - base), words);
                });
            }
        }
    });
    std::vector<uint32_t> df_cnt(NT);
    for (size_t b = 0; b < NB; ++b)
        for (size_t t = 0; t < NT; ++t) df_cnt[t] += blocks[b * NT + t].count;
    const PostingBlock empty{};
    uint16_t zero = 0;
    auto get_sparse = [&](const PostingBlock& block) { return block.data ? block.small() : &zero; };
    auto get_block = [&](uint32_t term, size_t b) -> const PostingBlock& {
        return term == ~0u ? empty : blocks[b * NT + term];
    };
    auto phrase_matches = [&](uint32_t doc, const QItem& query) {
        const auto& words = query.rw;
        const char* p = dr[doc].p, *end = p + dr[doc].l;
        while (p < end) {
            while (p < end && !CHAR_MAP[uint8_t(*p)]) ++p;
            const char* word = p;
            while (p < end && CHAR_MAP[uint8_t(*p)]) ++p;
            if (size_t(p - word) != words[0].size() || !same_word(word, words[0].data(), words[0].size())) continue;
            const char* next = p;
            size_t i = 1;
            for (; i < words.size(); ++i) {
                while (next < end && !CHAR_MAP[uint8_t(*next)]) ++next;
                word = next;
                while (next < end && CHAR_MAP[uint8_t(*next)]) ++next;
                if (size_t(next - word) != words[i].size() || !same_word(word, words[i].data(), words[i].size())) break;
            }
            if (i == words.size()) return true;
        }
        return false;
    };
    size_t NQ = qs.size();
    std::vector<std::string> res(NQ);
    std::atomic<size_t> qc{0};
    par(W, [&](size_t) {
        std::vector<uint64_t> cb(BLOCK_WORDS);
        std::vector<uint16_t> cur, nxt;
        cur.reserve(DENSE); nxt.reserve(DENSE);
        for (size_t q = qc.fetch_add(1); q < NQ; q = qc.fetch_add(1)) {
            const auto& query = qs[q];
            uint32_t count = 0, first[10], nfirst = 0;
            uint64_t sum = 0;
            auto add = [&](uint32_t doc) { ++count; sum += doc; if (nfirst < 10) first[nfirst++] = doc; };
            auto summarize = [&](uint64_t mask, uint32_t base) {
                unsigned bits = __builtin_popcountll(mask);
                if (bits <= 8) {
                    while (mask) { add(base + __builtin_ctzll(mask)); mask &= mask - 1; }
                    return;
                }
                count += bits;
                sum += uint64_t(base) * bits
                    + __builtin_popcountll(mask & 0xaaaaaaaaaaaaaaaaULL)
                    + 2ULL * __builtin_popcountll(mask & 0xccccccccccccccccULL)
                    + 4ULL * __builtin_popcountll(mask & 0xf0f0f0f0f0f0f0f0ULL)
                    + 8ULL * __builtin_popcountll(mask & 0xff00ff00ff00ff00ULL)
                    + 16ULL * __builtin_popcountll(mask & 0xffff0000ffff0000ULL)
                    + 32ULL * __builtin_popcountll(mask & 0xffffffff00000000ULL);
                while (mask && nfirst < 10) { first[nfirst++] = base + __builtin_ctzll(mask); mask &= mask - 1; }
            };
            std::array<size_t, 16> order{};
            bool absent = false;
            if (query.ph) {
                for (size_t i = 0; i < query.t.size(); ++i) {
                    order[i] = i;
                    if (query.t[i] == ~0u || !df_cnt[query.t[i]]) absent = true;
                }
                if (!absent) std::sort(order.begin(), order.begin() + query.t.size(), [&](size_t a, size_t b) { return df_cnt[query.t[a]] < df_cnt[query.t[b]]; });
            }
            for (size_t b = 0; b < NB && !absent; ++b) {
                uint32_t base = uint32_t(b << BLOCK_SHIFT);
                size_t words = std::min<size_t>(BLOCK_WORDS, NW - b * BLOCK_WORDS);
                const auto& initial = get_block(query.t[query.ph ? order[0] : 0], b);
                bool bitmap = initial.bitmap();
                cur.clear();
                if (bitmap) std::memcpy(cb.data(), initial.bits(), words * 8);
                else if (initial.count) cur.assign(get_sparse(initial), get_sparse(initial) + initial.count);
                if (query.ph) {
                    if (bitmap) {
                        for (size_t w = 0; w < words; ++w) {
                            uint64_t m = cb[w];
                            while (m) { cur.push_back(uint16_t(w * 64 + __builtin_ctzll(m))); m &= m - 1; }
                        }
                    }
                    for (size_t i = 1; i < query.t.size() && !cur.empty(); ++i) {
                        const auto& operand = get_block(query.t[order[i]], b);
                        if (!operand.count) { cur.clear(); break; }
                        size_t out = 0;
                        if (operand.bitmap()) {
                            const uint64_t* bits = operand.bits();
                            for (uint16_t doc : cur) if ((bits[doc >> 6] >> (doc & 63)) & 1) cur[out++] = doc;
                        } else {
                            const uint16_t* p = get_sparse(operand), *end = p + operand.count;
                            for (uint16_t doc : cur) {
                                p = std::lower_bound(p, end, doc);
                                if (p == end) break;
                                if (*p == doc) cur[out++] = doc;
                            }
                        }
                        cur.resize(out);
                    }
                    for (uint16_t local : cur) if (phrase_matches(base + local, query)) add(base + local);
                    continue;
                }
                for (size_t i = 0; i < query.op.size(); ++i) {
                    Op op = query.op[i];
                    const auto& operand = get_block(query.t[i + 1], b);
                    if (operand.bitmap()) {
                        const uint64_t* bits = operand.bits();
                        if (bitmap) {
                            if (op == Op::AND) apply_words<Op::AND>(cb.data(), bits, words);
                            else if (op == Op::OR) apply_words<Op::OR>(cb.data(), bits, words);
                            else apply_words<Op::NOT>(cb.data(), bits, words);
                        } else if (op == Op::OR) {
                            std::memcpy(cb.data(), bits, words * 8);
                            for (uint16_t doc : cur) cb[doc >> 6] |= 1ULL << (doc & 63);
                            bitmap = true;
                        } else {
                            size_t out = 0;
                            for (uint16_t doc : cur) {
                                bool present = (bits[doc >> 6] >> (doc & 63)) & 1;
                                if (present == (op == Op::AND)) cur[out++] = doc;
                            }
                            cur.resize(out);
                        }
                        continue;
                    }
                    const uint16_t* p = get_sparse(operand);
                    size_t size = operand.count;
                    if (!bitmap && op == Op::OR && cur.size() + size > DENSE) {
                        std::fill(cb.begin(), cb.begin() + words, 0);
                        for (uint16_t doc : cur) cb[doc >> 6] |= 1ULL << (doc & 63);
                        bitmap = true;
                    }
                    if (bitmap) {
                        if (op == Op::AND) {
                            cur.clear();
                            for (size_t k = 0; k < size; ++k) if ((cb[p[k] >> 6] >> (p[k] & 63)) & 1) cur.push_back(p[k]);
                            bitmap = false;
                        } else if (op == Op::OR) {
                            for (size_t k = 0; k < size; ++k) cb[p[k] >> 6] |= 1ULL << (p[k] & 63);
                        } else {
                            for (size_t k = 0; k < size; ++k) cb[p[k] >> 6] &= ~(1ULL << (p[k] & 63));
                        }
                    } else {
                        nxt.clear();
                        size_t a = 0, k = 0;
                        if (op == Op::AND) {
                            while (a < cur.size() && k < size) {
                                if (cur[a] == p[k]) { nxt.push_back(cur[a]); ++a; ++k; }
                                else if (cur[a] < p[k]) ++a;
                                else ++k;
                            }
                        } else if (op == Op::OR) {
                            while (a < cur.size() && k < size) {
                                if (cur[a] == p[k]) { nxt.push_back(cur[a]); ++a; ++k; }
                                else if (cur[a] < p[k]) nxt.push_back(cur[a++]);
                                else nxt.push_back(p[k++]);
                            }
                            while (a < cur.size()) nxt.push_back(cur[a++]);
                            while (k < size) nxt.push_back(p[k++]);
                        } else {
                            while (a < cur.size() && k < size) {
                                if (cur[a] == p[k]) { ++a; ++k; }
                                else if (cur[a] < p[k]) nxt.push_back(cur[a++]);
                                else ++k;
                            }
                            while (a < cur.size()) nxt.push_back(cur[a++]);
                        }
                        cur.swap(nxt);
                    }
                }
                if (bitmap) {
                    for (size_t w = 0; w < words; ++w) if (cb[w]) summarize(cb[w], base + uint32_t(w * 64));
                } else for (uint16_t doc : cur) add(base + doc);
            }
            char buf[32];
            auto& result = res[q];
            auto number = [&](uint64_t value) { result.append(buf, std::to_chars(buf, buf + 32, value).ptr); };
            number(count); result += ' '; number(sum);
            for (uint32_t i = 0; i < nfirst; ++i) { result += ' '; number(first[i]); }
        }
    });
    std::string out;
    out.reserve(NQ * 32);
    for (const auto& result : res) { out += result; out += '\n'; }
    int ofd = open((std::string(od) + "/results.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = write(ofd, out.data() + sent, out.size() - sent);
        if (n <= 0) _exit(1);
        sent += size_t(n);
    }
    close(ofd);
    _exit(0);
}
