#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr uint64_t INF = ~0ULL >> 1;

struct Edge { uint32_t to, w; };
struct Q { uint32_t s, t; };

struct HP {
    struct __attribute__((packed)) Item { uint64_t first; uint32_t second; };
    static_assert(sizeof(Item) == 12);
    std::array<std::vector<Item>, 65> buckets;
    uint64_t last = 0;
    size_t count = 0;
    static unsigned index(uint64_t v) { return v ? 64 - __builtin_clzll(v) : 0; }
    void ps(uint64_t d, uint32_t u) {
        buckets[index(d ^ last)].push_back({d, u});
        ++count;
    }
    void prepare() {
        if (!buckets[0].empty()) return;
        unsigned i = 1;
        while (buckets[i].empty()) ++i;
        last = INF;
        for (const auto& p : buckets[i]) last = std::min(last, uint64_t(p.first));
        for (const auto& p : buckets[i]) buckets[index(p.first ^ last)].push_back(p);
        buckets[i].clear();
    }
    uint64_t min() { prepare(); return last; }
    Item pp() {
        prepare();
        Item p = buckets[0].back();
        buckets[0].pop_back();
        --count;
        return p;
    }
    void clear() {
        size_t retained = 0;
        for (auto& bucket : buckets) {
            size_t bytes = bucket.capacity() * sizeof(Item);
            if (retained + bytes > (size_t(512) << 10)) std::vector<Item>().swap(bucket);
            else { retained += bytes; bucket.clear(); }
        }
        last = 0;
        count = 0;
    }
};

struct __attribute__((packed)) Slot {
    uint64_t f = INF, r = INF;
    uint32_t k = ~0u;
};

struct DS {
    std::vector<Slot> tb{4096};
    std::vector<uint32_t> us;
    DS() { us.reserve(2048); }
    void grow() {
        auto old = std::move(tb);
        tb.assign(old.size() * 2, Slot{});
        us.clear();
        uint32_t m = uint32_t(tb.size() - 1);
        for (const auto& s : old) if (s.k != ~0u) {
            uint32_t x = (s.k * 2654435761u) & m;
            while (tb[x].k != ~0u) x = (x + 1) & m;
            tb[x] = s;
            us.push_back(x);
        }
    }
    inline Slot& get(uint32_t u) {
        uint32_t m = uint32_t(tb.size() - 1), x = (u * 2654435761u) & m;
        while (tb[x].k != ~0u && tb[x].k != u) x = (x + 1) & m;
        if (tb[x].k == ~0u) {
            if ((us.size() + 1) * 2 > tb.size()) { grow(); return get(u); }
            tb[x].k = u;
            us.push_back(x);
        }
        return tb[x];
    }
    inline uint64_t gf(uint32_t u) const {
        uint32_t m = uint32_t(tb.size() - 1), x = (u * 2654435761u) & m;
        while (tb[x].k != ~0u) { if (tb[x].k == u) return tb[x].f; x = (x + 1) & m; }
        return INF;
    }
    inline uint64_t gr(uint32_t u) const {
        uint32_t m = uint32_t(tb.size() - 1), x = (u * 2654435761u) & m;
        while (tb[x].k != ~0u) { if (tb[x].k == u) return tb[x].r; x = (x + 1) & m; }
        return INF;
    }
    void clear() {
        if (tb.size() > 65536) { std::vector<Slot>(4096).swap(tb); std::vector<uint32_t>().swap(us); return; }
        for (uint32_t x : us) tb[x] = Slot{};
        us.clear();
    }
};

template<class F> void par(size_t W, F f) {
    std::vector<std::thread> th;
    for (size_t i = 1; i < W; ++i) th.emplace_back(f, i);
    f(0);
    for (auto& x : th) x.join();
}

inline uint32_t pu(const char*& p, const char* e) {
    uint32_t v = 0;
    while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return v;
}

