#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr uint64_t INF = ~0ULL;

struct MF {
    const char* d = nullptr;
    size_t s = 0;
    MF(const std::string& p) {
        int fd = open(p.c_str(), O_RDONLY);
        struct stat st{};
        fstat(fd, &st);
        s = st.st_size;
        d = s ? (char*)mmap(nullptr, s, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
        close(fd);
    }
};

struct Edge { uint32_t to = 0, w = 0; };
struct Q { uint32_t s = 0, t = 0; };

// 4-ary heap for fewer cache misses
struct HP {
    std::vector<std::pair<uint64_t, uint32_t>> d;
    HP() { d.reserve(16384); }
    bool em() const { return d.empty(); }
    size_t sz() const { return d.size(); }
    void cl() { d.clear(); }
    auto tp() const { return d[0]; }
    void ps(uint64_t w, uint32_t u) {
        size_t i = d.size();
        d.push_back({w, u});
        while (i > 0) {
            size_t p = (i - 1) >> 2;
            if (d[i].first < d[p].first) {
                std::swap(d[i], d[p]);
                i = p;
            } else break;
        }
    }
    auto pp() {
        auto t = d[0];
        auto b = d.back();
        d.pop_back();
        if (!d.empty()) {
            d[0] = b;
            size_t i = 0, n = d.size();
            while (true) {
                size_t c0 = (i << 2) + 1;
                if (c0 >= n) break;
                size_t s = c0;
                size_t c_end = std::min(n, c0 + 4);
                for (size_t c = c0 + 1; c < c_end; ++c) {
                    if (d[c].first < d[s].first) s = c;
                }
                if (d[s].first < b.first) {
                    d[i] = d[s];
                    i = s;
                } else {
                    d[i] = b;
                    break;
                }
            }
            d[i] = b;
        }
        return t;
    }
};

template<class F>
void par(size_t W, F f) {
    if (W <= 1) { f(0); return; }
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
    const char* dd = ac == 3 ? av[1] : "/data";
    const char* od = ac == 3 ? av[2] : "/out";
    size_t W = 12;
    if (auto* s = getenv("SD_THREADS")) {
        size_t v;
        auto r = std::from_chars(s, s + strlen(s), v);
        if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 12) W = v;
    }

    MF gf(std::string(dd) + "/graph.txt");
    const char* gp = gf.d, * ge = gp + gf.s;
    uint32_t n = pu(gp, ge);
    if (gp < ge && *gp == ' ') ++gp;
    uint32_t m = pu(gp, ge);
    while (gp < ge && *gp != '\n') ++gp;
    if (gp < ge) ++gp;

    const char* es = gp;
    size_t eb = ge - es;
    std::vector<const char*> bn(W + 1, ge);
    bn[0] = es;
    for (size_t i = 1; i < W; ++i) {
        const char* p = es + eb * i / W;
        while (p < ge && *(p - 1) != '\n') ++p;
        bn[i] = p;
    }

    std::vector<std::atomic<uint32_t>> odg(n), idg(n);
    for (uint32_t i = 0; i < n; ++i) {
        odg[i].store(0, std::memory_order_relaxed);
        idg[i].store(0, std::memory_order_relaxed);
    }

