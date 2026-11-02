#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr size_t G_SIZE = 262144;
static constexpr uint32_t G_MASK = G_SIZE - 1;
static constexpr size_t MAX_COUNTRIES = 242;
static constexpr size_t BITSET_WORDS = G_SIZE / 64; // 4096 words

struct Agg {
    uint64_t count = 0;
    int64_t id_sum = 0, amt_sum = 0;
    int64_t amt_min = 0x7FFFFFFFFFFFFFFFLL, amt_max = -0x7FFFFFFFFFFFFFFFLL;
    uint64_t name_bytes = 0, note_bytes = 0;
};

alignas(64) static std::atomic<uint64_t> g_hash_keys[G_SIZE];
alignas(64) static const char* g_name_ptr[G_SIZE];
alignas(64) static uint32_t g_name_id[G_SIZE];
static std::atomic<uint32_t> g_name_count{0};
alignas(64) static std::atomic<uint32_t> g_name_len[G_SIZE];
alignas(64) static std::atomic<uint64_t> g_country_bits[MAX_COUNTRIES][BITSET_WORDS];
static uint8_t r2c[1000000];

inline uint64_t hash_bytes(const char* p, size_t len) {
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t h1 = len ^ 0x9e3779b97f4a7c15ULL;
    uint64_t h2 = len * 0xbf58476d1ce4e5b9ULL;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h1 = _mm_crc32_u64(h1, v);
        h2 = (h2 ^ v) * 0x94d049bb133111ebULL;
    }
    for (; i < len; ++i) {
        uint8_t c = uint8_t(p[i]);
        h1 = _mm_crc32_u8(uint32_t(h1), c);
        h2 = (h2 ^ c) * 0x94d049bb133111ebULL;
    }
    uint64_t res = (h1 << 32) ^ h2 ^ (len << 48);
    return res ? res : 1ULL;
#else
    uint64_t h = len ^ 0x9e3779b97f4a7c15ULL;
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ uint8_t(p[i])) * 0xbf58476d1ce4e5b9ULL;
    }
    return h ? h : 1ULL;
#endif
}

struct NameArena {
    std::vector<std::unique_ptr<char[]>> blocks;
    size_t used = 0, capacity = 0;
    const char* copy(const char* s, uint32_t len) {
        if (!len) return "";
        if (capacity - used < len) {
            capacity = std::max<size_t>(65536, len);
            blocks.emplace_back(new char[capacity]);
            used = 0;
        }
        char* p = blocks.back().get() + used;
        memcpy(p, s, len);
        used += len;
        return p;
    }
};

struct QuoteCursor {
    const char* scan;
    const char* end;
#if defined(__AVX2__)
    const char* base = nullptr;
    uint32_t mask = 0;
#endif
    const char* next(const char* from) {
#if defined(__AVX2__)
        if (from >= scan) { scan = from; mask = 0; }
        else mask &= ~0u << unsigned(from - base);
        while (!mask) {
            if (end - scan < 32) {
                const char* q = (const char*)memchr(scan, '"', end - scan);
                scan = q ? q + 1 : end;
                return q;
            }
            base = scan;
            __m256i v = _mm256_loadu_si256((const __m256i*)scan);
            mask = uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('"'))));
            scan += 32;
        }
        unsigned bit = __builtin_ctz(mask);
        mask &= mask - 1;
        return base + bit;
#else
        return (const char*)memchr(from, '"', end - from);
#endif
    }
};

inline uint32_t get_name_slot(const char* s, uint32_t len, uint64_t h, NameArena& arena) {
    uint32_t idx = uint32_t(h ^ (h >> 32)) & G_MASK;
    for (;;) {
        uint64_t cur = g_hash_keys[idx].load(std::memory_order_relaxed);
        if (!cur) {
            uint64_t expected = 0;
            if (g_hash_keys[idx].compare_exchange_strong(expected, h, std::memory_order_relaxed)) {
                g_name_ptr[idx] = arena.copy(s, len);
                g_name_id[idx] = g_name_count.fetch_add(1, std::memory_order_relaxed);
                g_name_len[idx].store(len + 1, std::memory_order_release);
                return idx;
            }
            cur = expected;
        }
        if (cur == h) {
            uint32_t ready;
            while (!(ready = g_name_len[idx].load(std::memory_order_acquire))) {}
            if (ready == len + 1 && !memcmp(g_name_ptr[idx], s, len)) return idx;
        }
        idx = (idx + 1) & G_MASK;
    }
}