int main(int ac, char** av) {
    const char* dd = ac == 3 ? av[1] : "/data", *od = ac == 3 ? av[2] : "/out";
    size_t W = 12;
    auto map_f = [](const std::string& p, size_t& s) {
        int fd = open(p.c_str(), O_RDONLY);
        struct stat st{}; fstat(fd, &st); s = st.st_size;
        const char* d = s ? (const char*)mmap(nullptr, s, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        close(fd); return d;
    };
    size_t gs = 0, qsz = 0;
    const char* gp = map_f(std::string(dd) + "/graph.txt", gs), *ge = gp + gs;
    const char* graph_base = gp;
    uint32_t n = pu(gp, ge); ++gp; uint32_t m = pu(gp, ge);
    while (gp < ge && *gp != '\n') ++gp;
    if (gp < ge) ++gp;

    std::vector<const char*> bn(W + 1, ge); bn[0] = gp;
    for (size_t i = 1; i < W; ++i) {
        const char* p = gp + (ge - gp) * i / W;
        while (p < ge && *(p - 1) != '\n') ++p;
        bn[i] = p;
    }

    std::vector<uint32_t> fo(n + 1, 0), ro(n + 1, 0);
    par(W, [&](size_t w) {
        const char* sc = bn[w], *lim = bn[w + 1];
        while (sc < lim) {
            if (*sc < '0') { ++sc; continue; }
            uint32_t u = pu(sc, lim); ++sc; uint32_t v = pu(sc, lim);
            while (sc < lim && *sc != '\n') ++sc;
            if (sc < lim) ++sc;
            std::atomic_ref<uint32_t>(fo[u]).fetch_add(1, std::memory_order_relaxed);
            std::atomic_ref<uint32_t>(ro[v]).fetch_add(1, std::memory_order_relaxed);
        }
    });

    uint32_t sf = 0, sr = 0;
    for (uint32_t i = 0; i <= n; ++i) {
        uint32_t df = fo[i], dr = ro[i];
        fo[i] = sf; ro[i] = sr;
        sf += df; sr += dr;
    }
    std::vector<uint32_t> fc = fo, rc = ro;

    std::vector<Edge> fe(m), re(m);
    par(W, [&](size_t w) {
        const char* sc = bn[w], *lim = bn[w + 1];
        while (sc < lim) {
            if (*sc < '0') { ++sc; continue; }
            uint32_t u = pu(sc, lim); ++sc; uint32_t v = pu(sc, lim); ++sc; uint32_t wt = pu(sc, lim);
            while (sc < lim && *sc != '\n') ++sc;
            if (sc < lim) ++sc;
            fe[std::atomic_ref<uint32_t>(fc[u]).fetch_add(1, std::memory_order_relaxed)] = {v, wt};
            re[std::atomic_ref<uint32_t>(rc[v]).fetch_add(1, std::memory_order_relaxed)] = {u, wt};
        }
    });
    std::vector<uint32_t>().swap(fc);
    std::vector<uint32_t>().swap(rc);
    munmap((void*)graph_base, gs);

    const char* qp = map_f(std::string(dd) + "/queries.txt", qsz), *qe = qp + qsz;
    std::vector<Q> qs; qs.reserve(2048);
    while (qp < qe) {
        if (*qp < '0') { ++qp; continue; }
        uint32_t s = pu(qp, qe); ++qp; uint32_t t = pu(qp, qe);
        while (qp < qe && *qp != '\n') ++qp;
        if (qp < qe) ++qp;
        qs.push_back({s, t});
    }

    size_t NQ = qs.size();
    std::vector<int64_t> ans(NQ, -1);
    std::atomic<size_t> qi{0};

    par(W, [&](size_t) {
        DS dist; HP fh, rh;
        for (size_t q = qi.fetch_add(1); q < NQ; q = qi.fetch_add(1)) {
            uint32_t s = qs[q].s, t = qs[q].t;
            if (s == t) { ans[q] = 0; continue; }
            if (s >= n || t >= n || fo[s] == fo[s + 1] || ro[t] == ro[t + 1]) { ans[q] = -1; continue; }
            auto& ss = dist.get(s); ss.f = 0;
            auto& ts = dist.get(t); ts.r = 0;
            fh.ps(0, s); rh.ps(0, t);
            uint64_t mu = INF;

            while (fh.count && rh.count) {
                uint64_t fm = fh.min(), rm = rh.min();
                if (fm + rm >= mu) break;
                bool gf = (fh.count * 4 < rh.count) ? true : (rh.count * 4 < fh.count) ? false : (fm <= rm);
                if (gf) {
                    auto p = fh.pp(); uint64_t d = p.first; uint32_t u = p.second;
                    if (d >= mu || d > dist.gf(u)) continue;
                    for (uint32_t e = fo[u], en = fo[u + 1]; e < en; ++e) {
                        uint32_t v = fe[e].to; uint64_t nd = d + fe[e].w;
                        if (nd >= mu) continue;
                        auto& st = dist.get(v);
                        if (nd < st.f) {
                            st.f = nd;
                            if (st.r != INF && nd + st.r < mu) mu = nd + st.r;
                            fh.ps(nd, v);
                        }
                    }
                } else {
                    auto p = rh.pp(); uint64_t d = p.first; uint32_t v = p.second;
                    if (d >= mu || d > dist.gr(v)) continue;
                    for (uint32_t e = ro[v], en = ro[v + 1]; e < en; ++e) {
                        uint32_t u = re[e].to; uint64_t nd = d + re[e].w;
                        if (nd >= mu) continue;
                        auto& st = dist.get(u);
                        if (nd < st.r) {
                            st.r = nd;
                            if (st.f != INF && st.f + nd < mu) mu = st.f + nd;
                            rh.ps(nd, u);
                        }
                    }
                }
            }
            ans[q] = (mu == INF) ? -1 : int64_t(mu);
            dist.clear(); fh.clear(); rh.clear();
        }
    });

    std::string out; out.reserve(NQ * 12); char buf[24];
    for (int64_t v : ans) {
        if (v < 0) out += "-1\n";
        else { out.append(buf, std::to_chars(buf, buf + 24, uint64_t(v)).ptr); out += '\n'; }
    }
    int ofd = open((std::string(od) + "/distances.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);
    _exit(0);
}
