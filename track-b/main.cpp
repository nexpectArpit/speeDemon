#include <algorithm>
#include <array>
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
    ~MF() { if (d) munmap((void*)d, s); }
};

static constexpr auto make_char_map() {
    std::array<uint8_t, 256> m{};
    for (int c = 0; c < 256; ++c) {
        if (c >= 'a' && c <= 'z') m[c] = c;
        else if (c >= '0' && c <= '9') m[c] = c;
        else if (c >= 'A' && c <= 'Z') m[c] = c + 32;
        else m[c] = 0;
    }
    return m;
}
static constexpr auto CHAR_MAP = make_char_map();

struct Span {
    const char* p;
    uint16_t l;
};

inline bool mt(Span s, std::string_view t) {
    if (s.l != t.size()) return false;
    for (size_t i = 0; i < s.l; ++i) {
        if (CHAR_MAP[(uint8_t)s.p[i]] != uint8_t(t[i])) return false;
    }
    return true;
}

static constexpr size_t TS = 262144;
static constexpr uint32_t TM = TS - 1;

struct DE {
    uint32_t h = 0, id = 0;
};

struct Dict {
    std::vector<DE> tb;
    std::vector<std::string> tm;
    Dict() : tb(TS) {}

    uint32_t in(std::string_view s) {
        uint32_t h = 2166136261u;
        for (char c : s) h = (h ^ uint8_t(c)) * 16777619u;
        uint32_t x = h & TM;
        while (tb[x].id) {
            uint32_t tid = tb[x].id - 1;
            if (tb[x].h == h && tm[tid] == s) return tid;
            x = (x + 1) & TM;
        }
        uint32_t id = uint32_t(tm.size());
        tm.emplace_back(s);
        tb[x] = {h, id + 1};
        return id;
    }

    uint32_t fn_fast(const char* s, size_t l, uint32_t h) const {
        uint32_t x = h & TM;
        while (tb[x].id) {
            uint32_t tid = tb[x].id - 1;
            if (tb[x].h == h && tm[tid].size() == l) {
                bool eq = true;
                for (size_t i = 0; i < l; ++i) {
                    if (CHAR_MAP[(uint8_t)s[i]] != uint8_t(tm[tid][i])) {
                        eq = false;
                        break;
                    }
                }
                if (eq) return tid;
            }
            x = (x + 1) & TM;
        }
        return ~0u;
    }
};

enum class Op : uint8_t { AND, OR, NOT };

struct QItem {
    bool ph = false;
    std::vector<uint32_t> t;
    std::vector<Op> op;
    std::vector<std::string> rw;
};

struct DocRef {
    const char* p;
    uint32_t l;
};

template<class F>
void par(size_t W, F f) {
    if (W <= 1) { f(0); return; }
    std::vector<std::thread> th;
    for (size_t i = 1; i < W; ++i) th.emplace_back(f, i);
    f(0);
    for (auto& x : th) x.join();
}

