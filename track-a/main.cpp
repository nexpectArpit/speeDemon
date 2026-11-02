#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <cerrno>
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

static constexpr size_t H = 72, MAX_EP = 2048, TS = 4096, TM = TS - 1;

inline uint64_t fast_hash(const char* s, uint16_t l) {
#if defined(__x86_64__) || defined(_M_X64)
    uint64_t h = l; uint16_t i = 0;
    for (; i + 8 <= l; i += 8) { uint64_t v; memcpy(&v, s + i, 8); h = _mm_crc32_u64(h, v); }
    if (i + 4 <= l) { uint32_t v; memcpy(&v, s + i, 4); h = _mm_crc32_u32(uint32_t(h), v); i += 4; }
    for (; i < l; ++i) h = _mm_crc32_u8(uint32_t(h), uint8_t(s[i]));
    return h;
#else
    uint64_t h = 14695981039346656037ULL;
    for (uint16_t i = 0; i < l; ++i) h = (h ^ uint8_t(s[i])) * 1099511628211ULL;
    return h;
#endif
}

inline size_t hi(const char* t) {
    int d = (t[8] - '0') * 10 + t[9] - '0';
    int ds = (t[6] == '0') ? (d - 30) : (d + 1);
    return size_t(ds * 24 + (t[11] - '0') * 10 + t[12] - '0' - 18);
}

struct GlobalEndpointDict {
    std::atomic<uint64_t> tb_h[TS]{};
    std::atomic<const char*> tb_p[TS]{};
    std::atomic<uint32_t> tb_len_id[TS]{};
    std::atomic<uint16_t> count{0};
    std::string_view ep_strings[MAX_EP];

    uint16_t get_or_insert(const char* s, uint16_t l) {
        uint64_t h = fast_hash(s, l);
        if (!h) h = 1;
        uint32_t x = uint32_t(h) & TM;
        for (;;) {
            uint64_t slot_h = tb_h[x].load(std::memory_order_relaxed);
            if (!slot_h) {
                uint64_t expected = 0;
                if (tb_h[x].compare_exchange_strong(expected, h, std::memory_order_relaxed)) {
                    uint16_t id = count.fetch_add(1, std::memory_order_relaxed);
                    ep_strings[id] = std::string_view(s, l);
                    tb_p[x].store(s, std::memory_order_relaxed);
                    tb_len_id[x].store((uint32_t(l) << 16) | (id + 1), std::memory_order_release);
                    return id;
                }
                slot_h = tb_h[x].load(std::memory_order_relaxed);
            }
            if (slot_h == h) {
                uint32_t pack;
                while (!(pack = tb_len_id[x].load(std::memory_order_acquire))) {}
                if ((pack >> 16) == l && !memcmp(tb_p[x].load(std::memory_order_relaxed), s, l))
                    return uint16_t((pack & 0xFFFF) - 1);
            }
            x = (x + 1) & TM;
        }
    }
} g_dict;

struct __attribute__((packed)) Rec {
    uint32_t lat;
    uint16_t ep_err;
};
static_assert(sizeof(Rec) == 6 && MAX_EP < 0x8000);