    par(W, [&](size_t wid) {
        const char* sc = bn[wid], * lim = bn[wid + 1];
        while (sc < lim) {
            if (*sc == '\n' || *sc == '\r') { ++sc; continue; }
            uint32_t u = pu(sc, lim);
            ++sc;
            uint32_t v = pu(sc, lim);
            const char* nl = (const char*)memchr(sc, '\n', lim - sc);
            sc = nl ? nl + 1 : lim;
            if (u < n && v < n) {
                odg[u].fetch_add(1, std::memory_order_relaxed);
                idg[v].fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::vector<uint32_t> fo(n + 1, 0), ro(n + 1, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t od = odg[i].load(std::memory_order_relaxed);
        uint32_t id = idg[i].load(std::memory_order_relaxed);
        fo[i + 1] = fo[i] + od;
        ro[i + 1] = ro[i] + id;
        odg[i].store(fo[i], std::memory_order_relaxed);
        idg[i].store(ro[i], std::memory_order_relaxed);
    }

    std::vector<Edge> fe(m), re(m);
    par(W, [&](size_t wid) {
        const char* sc = bn[wid], * lim = bn[wid + 1];
        while (sc < lim) {
            if (*sc == '\n' || *sc == '\r') { ++sc; continue; }
            uint32_t u = pu(sc, lim);
            ++sc;
            uint32_t v = pu(sc, lim);
            ++sc;
            uint32_t w = pu(sc, lim);
            if (sc < lim && *sc == '\r') ++sc;
            if (sc < lim && *sc == '\n') ++sc;
            if (u < n && v < n) {
                fe[odg[u].fetch_add(1, std::memory_order_relaxed)] = {v, w};
                re[idg[v].fetch_add(1, std::memory_order_relaxed)] = {u, w};
            }
        }
    });
    std::vector<std::atomic<uint32_t>>().swap(odg);
    std::vector<std::atomic<uint32_t>>().swap(idg);

    MF qf(std::string(dd) + "/queries.txt");
    const char* qp = qf.d, * qe = qp + qf.s;
    std::vector<Q> qs;
    qs.reserve(2048);
    while (qp < qe) {
        if (*qp == '\n' || *qp == '\r') { ++qp; continue; }
        uint32_t s = pu(qp, qe);
        if (qp < qe && *qp == ' ') ++qp;
        uint32_t t = pu(qp, qe);
        while (qp < qe && *qp != '\n') ++qp;
        if (qp < qe) ++qp;
        qs.push_back({s, t});
    }

    size_t NQ = qs.size();
    std::vector<int64_t> ans(NQ, -1);

    size_t QW = std::min<size_t>(W, 6);
    struct WS {
        std::vector<uint64_t> fd, rd;
        std::vector<uint32_t> vs;
        HP fh, rh;
        WS(size_t N) : fd(N, INF), rd(N, INF) { vs.reserve(16384); }
    };
    std::vector<WS> ws;
    ws.reserve(QW);
    for (size_t w = 0; w < QW; ++w) ws.emplace_back(n);
    std::atomic<size_t> qi{0};

    par(QW, [&](size_t wid) {
        WS& st = ws[wid];
        auto& fd = st.fd;
        auto& rd = st.rd;
        auto& vs = st.vs;
        auto& fh = st.fh;
        auto& rh = st.rh;

        for (size_t q = qi.fetch_add(1); q < NQ; q = qi.fetch_add(1)) {
            uint32_t s = qs[q].s, t = qs[q].t;
            if (s == t) { ans[q] = 0; continue; }
            if (s >= n || t >= n || fo[s] == fo[s + 1] || ro[t] == ro[t + 1]) { ans[q] = -1; continue; }

            fd[s] = 0;
            rd[t] = 0;
            vs.push_back(s);
            vs.push_back(t);
            fh.ps(0, s);
            rh.ps(0, t);
            uint64_t mu = INF;

            while (!fh.em() && !rh.em()) {
                uint64_t fm = fh.tp().first, rm = rh.tp().first;
                if (fm + rm >= mu) break;

                bool go_fwd = (fh.sz() * 4 < rh.sz()) ? true : (rh.sz() * 4 < fh.sz()) ? false : (fm <= rm);

                if (go_fwd) {
                    auto [d, u] = fh.pp();
                    if (d > fd[u] || d >= mu) continue;
                    for (uint32_t e = fo[u], en = fo[u + 1]; e < en; ++e) {
                        uint32_t v = fe[e].to;
                        uint64_t nd = d + fe[e].w;
                        if (nd < fd[v]) {
                            if (fd[v] == INF && rd[v] == INF) vs.push_back(v);
                            fd[v] = nd;
                            fh.ps(nd, v);
                            if (rd[v] != INF && nd + rd[v] < mu) mu = nd + rd[v];
                        }
                    }
                } else {
                    auto [d, v] = rh.pp();
                    if (d > rd[v] || d >= mu) continue;
                    for (uint32_t e = ro[v], en = ro[v + 1]; e < en; ++e) {
                        uint32_t u = re[e].to;
                        uint64_t nd = d + re[e].w;
                        if (nd < rd[u]) {
                            if (fd[u] == INF && rd[u] == INF) vs.push_back(u);
                            rd[u] = nd;
                            rh.ps(nd, u);
                            if (fd[u] != INF && fd[u] + nd < mu) mu = fd[u] + nd;
                        }
                    }
                }
            }
            ans[q] = (mu == INF) ? -1 : int64_t(mu);
            for (uint32_t nd : vs) { fd[nd] = INF; rd[nd] = INF; }
            vs.clear();
            fh.cl();
            rh.cl();
        }
    });

    std::string out;
    out.reserve(NQ * 12);
    char buf[24];
    for (size_t q = 0; q < NQ; ++q) {
        if (ans[q] < 0) out += "-1\n";
        else {
            out.append(buf, std::to_chars(buf, buf + 24, uint64_t(ans[q])).ptr);
            out += '\n';
        }
    }
    int ofd = open((std::string(od) + "/distances.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);
    _exit(0);
}
