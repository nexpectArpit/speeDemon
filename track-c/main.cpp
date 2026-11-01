#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

static constexpr size_t G_SIZE = 262144;
static constexpr uint32_t G_MASK = G_SIZE - 1;
static constexpr size_t MAX_COUNTRIES = 256;
static constexpr size_t BITSET_WORDS = G_SIZE / 64; // 4096 words

struct Agg {
    uint64_t count = 0;
    int64_t id_sum = 0, amt_sum = 0, amt_min = INT64_MAX, amt_max = INT64_MIN;
    uint64_t name_bytes = 0, note_bytes = 0;
};

alignas(64) static std::atomic<uint64_t> g_hash_keys[G_SIZE];
alignas(64) static std::atomic<uint64_t> g_country_bits[MAX_COUNTRIES][BITSET_WORDS];
static uint16_t r2c[1000000];

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
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ uint8_t(p[i])) * 1099511628211ULL;
    }
    return h ? h : 1ULL;
#endif
}

// Slot index (0..262143) is directly the unique name ID!
inline uint32_t get_name_slot(uint64_t h) {
    uint32_t idx = uint32_t(h ^ (h >> 32)) & G_MASK;
    while (true) {
        uint64_t cur = g_hash_keys[idx].load(std::memory_order_relaxed);
        if (cur == h) return idx;
        if (cur == 0) {
            uint64_t expected = 0;
            if (g_hash_keys[idx].compare_exchange_strong(expected, h, std::memory_order_relaxed)) {
                return idx;
            }
            if (expected == h) return idx;
        }
        idx = (idx + 1) & G_MASK;
    }
}

inline void write_u64(std::string& o, uint64_t v) {
    char b[24];
    o.append(b, std::to_chars(b, b + 24, v).ptr);
}

inline void write_cents(std::string& o, int64_t c) {
    if (c < 0) { o += '-'; c = -c; }
    write_u64(o, uint64_t(c / 100));
    uint64_t f = uint64_t(c % 100);
    o += '.';
    o += char('0' + f / 10);
    o += char('0' + f % 10);
}