int main(int ac, char** av) {
    const char* dd = ac == 3 ? av[1] : "/data", *od = ac == 3 ? av[2] : "/out";
    int fd = open((std::string(dd) + "/requests.log").c_str(), O_RDONLY);
    struct stat st{}; fstat(fd, &st); size_t sz = size_t(st.st_size);
    const char* da = sz ? (const char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    close(fd);

    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        size_t v; auto r = std::from_chars(s, s + strlen(s), v);
        if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 16) W = v;
    }
    if (sz < 65536) W = 1;

    std::vector<const char*> bn(W + 1);
    bn[0] = da; bn[W] = da + sz;
    for (size_t i = 1; i < W; ++i) {
        auto* p = da + sz * i / W;
        while (p < bn[W] && p[-1] != '\n') ++p;
        bn[i] = p;
    }

    std::vector<std::array<std::vector<Rec>, H>> th_recs(W);
    std::vector<std::thread> th;

    auto parse_worker = [&](size_t w) {
            auto& recs = th_recs[w];
            const char* c = bn[w], *e = bn[w + 1];
            struct L1EP { const char* p = nullptr; uint16_t l = 0, id = 0; } l1[2048]{};
            while (c < e) {
                size_t h = hi(c); c += 28;
                if (c[0] == 'G' || (c[0] == 'P' && c[1] == 'U')) c += 4;
                else if (c[0] == 'H' || (c[0] == 'P' && c[1] == 'O')) c += 5;
                else if (c[0] == 'P') c += 6;
                else if (c[0] == 'D') c += 7;
                else c += 8;

                const char* es = c, *sp = (const char*)memchr(es, ' ', 128);
                uint16_t el = uint16_t(sp - es), ei;
                uint64_t head = 0, tail = 0;
                memcpy(&head, es, std::min<size_t>(el, 8));
                if (el >= 8) memcpy(&tail, es + el - 8, 8);
                uint64_t key = (head ^ (tail * 0x9e3779b97f4a7c15ULL) ^ el) * 0xbf58476d1ce4e5b9ULL;
                uint16_t l1_idx = uint16_t((key ^ (key >> 32)) & 2047);
                if (l1[l1_idx].l == el && !memcmp(l1[l1_idx].p, es, el)) ei = l1[l1_idx].id;
                else { ei = g_dict.get_or_insert(es, el); l1[l1_idx] = {es, el, ei}; }
                c = sp + 1; bool is_err = (c[0] == '5'); c += 4;
                uint32_t lat = 0;
                while (*c != '\n') { lat = lat * 10 + uint32_t(*c - '0'); ++c; }
                ++c;
                recs[h].push_back({lat, uint16_t(ei | (is_err ? 0x8000 : 0))});
            }
    };
    for (size_t w = 1; w < W; ++w) th.emplace_back(parse_worker, w);
    parse_worker(0);
    for (auto& t : th) t.join();
    th.clear();

    uint16_t num_ep = g_dict.count.load();
    std::vector<uint16_t> sorted_order(num_ep);
    for (uint16_t i = 0; i < num_ep; ++i) sorted_order[i] = i;
    std::sort(sorted_order.begin(), sorted_order.end(), [&](uint16_t a, uint16_t b) {
        return g_dict.ep_strings[a] < g_dict.ep_strings[b];
    });

    std::array<std::string, H> gh;
    {
        int day = 30, hr = 18, m = 10;
        for (size_t h = 0; h < H; ++h) {
            char b[32]; snprintf(b, sizeof(b), "2026-%02d-%02dT%02d:00:00Z", m, day, hr);
            gh[h] = b; hr++; if (hr == 24) { hr = 0; day++; if (day == 32) { day = 1; m = 11; } }
        }
    }

    std::array<std::string, H> ob;
    std::array<std::atomic<bool>, H> ready{};
    std::mutex output_mutex;
    size_t next_output = 0;
    int ofd = open((std::string(od) + "/summary.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) return 1;
    std::atomic<size_t> hc{0};
    auto aggregate_worker = [&]() {
            for (size_t h = hc.fetch_add(1); h < H; h = hc.fetch_add(1)) {
                std::vector<uint32_t> counts(num_ep), errors(num_ep), starts(num_ep + 1);
                for (size_t w = 0; w < W; ++w) {
                    for (const auto& rec : th_recs[w][h]) {
                        uint16_t id = rec.ep_err & 0x7fff;
                        ++counts[id];
                        errors[id] += rec.ep_err >> 15;
                    }
                }
                for (size_t i = 0; i < num_ep; ++i) starts[i + 1] = starts[i] + counts[i];
                std::unique_ptr<uint32_t[]> latencies(new uint32_t[starts.back()]);
                std::vector<uint32_t> positions = starts;
                for (size_t w = 0; w < W; ++w) {
                    for (const auto& rec : th_recs[w][h])
                        latencies[positions[rec.ep_err & 0x7fff]++] = rec.lat;
                    std::vector<Rec>().swap(th_recs[w][h]);
                }
                auto& buf = ob[h]; buf.reserve(num_ep * 85); char b[32];
                for (uint16_t s_id = 0; s_id < num_ep; ++s_id) {
                    uint16_t id = sorted_order[s_id];
                    uint32_t cnt = counts[id];
                    if (!cnt) continue;
                    uint32_t* v = latencies.get() + starts[id];
                    size_t r50 = (50ULL * cnt + 99) / 100 - 1, r95 = (95ULL * cnt + 99) / 100 - 1, r99 = (99ULL * cnt + 99) / 100 - 1;
                    uint32_t p50, p95, p99;
                    if (cnt <= 64) {
                        std::sort(v, v + cnt); p50 = v[r50]; p95 = v[r95]; p99 = v[r99];
                    } else {
                        std::nth_element(v, v + r50, v + cnt); p50 = v[r50];
                        if (r95 > r50) std::nth_element(v + r50 + 1, v + r95, v + cnt);
                        p95 = v[r95];
                        if (r99 == cnt - 1) p99 = *std::max_element(v + r95, v + cnt);
                        else if (r99 > r95) { std::nth_element(v + r95 + 1, v + r99, v + cnt); p99 = v[r99]; }
                        else p99 = p95;
                    }
                    uint32_t err_cnt = errors[id];
                    uint64_t ppm = (uint64_t(err_cnt) * 1000000ULL) / cnt;

                    buf.append(gh[h]); buf += ' ';
                    buf.append(g_dict.ep_strings[sorted_order[s_id]]); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, cnt).ptr); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, p50).ptr); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, p95).ptr); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, p99).ptr); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, err_cnt).ptr); buf += ' ';
                    buf.append(b, std::to_chars(b, b + 32, ppm).ptr); buf += '\n';
                }
                latencies.reset();
                ready[h].store(true, std::memory_order_release);
                std::lock_guard<std::mutex> lock(output_mutex);
                while (next_output < H && ready[next_output].load(std::memory_order_acquire)) {
                    auto& out = ob[next_output++];
                    size_t sent = 0;
                    while (sent < out.size()) {
                        ssize_t n = write(ofd, out.data() + sent, out.size() - sent);
                        if (n > 0) sent += size_t(n);
                        else if (n < 0 && errno == EINTR) continue;
                        else _exit(1);
                    }
                    std::string().swap(out);
                }
            }
    };
    for (size_t w = 1; w < W; ++w) th.emplace_back(aggregate_worker);
    aggregate_worker();
    for (auto& t : th) t.join();

    close(ofd);

    _exit(0);
}
