#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr size_t H = 72, TS = 8192;
static constexpr uint32_t TM = TS - 1;

struct DE {
    uint64_t h = 0;
    const char* p = nullptr;
    uint16_t l = 0, id = 0;
};

struct Rec {
    uint32_t lat;
    uint16_t ep;
    uint8_t h;
    uint8_t err;
};

inline uint64_t fast_hash(const char* s, uint16_t l) {
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t h = l;
    uint16_t i = 0;
    for (; i + 8 <= l; i += 8) {
        uint64_t v;
        memcpy(&v, s + i, 8);
        h = _mm_crc32_u64(h, v);
    }
    if (i + 4 <= l) {
        uint32_t v;
        memcpy(&v, s + i, 4);
        h = _mm_crc32_u32(uint32_t(h), v);
        i += 4;
    }
    for (; i < l; ++i) {
        h = _mm_crc32_u8(uint32_t(h), uint8_t(s[i]));
    }
    return h;
#else
    uint64_t h = 14695981039346656037ULL;
    for (uint16_t i = 0; i < l; ++i) {
        h = (h ^ uint8_t(s[i])) * 1099511628211ULL;
    }
    return h;
#endif
}

struct Sh {
    DE tb[TS]{};
    std::vector<std::string_view> ep;
    std::vector<Rec> recs;
    std::array<uint32_t, H + 1> h_off{};
    std::array<std::string, H> hs;

    uint16_t in(const char* s, uint16_t l) {
        uint64_t h = fast_hash(s, l);
        uint32_t x = uint32_t(h) & TM;
        while (tb[x].p) {
            if (tb[x].h == h && tb[x].l == l && !memcmp(tb[x].p, s, l)) return tb[x].id;
            x = (x + 1) & TM;
        }
        uint16_t id = uint16_t(ep.size());
        ep.emplace_back(s, l);
        tb[x] = {h, s, l, id};
        return id;
    }
};

inline size_t hi(const char* t) {
    int d = (t[8] - '0') * 10 + t[9] - '0';
    int ds = (t[6] == '0') ? (d - 30) : (d + 1);
    return size_t(ds * 24 + (t[11] - '0') * 10 + t[12] - '0' - 18);
}

void pc(const char* c, const char* e, Sh& s) {
    size_t est_lines = (e - c) / 75 + 4096;
    std::vector<Rec> raw;
    raw.reserve(est_lines);
    uint32_t h_cnt[H]{};

    while (c < e) {
        size_t h = hi(c);
        if (s.hs[h].empty()) s.hs[h] = std::string(c, 13) + ":00:00Z";
        c += 28;
        // Fast method skip: GET/PUT(3), POST/HEAD(4), PATCH(5), DELETE(6), OPTIONS(7)
        if (c[0] == 'G' || (c[0] == 'P' && c[1] == 'U')) c += 4;
        else if (c[0] == 'H' || (c[0] == 'P' && c[1] == 'O')) c += 5;
        else if (c[0] == 'P') c += 6;
        else if (c[0] == 'D') c += 7;
        else c += 8;

        const char* es = c;
        const char* sp = (const char*)memchr(es, ' ', 128);
        uint16_t el = uint16_t(sp - es);
        uint16_t ei = s.in(es, el);
        c = sp + 1;

        uint8_t er = (c[0] == '5');
        c += 4;
        uint32_t l = 0;
        while (*c != '\n') {
            l = l * 10 + uint32_t(*c - '0');
            ++c;
        }
        ++c;
        raw.push_back({l, ei, uint8_t(h), er});
        h_cnt[h]++;
    }

    s.h_off[0] = 0;
    for (size_t h = 0; h < H; ++h) s.h_off[h + 1] = s.h_off[h] + h_cnt[h];
    s.recs.resize(raw.size());
    auto cur_off = s.h_off;
    for (const auto& r : raw) {
        s.recs[cur_off[r.h]++] = r;
    }
}

inline void wu(std::string& o, uint64_t v) {
    char b[24];
    o.append(b, std::to_chars(b, b + 24, v).ptr);
}