int main(int ac, char** av) {
    try {
        const char* dd = ac == 3 ? av[1] : "/data";
        const char* od = ac == 3 ? av[2] : "/out";
        size_t W = 12;
        if (auto* s = getenv("SD_THREADS")) {
            size_t v;
            auto r = std::from_chars(s, s + strlen(s), v);
            if (r.ec == std::errc{} && r.ptr == s + strlen(s) && v >= 1 && v <= 12) W = v;
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
                QItem q;
                size_t p = 5;
                bool eo = false;
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
                QItem q;
                q.ph = true;
                size_t p = 8, qe_ = l.rfind('"');
                if (qe_ == std::string_view::npos) qe_ = l.size();
                while (p < qe_) {
                    size_t sp = l.find(' ', p);
                    if (sp == std::string_view::npos || sp > qe_) sp = qe_;
                    std::string_view w = l.substr(p, sp - p);
                    q.t.push_back(dt.in(w));
                    q.rw.emplace_back(w);
                    p = sp + 1;
                }
                qs.push_back(std::move(q));
            }
        }

        size_t NT = dt.tm.size();
        MF df(std::string(dd) + "/docs.txt");
        const char* dp = df.d, * de = dp + df.s;
        const char* last = de;
        while (last > dp && *(last - 1) == '\n') --last;
        while (last > dp && *(last - 1) != '\n') --last;
        uint32_t ND = 0;
        while (last < de && *last >= '0' && *last <= '9') ND = ND * 10 + (*last++ - '0');
        ND++;
        size_t NW = (ND + 63) / 64;
        std::vector<DocRef> dr(ND);

        std::vector<const char*> bn(W + 1, de);
        bn[0] = dp;
        for (size_t i = 1; i < W; ++i) {
            const char* p = dp + df.s * i / W;
            while (p < de && *(p - 1) != '\n') ++p;
            bn[i] = p;
        }

        std::vector<std::vector<uint64_t>> thread_posts(W);
        for (auto& tp : thread_posts) tp.reserve(2500000);

        par(W, [&](size_t wid) {
            const char* sc = bn[wid], * end = bn[wid + 1];
            auto& posts = thread_posts[wid];
            std::vector<uint32_t> last_doc(NT, ~0u);
            while (sc < end) {
                uint32_t id = 0;
                while (sc < end && *sc >= '0' && *sc <= '9') id = id * 10 + (*sc++ - '0');
                if (sc < end && *sc == '\t') ++sc;
                const char* ts = sc;
                while (sc < end && *sc != '\n') ++sc;
                uint32_t len = uint32_t(sc - ts);
                if (sc < end) ++sc;
                dr[id] = {ts, len};

                const char* tp = ts, * te = ts + len;
                while (tp < te) {
                    while (tp < te && !CHAR_MAP[(uint8_t)*tp]) ++tp;
                    if (tp >= te) break;
                    const char* tok_st = tp;
                    uint32_t h = 2166136261u;
                    while (tp < te && CHAR_MAP[(uint8_t)*tp]) {
                        h = (h ^ CHAR_MAP[(uint8_t)*tp]) * 16777619u;
                        ++tp;
                    }
                    uint32_t tl = uint32_t(tp - tok_st);
                    uint32_t tid = dt.fn_fast(tok_st, tl, h);
                    if (tid != ~0u) {
                        if (last_doc[tid] != id) {
                            last_doc[tid] = id;
                            posts.push_back((uint64_t(tid) << 32) | id);
                        }
                    }
                }
            }
        });

        std::vector<uint32_t> df_cnt(NT, 0);
        for (size_t w = 0; w < W; ++w) {
            for (uint64_t p : thread_posts[w]) {
                df_cnt[uint32_t(p >> 32)]++;
            }
        }

        std::vector<uint32_t> pos_off(NT + 1, 0);
        for (size_t t = 0; t < NT; ++t) pos_off[t + 1] = pos_off[t] + df_cnt[t];
        size_t total_p = pos_off[NT];

        std::vector<uint32_t> all_postings(total_p);
        auto cur_off = pos_off;
        for (size_t w = 0; w < W; ++w) {
            for (uint64_t p : thread_posts[w]) {
                uint32_t tid = uint32_t(p >> 32);
                uint32_t did = uint32_t(p);
                all_postings[cur_off[tid]++] = did;
            }
            std::vector<uint64_t>().swap(thread_posts[w]); // free immediately
        }

        std::vector<std::vector<uint64_t>> bs(NT);
        for (size_t t = 0; t < NT; ++t) {
            if (df_cnt[t] >= 1024) {
                bs[t].assign(NW, 0);
                uint32_t st = pos_off[t], en = pos_off[t + 1];
                for (uint32_t i = st; i < en; ++i) {
                    uint32_t id = all_postings[i];
                    bs[t][id >> 6] |= (1ULL << (id & 63));
                }
            }
        }

        auto get_pos = [&](uint32_t t) -> std::pair<const uint32_t*, size_t> {
            return {all_postings.data() + pos_off[t], df_cnt[t]};
        };

        size_t NQ = qs.size();
        std::vector<std::string> res(NQ);
        std::atomic<size_t> qc{0};

        par(W, [&](size_t) {
            std::vector<uint64_t> cb(NW), tb(NW);
            std::vector<uint32_t> pc;
            std::vector<Span> dtk;
            dtk.reserve(512);
            char b[32];

            for (size_t q = qc.fetch_add(1); q < NQ; q = qc.fetch_add(1)) {
                auto& it = qs[q];
                auto& rs = res[q];

                if (!it.ph) {
                    std::fill(cb.begin(), cb.end(), 0);
                    uint32_t t0 = it.t[0];
                    if (t0 != ~0u) {
                        if (!bs[t0].empty()) std::copy(bs[t0].begin(), bs[t0].end(), cb.begin());
                        else {
                            auto [p, sz] = get_pos(t0);
                            for (size_t i = 0; i < sz; ++i) {
                                uint32_t d = p[i];
                                cb[d >> 6] |= (1ULL << (d & 63));
                            }
                        }
                    }
                    for (size_t i = 0; i < it.op.size(); ++i) {
                        Op o = it.op[i];
                        uint32_t ti = it.t[i + 1];
                        if (ti == ~0u || df_cnt[ti] == 0) {
                            if (o == Op::AND) std::fill(cb.begin(), cb.end(), 0);
                            continue;
                        }
                        if (!bs[ti].empty()) {
                            const auto& b_ = bs[ti];
                            if (o == Op::AND) for (size_t w = 0; w < NW; ++w) cb[w] &= b_[w];
                            else if (o == Op::OR) for (size_t w = 0; w < NW; ++w) cb[w] |= b_[w];
                            else for (size_t w = 0; w < NW; ++w) cb[w] &= ~b_[w];
                        } else {
                            auto [pl, psz] = get_pos(ti);
                            if (o == Op::AND) {
                                std::fill(tb.begin(), tb.end(), 0);
                                for (size_t k = 0; k < psz; ++k) {
                                    uint32_t d = pl[k];
                                    if (cb[d >> 6] & (1ULL << (d & 63))) tb[d >> 6] |= (1ULL << (d & 63));
                                }
                                std::copy(tb.begin(), tb.end(), cb.begin());
                            } else if (o == Op::OR) {
                                for (size_t k = 0; k < psz; ++k) {
                                    uint32_t d = pl[k];
                                    cb[d >> 6] |= (1ULL << (d & 63));
                                }
                            } else {
                                for (size_t k = 0; k < psz; ++k) {
                                    uint32_t d = pl[k];
                                    cb[d >> 6] &= ~(1ULL << (d & 63));
                                }
                            }
                        }
                    }
                    uint32_t cnt = 0, f10[10], n10 = 0;
                    uint64_t isum = 0;
                    for (size_t w = 0; w < NW; ++w) {
                        uint64_t m = cb[w];
                        if (!m) continue;
                        cnt += __builtin_popcountll(m);
                        size_t base = w << 6;
                        while (m) {
                            int bit = __builtin_ctzll(m);
                            uint32_t d = uint32_t(base + bit);
                            isum += d;
                            if (n10 < 10) f10[n10++] = d;
                            m &= (m - 1);
                        }
                    }
                    rs.append(b, std::to_chars(b, b + 32, cnt).ptr);
                    rs += ' ';
                    rs.append(b, std::to_chars(b, b + 32, isum).ptr);
                    for (uint32_t i = 0; i < n10; ++i) {
                        rs += ' ';
                        rs.append(b, std::to_chars(b, b + 32, f10[i]).ptr);
                    }
                } else {
                    const auto& ts = it.t;
                    const auto& rw = it.rw;
                    size_t K = ts.size();
                    bool emp = false;
                    for (uint32_t ti : ts) if (ti == ~0u || df_cnt[ti] == 0) { emp = true; break; }
                    if (emp) { rs = "0 0"; continue; }

                    std::array<size_t, 16> ord;
                    for (size_t i = 0; i < K; ++i) ord[i] = i;
                    std::sort(ord.begin(), ord.begin() + K, [&](size_t a, size_t b) {
                        return df_cnt[ts[a]] < df_cnt[ts[b]];
                    });

                    auto [first_p, first_sz] = get_pos(ts[ord[0]]);
                    pc.assign(first_p, first_p + first_sz);

                    for (size_t idx = 1; idx < K; ++idx) {
                        uint32_t ti = ts[ord[idx]];
                        if (!bs[ti].empty()) {
                            const auto& b_ = bs[ti];
                            size_t o = 0;
                            for (uint32_t d : pc) if (b_[d >> 6] & (1ULL << (d & 63))) pc[o++] = d;
                            pc.resize(o);
                        } else {
                            auto [pl, psz] = get_pos(ti);
                            size_t o = 0;
                            for (uint32_t d : pc) {
                                if (std::binary_search(pl, pl + psz, d)) pc[o++] = d;
                            }
                            pc.resize(o);
                        }
                        if (pc.empty()) break;
                    }

                    uint32_t cnt = 0, f10[10], n10 = 0;
                    uint64_t isum = 0;
                    for (uint32_t d : pc) {
                        const char* tp = dr[d].p, * te = tp + dr[d].l;
                        dtk.clear();
                        while (tp < te) {
                            while (tp < te && !CHAR_MAP[(uint8_t)*tp]) ++tp;
                            if (tp >= te) break;
                            const char* st = tp;
                            while (tp < te && CHAR_MAP[(uint8_t)*tp]) ++tp;
                            dtk.push_back({st, uint16_t(tp - st)});
                        }
                        if (dtk.size() < K) continue;
                        bool ok = false;
                        for (size_t p = 0; p + K <= dtk.size(); ++p) {
                            bool eq = true;
                            for (size_t j = 0; j < K; ++j) {
                                if (!mt(dtk[p + j], rw[j])) { eq = false; break; }
                            }
                            if (eq) { ok = true; break; }
                        }
                        if (ok) {
                            cnt++;
                            isum += d;
                            if (n10 < 10) f10[n10++] = d;
                        }
                    }
                    rs.append(b, std::to_chars(b, b + 32, cnt).ptr);
                    rs += ' ';
                    rs.append(b, std::to_chars(b, b + 32, isum).ptr);
                    for (uint32_t i = 0; i < n10; ++i) {
                        rs += ' ';
                        rs.append(b, std::to_chars(b, b + 32, f10[i]).ptr);
                    }
                }
            }
        });

        std::string out;
        out.reserve(NQ * 32);
        for (size_t q = 0; q < NQ; ++q) {
            out.append(res[q]);
            out += '\n';
        }
        std::filesystem::create_directories(od);
        int ofd = open((std::string(od) + "/results.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
        close(ofd);
        _exit(0);
    }
    catch (std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