int main(int ac, char** av) {
    const char* dd = ac == 3 ? av[1] : "/data";
    const char* od = ac == 3 ? av[2] : "/out";

    int rfd = open((std::string(dd) + "/regions.csv").c_str(), O_RDONLY);
    struct stat rst{};
    fstat(rfd, &rst);
    const char* rp = (const char*)mmap(nullptr, rst.st_size, PROT_READ, MAP_PRIVATE, rfd, 0);
    close(rfd);

    memset(r2c, 0xFF, sizeof(r2c));
    std::vector<std::string> cnames;
    cnames.reserve(256);
    {
        struct CEnt { uint32_t h; uint16_t id; };
        CEnt ctb[1024]{};
        auto get_c = [&](const char* s, uint16_t l) -> uint16_t {
            uint32_t h = 2166136261u;
            for (uint16_t i = 0; i < l; ++i) h = (h ^ uint8_t(s[i])) * 16777619u;
            uint32_t x = h & 1023;
            while (ctb[x].h) {
                if (ctb[x].h == h && cnames[ctb[x].id].size() == l && !memcmp(cnames[ctb[x].id].data(), s, l))
                    return ctb[x].id;
                x = (x + 1) & 1023;
            }
            uint16_t id = uint16_t(cnames.size());
            cnames.emplace_back(s, l);
            ctb[x] = {h, id};
            return id;
        };
        const char* p = rp;
        const char* end = rp + rst.st_size;
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
        while (p < end) {
            uint32_t reg = 0;
            while (p < end && *p >= '0' && *p <= '9') reg = reg * 10 + (*p++ - '0');
            if (p >= end || *p != ',') break;
            ++p;
            const char* cs = p;
            while (p < end && *p != '\n' && *p != '\r') ++p;
            uint16_t clen = uint16_t(p - cs);
            if (p < end && *p == '\r') ++p;
            if (p < end) ++p;
            uint16_t cid = get_c(cs, clen);
            if (reg < 1000000) r2c[reg] = cid;
        }
    }
    uint16_t uid = uint16_t(cnames.size());
    cnames.emplace_back("_UNMATCHED");
    size_t NC = cnames.size();

    int fd = open((std::string(dd) + "/records.csv").c_str(), O_RDONLY);
    struct stat st{};
    fstat(fd, &st);
    size_t sz = st.st_size;
    const char* d = (const char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    size_t fs = (sz >= 3 && uint8_t(d[0]) == 0xEF && uint8_t(d[1]) == 0xBB && uint8_t(d[2]) == 0xBF) ? 3 : 0;
    while (fs < sz && d[fs] != '\n') ++fs;
    if (fs < sz) ++fs;

    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        size_t v;
        auto r = std::from_chars(s, s + strlen(s), v);
        if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 12) W = v;
    }
    if (sz - fs < 65536) W = 1;

    std::vector<const char*> bn(W + 1, d + sz);
    bn[0] = d + fs;
    for (size_t i = 1; i < W; ++i) {
        size_t pos = fs + (sz - fs) * i / W;
        bool found = false;
        while (pos < sz) {
            const char* nl = (const char*)memchr(d + pos, '\n', sz - pos);
            if (!nl) break;
            pos = size_t(nl - d) + 1;
            size_t p = pos;
            if (p + 3 <= sz && uint8_t(d[p]) == 0xEF && uint8_t(d[p + 1]) == 0xBB && uint8_t(d[p + 2]) == 0xBF) p += 3;
            size_t q = p;
            while (q < sz && d[q] >= '0' && d[q] <= '9') ++q;
            if (q > p && q < sz && d[q] == ',') {
                size_t pr = pos - 1;
                if (pr > 0 && d[pr - 1] == '\r') --pr;
                if (pr >= 3 && d[pr - 3] == '.' && d[pr - 2] >= '0' && d[pr - 2] <= '9' && d[pr - 1] >= '0' && d[pr - 1] <= '9') {
                    bn[i] = d + pos;
                    found = true;
                    break;
                }
            }
        }
        if (!found) bn[i] = d + sz;
    }
    for (size_t i = 1; i < W; ++i) if (bn[i] < bn[i - 1]) bn[i] = bn[i - 1];

    uint32_t empty_slot = get_name_slot(hash_bytes("", 0));
    std::vector<std::vector<Agg>> sh(W, std::vector<Agg>(NC));

    auto worker = [&](size_t wid) {
        auto& ag = sh[wid];
        struct L1 { uint64_t h = 0; uint32_t nid = 0; };
        std::vector<L1> l1(32768);
        const char* c = bn[wid];
        const char* end = bn[wid + 1];
        while (c < end) {
            if (uint8_t(c[0]) == 0xEF && uint8_t(c[1]) == 0xBB && uint8_t(c[2]) == 0xBF) c += 3;
            uint64_t id = 0;
            while (*c >= '0' && *c <= '9') id = id * 10 + (*c++ - '0');
            ++c; // ','

            uint32_t name_len = 0;
            uint32_t nid = empty_slot;
            if (*c == '"') {
                ++c;
                const char* name_start = c;
                uint32_t esc_cnt = 0;
                while (true) {
                    const char* q = (const char*)memchr(c, '"', end - c);
                    if (q[1] == '"') { ++esc_cnt; c = q + 2; }
                    else { c = q; break; }
                }
                uint32_t raw_len = uint32_t(c - name_start);
                name_len = raw_len - esc_cnt;
                if (raw_len > 0) {
                    uint64_t nh = hash_bytes(name_start, raw_len);
                    uint32_t lidx = uint32_t(nh ^ (nh >> 15)) & 32767;
                    if (l1[lidx].h == nh) {
                        nid = l1[lidx].nid;
                    } else {
                        nid = get_name_slot(nh);
                        l1[lidx] = {nh, nid};
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
                    const char* q = (const char*)memchr(c, '"', end - c);
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
            uint16_t cid = (reg < 1000000) ? r2c[reg] : uid;
            if (cid == 65535) cid = uid;

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
            if (!(word.load(std::memory_order_relaxed) & mask)) {
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

    std::vector<uint16_t> sc;
    for (uint16_t k = 0; k < NC; ++k) if (ta[k].count) sc.push_back(k);
    std::sort(sc.begin(), sc.end(), [&](uint16_t a, uint16_t b) { return cnames[a] < cnames[b]; });

    std::string out;
    out.reserve(sc.size() * 120);
    for (uint16_t k : sc) {
        uint64_t nd = 0;
        for (size_t w = 0; w < BITSET_WORDS; ++w) {
            nd += __builtin_popcountll(g_country_bits[k][w].load(std::memory_order_relaxed));
        }
        const Agg& a = ta[k];
        out += cnames[k]; out += ' ';
        write_u64(out, a.count); out += ' ';
        write_u64(out, a.id_sum); out += ' ';
        write_cents(out, a.amt_sum); out += ' ';
        write_cents(out, a.amt_min); out += ' ';
        write_cents(out, a.amt_max); out += ' ';
        write_u64(out, nd); out += ' ';
        write_u64(out, a.name_bytes); out += ' ';
        write_u64(out, a.note_bytes); out += '\n';
    }

    std::string out_path = std::string(od) + "/aggregates.txt";
    int ofd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);
    _exit(0);
}