int main(int ac, char** av) {
    const char* dd = ac == 3 ? av[1] : "/data";
    const char* od = ac == 3 ? av[2] : "/out";
    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        int v = atoi(s);
        if (v >= 1 && v <= 12) W = v;
    }

    std::memset(r2c, 0xFF, sizeof(r2c));

    std::vector<std::string> cnames;
    cnames.reserve(256);
    {
        struct CEnt { uint32_t h; uint8_t id; };
        CEnt ctb[1024]{};
        auto get_c = [&](const char* s, uint16_t l) -> uint8_t {
            uint32_t h = 2166136261u;
            for (uint16_t i = 0; i < l; ++i) h = (h ^ uint8_t(s[i])) * 16777619u;
            uint32_t x = h & 1023;
            while (ctb[x].h) {
                if (ctb[x].h == h && cnames[ctb[x].id] == std::string_view(s, l))
                    return ctb[x].id;
                x = (x + 1) & 1023;
            }
            uint8_t id = uint8_t(cnames.size());
            cnames.emplace_back(s, l);
            ctb[x] = {h, id};
            return id;
        };

        int fd = open((std::string(dd) + "/regions.csv").c_str(), O_RDONLY);
        struct stat st;
        fstat(fd, &st);
        const char* p = (char*)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        const char* end = p + st.st_size;
        close(fd);

        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
        while (p < end) {
            uint32_t reg = 0;
            while (*p >= '0' && *p <= '9') reg = reg * 10 + (*p++ - '0');
            ++p; // ','
            const char* cs = p;
            while (p < end && *p != '\n' && *p != '\r') ++p;
            uint16_t clen = uint16_t(p - cs);
            if (p < end && *p == '\r') ++p;
            if (p < end) ++p;
            uint8_t cid = get_c(cs, clen);
            if (reg < 1000000) r2c[reg] = cid;
        }
    }
    uint8_t uid = uint8_t(cnames.size());
    cnames.emplace_back("_UNMATCHED");
    size_t NC = cnames.size();

    int fd = open((std::string(dd) + "/records.csv").c_str(), O_RDONLY);
    struct stat st;
    fstat(fd, &st);
    size_t sz = st.st_size;
    const char* d = (char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    const char* p0 = d;
    while (p0 < d + sz && *p0 != '\n') ++p0;
    if (p0 < d + sz) ++p0;

    std::vector<const char*> bn(W + 1);
    bn[0] = p0;
    bn[W] = d + sz;

    for (size_t i = 1; i < W; ++i) {
        const char* p = d + sz * i / W;
        bn[i] = d + sz;
        while (p < d + sz) {
            const char* nl = (const char*)memchr(p, '\n', d + sz - p);
            if (!nl) break;
            const char* start = nl + 1;
            p = start;
            if (d + sz - p >= 3 && uint8_t(p[0]) == 0xef && uint8_t(p[1]) == 0xbb && uint8_t(p[2]) == 0xbf) p += 3;
            const char* digits = p;
            while (p < d + sz && *p >= '0' && *p <= '9') ++p;
            if (p > digits && d + sz - p >= 2 && *p == ',' && (p[1] == '"' || p[1] == ',')) {
                bn[i] = start;
                break;
            }
        }
    }
    std::vector<NameArena> arenas(W);
    uint32_t empty_slot = get_name_slot("", 0, hash_bytes("", 0), arenas[0]);
    uint32_t empty_id = g_name_id[empty_slot];
    std::vector<std::vector<Agg>> sh(W, std::vector<Agg>(NC));

    auto worker = [&](size_t wid) {
        auto& ag = sh[wid];
        struct L1Entry { uint64_t hash = 0; const char* p = nullptr; uint32_t len = 0; uint32_t slot = 0; };
        L1Entry l1[4096]{};
        const char* c = bn[wid];
        const char* end = bn[wid + 1];
        QuoteCursor quotes{c, end};
        while (c < end) {
            if (uint8_t(c[0]) == 0xEF && uint8_t(c[1]) == 0xBB && uint8_t(c[2]) == 0xBF) c += 3;
            uint64_t id = 0;
            while (*c >= '0' && *c <= '9') id = id * 10 + (*c++ - '0');
            ++c; // ','

            uint32_t name_len = 0;
            uint32_t nid = empty_id;
            if (*c == '"') {
                ++c;
                const char* name_start = c;
                uint32_t esc_cnt = 0;
                while (true) {
                    const char* q = quotes.next(c);
                    if (q[1] == '"') { ++esc_cnt; c = q + 2; }
                    else { c = q; break; }
                }
                uint32_t raw_len = uint32_t(c - name_start);
                name_len = raw_len - esc_cnt;
                if (raw_len > 0) {
                    uint64_t nh = hash_bytes(name_start, raw_len);
                    uint32_t lidx = uint32_t(nh ^ (nh >> 12)) & 4095;
                    if (l1[lidx].hash == nh && l1[lidx].len == raw_len && !memcmp(l1[lidx].p, name_start, raw_len)) {
                        nid = l1[lidx].slot;
                    } else {
                        uint32_t slot = get_name_slot(name_start, raw_len, nh, arenas[wid]);
                        nid = g_name_id[slot];
                        l1[lidx] = {nh, g_name_ptr[slot], raw_len, nid};
                    }
                }
                c += 2; // skip '"' and ','
            } else {
                ++c; // empty unquoted name, skip ','
            }

            uint32_t note_len = 0;
            if (*c == '"') {
                ++c;
                while (true) {
                    const char* q = quotes.next(c);
                    note_len += uint32_t(q - c);
                    if (q[1] == '"') { ++note_len; c = q + 2; }
                    else { c = q + 1; break; }
                }
                ++c; // skip ','
            } else {
                ++c; // empty unquoted note, skip ','
            }

            uint32_t reg = 0;
            while (*c >= '0' && *c <= '9') reg = reg * 10 + (*c++ - '0');
            ++c; // ','
            uint8_t cid = (reg < 1000000) ? r2c[reg] : uid;
            if (cid == 0xFF) cid = uid;

            bool neg = (*c == '-');
            if (neg) ++c;
            int64_t cts = 0;
            while (*c >= '0' && *c <= '9') cts = cts * 10 + (*c++ - '0');
            ++c; // '.'
            cts = cts * 100 + (c[0] - '0') * 10 + (c[1] - '0');
            c += 2;
            if (neg) cts = -cts;
            if (*c == '\r') ++c;
            if (*c == '\n') ++c;

            uint64_t mask = 1ULL << (nid & 63);
            auto& word = g_country_bits[cid][nid >> 6];
            uint64_t cur_val = word.load(std::memory_order_relaxed);
            if (!(cur_val & mask)) {
                word.fetch_or(mask, std::memory_order_relaxed);
            }
            Agg& a = ag[cid];
            a.count++;
            a.id_sum += int64_t(id);
            a.amt_sum += cts;
            if (cts < a.amt_min) a.amt_min = cts;
            if (cts > a.amt_max) a.amt_max = cts;
            a.name_bytes += name_len;
            a.note_bytes += note_len;
        }
    };

    std::vector<std::thread> th;
    for (size_t i = 1; i < W; ++i) th.emplace_back(worker, i);
    worker(0);
    for (auto& t : th) t.join();

    std::vector<Agg> ta(NC);
    for (size_t k = 0; k < NC; ++k) {
        for (size_t w = 0; w < W; ++w) {
            const Agg& sa = sh[w][k];
            if (!sa.count) continue;
            ta[k].count += sa.count;
            ta[k].id_sum += sa.id_sum;
            ta[k].amt_sum += sa.amt_sum;
            if (sa.amt_min < ta[k].amt_min) ta[k].amt_min = sa.amt_min;
            if (sa.amt_max > ta[k].amt_max) ta[k].amt_max = sa.amt_max;
            ta[k].name_bytes += sa.name_bytes;
            ta[k].note_bytes += sa.note_bytes;
        }
    }

    std::vector<uint8_t> sc;
    for (uint8_t k = 0; k < NC; ++k) if (ta[k].count) sc.push_back(k);
    std::sort(sc.begin(), sc.end(), [&](uint8_t a, uint8_t b) { return cnames[a] < cnames[b]; });

    std::string out;
    out.reserve(sc.size() * 120);
    for (uint8_t k : sc) {
        uint64_t nd = 0;
        for (size_t w = 0, words = (g_name_count.load(std::memory_order_relaxed) + 63) / 64; w < words; ++w) {
            nd += __builtin_popcountll(g_country_bits[k][w].load(std::memory_order_relaxed));
        }
        char buf[256];
        const auto& a = ta[k];
        auto pfmt = [](int64_t v, char* b) -> char* {
            if (v < 0) { *b++ = '-'; v = -v; }
            int64_t w = v / 100, f = v % 100;
            b += sprintf(b, "%lld.%02lld", (long long)w, (long long)f);
            return b;
        };
        char* p = buf + sprintf(buf, "%s %llu %lld ", cnames[k].c_str(), (unsigned long long)a.count, (long long)a.id_sum);
        p = pfmt(a.amt_sum, p); *p++ = ' ';
        p = pfmt(a.amt_min, p); *p++ = ' ';
        p = pfmt(a.amt_max, p); *p++ = ' ';
        p += sprintf(p, "%llu %llu %llu\n", (unsigned long long)nd, (unsigned long long)a.name_bytes, (unsigned long long)a.note_bytes);
        out.append(buf, p - buf);
    }

    int ofd = open((std::string(od) + "/aggregates.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);

    _exit(0);
}
