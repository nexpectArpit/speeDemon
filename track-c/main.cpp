#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
static constexpr size_t BITSET_WORDS = 2048;

struct Agg {
    uint64_t count = 0;
    int64_t id_sum = 0, amt_sum = 0, amt_min = INT64_MAX, amt_max = INT64_MIN;
    uint64_t name_bytes = 0, note_bytes = 0;
};

alignas(64) static std::atomic<uint64_t> g_hash_keys[G_SIZE];
alignas(64) static std::atomic<uint32_t> g_hash_ids[G_SIZE];
alignas(64) static std::atomic<uint32_t> g_next_nid{1};
alignas(64) static std::atomic<uint64_t> g_country_bits[MAX_COUNTRIES][BITSET_WORDS];

inline uint32_t get_name_id(uint64_t h) {
    if (h == 0) h = 1;
    uint32_t idx = uint32_t(h) & G_MASK;
    while (true) {
        uint64_t cur = g_hash_keys[idx].load(std::memory_order_acquire);
        if (cur == h) {
            uint32_t id = g_hash_ids[idx].load(std::memory_order_relaxed);
            while (id == 0) id = g_hash_ids[idx].load(std::memory_order_acquire);
            return id - 1;
        }
        if (cur == 0) {
            uint64_t expected = 0;
            if (g_hash_keys[idx].compare_exchange_strong(expected, h, std::memory_order_acq_rel)) {
                uint32_t id = g_next_nid.fetch_add(1, std::memory_order_relaxed);
                g_hash_ids[idx].store(id, std::memory_order_release);
                return id - 1;
            }
            if (expected == h) {
                uint32_t id = g_hash_ids[idx].load(std::memory_order_relaxed);
                while (id == 0) id = g_hash_ids[idx].load(std::memory_order_acquire);
                return id - 1;
            }
        }
        idx = (idx + 1) & G_MASK;
    }
}

inline uint64_t hash_bytes(const char* p, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h = (h ^ uint8_t(p[i])) * 1099511628211ULL;
    }
    return h ^ (h >> 32) ^ (len << 48);
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

    std::vector<uint16_t> r2c(1000000, 65535);
    std::vector<std::string> cnames;
    cnames.reserve(256);
    {
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
            std::string_view cn(cs, p - cs);
            if (p < end && *p == '\r') ++p;
            if (p < end) ++p;
            uint16_t cid = 0;
            while (cid < cnames.size() && cnames[cid] != cn) ++cid;
            if (cid == cnames.size()) cnames.emplace_back(cn);
            if (reg >= r2c.size()) r2c.resize(std::max(size_t(reg + 1), r2c.size() * 2), 65535);
            r2c[reg] = cid;
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
            if (d[pos - 1] == '\n') {
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
            ++pos;
        }
        if (!found) bn[i] = d + sz;
    }
    for (size_t i = 1; i < W; ++i) if (bn[i] < bn[i - 1]) bn[i] = bn[i - 1];

    std::vector<std::vector<Agg>> sh(W, std::vector<Agg>(NC));
    auto worker = [&](size_t wid) {
        auto& ag = sh[wid];
        const char* c = bn[wid];
        const char* end = bn[wid + 1];
        while (c < end) {
            if (c + 3 <= end && uint8_t(c[0]) == 0xEF && uint8_t(c[1]) == 0xBB && uint8_t(c[2]) == 0xBF) c += 3;
            uint64_t id = 0;
            while (c < end && *c >= '0' && *c <= '9') id = id * 10 + (*c++ - '0');
            if (c < end && *c == ',') ++c;

            const char* name_start = c;
            uint32_t name_len = 0;
            uint32_t raw_len = 0;
            if (c < end && *c == '"') {
                ++c;
                name_start = c;
                uint32_t esc_cnt = 0;
                while (c < end) {
                    const char* q = (const char*)memchr(c, '"', end - c);
                    if (!q) { c = end; break; }
                    if (q + 1 < end && q[1] == '"') {
                        ++esc_cnt;
                        c = q + 2;
                    } else {
                        c = q;
                        break;
                    }
                }
                raw_len = uint32_t(c - name_start);
                name_len = raw_len - esc_cnt;
                if (c < end && *c == '"') ++c;
            } else {
                name_start = c;
                const char* cm = (const char*)memchr(c, ',', end - c);
                if (!cm) cm = end;
                raw_len = uint32_t(cm - c);
                name_len = raw_len;
                c = cm;
            }
            if (c < end && *c == ',') ++c;
            uint32_t nid = get_name_id(hash_bytes(name_start, raw_len));

            uint32_t note_len = 0;
            if (c < end && *c == '"') {
                ++c;
                while (c < end) {
                    const char* q = (const char*)memchr(c, '"', end - c);
                    if (!q) { note_len += uint32_t(end - c); c = end; break; }
                    note_len += uint32_t(q - c);
                    if (q + 1 < end && q[1] == '"') {
                        note_len++;
                        c = q + 2;
                    } else {
                        c = q + 1;
                        break;
                    }
                }
            } else {
                const char* cm = (const char*)memchr(c, ',', end - c);
                if (!cm) cm = end;
                note_len = uint32_t(cm - c);
                c = cm;
            }
            if (c < end && *c == ',') ++c;

            uint32_t reg = 0;
            while (c < end && *c >= '0' && *c <= '9') reg = reg * 10 + (*c++ - '0');
            if (c < end && *c == ',') ++c;
            uint16_t cid = (reg < r2c.size()) ? r2c[reg] : uid;
            if (cid == 65535) cid = uid;

            bool neg = (c < end && *c == '-');
            if (neg) ++c;
            int64_t cts = 0;
            while (c < end && *c >= '0' && *c <= '9') cts = cts * 10 + (*c++ - '0');
            if (c < end && *c == '.') {
                ++c;
                if (c < end) cts = cts * 10 + (*c++ - '0');
                if (c < end) cts = cts * 10 + (*c++ - '0');
            }
            if (neg) cts = -cts;
            if (c < end && *c == '\r') ++c;
            if (c < end && *c == '\n') ++c;

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

    if (W <= 1) {
        worker(0);
    } else {
        std::vector<std::thread> th;
        for (size_t i = 1; i < W; ++i) th.emplace_back(worker, i);
        worker(0);
        for (auto& t : th) t.join();
    }

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

    uint32_t total_words = (g_next_nid.load() + 63) >> 6;
    std::string out;
    out.reserve(sc.size() * 120);
    for (uint16_t k : sc) {
        uint64_t nd = 0;
        for (uint32_t w = 0; w < total_words; ++w) {
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

    std::filesystem::create_directories(od);
    std::string out_path = std::string(od) + "/aggregates.txt";
    int ofd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);
    _exit(0);
}