int main(int ac, char** av) {
    const char* dd = ac == 3 ? av[1] : "/data";
    const char* od = ac == 3 ? av[2] : "/out";
    int fd = open((std::string(dd) + "/requests.log").c_str(), O_RDONLY);
    struct stat st{};
    fstat(fd, &st);
    size_t sz = size_t(st.st_size);
    const char* da = sz ? (const char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    close(fd);

    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        size_t v;
        auto r = std::from_chars(s, s + strlen(s), v);
        if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 12) W = v;
    }
    if (sz < 65536) W = 1;

    std::vector<Sh> sh(W);
    if (sz) {
        std::vector<const char*> bn(W + 1);
        bn[0] = da;
        bn[W] = da + sz;
        for (size_t i = 1; i < W; ++i) {
            auto* p = da + sz * i / W;
            while (p < bn[W] && p[-1] != '\n') ++p;
            bn[i] = p;
        }
        std::vector<std::thread> th;
        for (size_t i = 1; i < W; ++i) th.emplace_back(pc, bn[i], bn[i + 1], std::ref(sh[i]));
        pc(bn[0], bn[1], sh[0]);
        for (auto& t : th) t.join();
    }

    std::vector<std::string_view> ge;
    ge.reserve(2048);
    std::unordered_map<std::string_view, uint16_t> ed;
    ed.reserve(4096);
    std::array<std::string, H> gh;
    for (auto& s : sh) {
        for (size_t h = 0; h < H; ++h)
            if (gh[h].empty() && !s.hs[h].empty()) gh[h] = std::move(s.hs[h]);
        for (auto& e : s.ep)
            if (ed.try_emplace(e, uint16_t(ge.size())).second) ge.push_back(e);
    }
    std::sort(ge.begin(), ge.end());
    for (uint16_t i = 0; i < uint16_t(ge.size()); ++i) ed[ge[i]] = i;

    std::vector<std::vector<uint16_t>> sr(W);
    for (size_t w = 0; w < W; ++w) {
        sr[w].resize(sh[w].ep.size());
        for (size_t i = 0; i < sh[w].ep.size(); ++i) sr[w][i] = ed[sh[w].ep[i]];
    }

    size_t ne = ge.size();
    std::array<std::string, H> ob;
    std::atomic<size_t> hc{0};
    auto work = [&](size_t) {
        std::vector<uint32_t> ec(ne, 0), ee(ne, 0), eo(ne + 1, 0);
        std::vector<uint32_t> la;
        la.reserve(65536);
        for (size_t h = hc.fetch_add(1); h < H; h = hc.fetch_add(1)) {
            if (gh[h].empty()) continue;
            size_t tr = 0;
            std::fill(ec.begin(), ec.end(), 0);
            std::fill(ee.begin(), ee.end(), 0);
            for (size_t w = 0; w < W; ++w) {
                uint32_t st_i = sh[w].h_off[h], en_i = sh[w].h_off[h + 1];
                tr += (en_i - st_i);
                const Rec* r_ptr = sh[w].recs.data();
                const uint16_t* sr_ptr = sr[w].data();
                for (uint32_t i = st_i; i < en_i; ++i) {
                    uint16_t g = sr_ptr[r_ptr[i].ep];
                    ec[g]++;
                    ee[g] += r_ptr[i].err;
                }
            }
            if (!tr) continue;
            eo[0] = 0;
            for (size_t e = 0; e < ne; ++e) eo[e + 1] = eo[e] + ec[e];
            la.resize(tr);
            auto co = eo;
            for (size_t w = 0; w < W; ++w) {
                uint32_t st_i = sh[w].h_off[h], en_i = sh[w].h_off[h + 1];
                const Rec* r_ptr = sh[w].recs.data();
                const uint16_t* sr_ptr = sr[w].data();
                for (uint32_t i = st_i; i < en_i; ++i) {
                    uint16_t g = sr_ptr[r_ptr[i].ep];
                    la[co[g]++] = r_ptr[i].lat;
                }
            }
            auto& buf = ob[h];
            buf.reserve(ne * 85);
            for (size_t e = 0; e < ne; ++e) {
                uint32_t cnt = ec[e];
                if (!cnt) continue;
                uint32_t* v = la.data() + eo[e];
                size_t r50 = (50ULL * cnt + 99) / 100 - 1;
                size_t r95 = (95ULL * cnt + 99) / 100 - 1;
                size_t r99 = (99ULL * cnt + 99) / 100 - 1;
                uint32_t p50, p95, p99;
                if (cnt <= 64) {
                    std::sort(v, v + cnt);
                    p50 = v[r50];
                    p95 = v[r95];
                    p99 = v[r99];
                } else {
                    std::nth_element(v, v + r50, v + cnt);
                    p50 = v[r50];
                    if (r95 > r50) std::nth_element(v + r50 + 1, v + r95, v + cnt);
                    p95 = v[r95];
                    if (r99 == cnt - 1) {
                        p99 = *std::max_element(v + r95, v + cnt);
                    } else if (r99 > r95) {
                        std::nth_element(v + r95 + 1, v + r99, v + cnt);
                        p99 = v[r99];
                    } else {
                        p99 = p95;
                    }
                }
                uint64_t ppm = uint64_t(ee[e]) * 1000000ULL / cnt;
                buf += gh[h];
                buf += ' ';
                buf.append(ge[e]);
                buf += ' ';
                wu(buf, cnt);
                buf += ' ';
                wu(buf, p50);
                buf += ' ';
                wu(buf, p95);
                buf += ' ';
                wu(buf, p99);
                buf += ' ';
                wu(buf, ee[e]);
                buf += ' ';
                wu(buf, ppm);
                buf += '\n';
            }
        }
    };

    std::vector<std::thread> th;
    for (size_t i = 1; i < W; ++i) th.emplace_back(work, i);
    work(0);
    for (auto& t : th) t.join();

    std::string out_path = std::string(od) + "/summary.txt";
    int ofd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (auto& b : ob) {
        if (!b.empty()) {
            [[maybe_unused]] auto wr = write(ofd, b.data(), b.size());
        }
    }
    close(ofd);
    _exit(0);
}
