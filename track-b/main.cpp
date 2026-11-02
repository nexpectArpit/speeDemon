#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
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

struct DE { uint64_t h64 = 0; uint32_t id = 0; };

struct Dict {
    std::vector<DE> tb{TS};
    uint32_t count = 0;
    uint32_t in(std::string_view s) {
        uint64_t h = fast_hash_swar(s.data(), s.size());
        uint32_t x = uint32_t(h) & TM;
        while (tb[x].id) {
            if (tb[x].h64 == h) return tb[x].id - 1;
            x = (x + 1) & TM;
        }
        uint32_t id = count++;
        tb[x] = {h, id + 1};
        return id;
    }
    inline uint32_t fn_fast(uint64_t h) const {
        uint32_t x = uint32_t(h) & TM;
        while (tb[x].id) {
            if (tb[x].h64 == h) return tb[x].id - 1;
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
    std::vector<std::string_view> rw;
};

struct DocRef { const char* p; uint32_t l; };

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
    size_t NW = (ND + 63) / 64;
    std::vector<DocRef> dr(ND);

    std::vector<const char*> bn(W + 1, de);
    bn[0] = dp;
    for (size_t i = 1; i < W; ++i) {
        const char* p = dp + df.s * i / W;
        while (p < de && *(p - 1) != '\n') ++p;
        bn[i] = p;
    }

    std::vector<std::vector<uint32_t>> th_df(W, std::vector<uint32_t>(NT, 0));

    par(W, [&](size_t wid) {
        const char* sc = bn[wid], * end = bn[wid + 1];
        auto& l_df = th_df[wid];
        std::vector<uint32_t> last_doc(NT, ~0u);
        struct L1F { uint64_t hash = 0; uint32_t tid = ~0u; };
        std::vector<L1F> l1(16384);

        while (sc < end) {
            uint32_t id = 0;
            while (*sc >= '0' && *sc <= '9') id = id * 10 + (*sc++ - '0');
            ++sc;
            const char* ts = sc;
            const char* nl = (const char*)memchr(sc, '\n', end - sc);
            if (!nl) nl = end;
            uint32_t len = uint32_t(nl - ts);
            sc = (nl < end) ? nl + 1 : end;
            dr[id] = {ts, len};

            const char* tp = ts, * te = ts + len;
            while (tp < te) {
                while (tp < te && !CHAR_MAP[(uint8_t)*tp]) ++tp;
                if (tp >= te) break;
                const char* tok_st = tp;
                while (tp < te && CHAR_MAP[(uint8_t)*tp]) ++tp;
                size_t tl = size_t(tp - tok_st);
                if (tl <= 32) {
                    uint64_t h64 = fast_hash_swar(tok_st, tl);
                    uint32_t lidx = uint32_t(h64) & 16383;
                    uint32_t tid;
                    if (l1[lidx].hash == h64) {
                        tid = l1[lidx].tid;
                    } else {
                        tid = dt.fn_fast(h64);
                        l1[lidx] = {h64, tid};
                    }
                    if (tid != ~0u && last_doc[tid] != id) {
                        last_doc[tid] = id;
                        l_df[tid]++;
                    }
                }
            }
        }
    });

    std::vector<uint32_t> df_cnt(NT, 0);
    for (size_t t = 0; t < NT; ++t) {
        uint32_t sum = 0;
        for (size_t w = 0; w < W; ++w) sum += th_df[w][t];
        df_cnt[t] = sum;
    }

    static constexpr uint32_t BITSET_THRESH = 5000;
    std::vector<uint32_t> bitset_map(NT, ~0u);
    uint32_t num_bitsets = 0;
    for (size_t t = 0; t < NT; ++t) {
        if (df_cnt[t] >= BITSET_THRESH) bitset_map[t] = num_bitsets++;
    }

    std::vector<std::vector<uint64_t>> bitsets(num_bitsets, std::vector<uint64_t>(NW, 0));
    std::vector<std::vector<uint32_t>> th_off(W, std::vector<uint32_t>(NT, 0));
    std::vector<uint32_t> pos_off(NT + 1, 0);
    for (size_t t = 0; t < NT; ++t) {
        if (bitset_map[t] == ~0u) {
            uint32_t sum = 0;
            for (size_t w = 0; w < W; ++w) {
                th_off[w][t] = pos_off[t] + sum;
                sum += th_df[w][t];
            }
            pos_off[t + 1] = pos_off[t] + sum;
        } else {
            pos_off[t + 1] = pos_off[t];
        }
    }
    std::vector<std::vector<uint32_t>>().swap(th_df);

    uint32_t total_array_postings = pos_off[NT];
    std::vector<uint32_t> all_postings(total_array_postings);

    par(W, [&](size_t wid) {
        const char* sc = bn[wid], * end = bn[wid + 1];
        auto& l_off = th_off[wid];
        std::vector<uint32_t> last_doc(NT, ~0u);
        struct L1F { uint64_t hash = 0; uint32_t tid = ~0u; };
        std::vector<L1F> l1(16384);

        while (sc < end) {
            uint32_t id = 0;
            while (*sc >= '0' && *sc <= '9') id = id * 10 + (*sc++ - '0');
            sc = dr[id].p + dr[id].l + 1;
            const char* tp = dr[id].p, * te = tp + dr[id].l;
            while (tp < te) {
                while (tp < te && !CHAR_MAP[(uint8_t)*tp]) ++tp;
                if (tp >= te) break;
                const char* tok_st = tp;
                while (tp < te && CHAR_MAP[(uint8_t)*tp]) ++tp;
                size_t tl = size_t(tp - tok_st);
                if (tl <= 32) {
                    uint64_t h64 = fast_hash_swar(tok_st, tl);
                    uint32_t lidx = uint32_t(h64) & 16383;
                    uint32_t tid;
                    if (l1[lidx].hash == h64) {
                        tid = l1[lidx].tid;
                    } else {
                        tid = dt.fn_fast(h64);
                        l1[lidx] = {h64, tid};
                    }
                    if (tid != ~0u && last_doc[tid] != id) {
                        last_doc[tid] = id;
                        uint32_t bs_id = bitset_map[tid];
                        if (bs_id != ~0u) {
                            std::atomic_ref<uint64_t>(bitsets[bs_id][id >> 6]).fetch_or(1ULL << (id & 63), std::memory_order_relaxed);
                        } else {
                            all_postings[l_off[tid]++] = id;
                        }
                    }
                }
            }
        }
    });
    std::vector<std::vector<uint32_t>>().swap(th_off);

    auto get_pos = [&](uint32_t t) -> std::pair<const uint32_t*, size_t> {
        return {all_postings.data() + pos_off[t], df_cnt[t]};
    };

    size_t NQ = qs.size();
    std::vector<std::string> res(NQ);
    std::atomic<size_t> qc{0};

    par(W, [&](size_t) {
        std::vector<uint64_t> cb(NW, 0);
        std::vector<uint32_t> cur, nxt;
        cur.reserve(65536); nxt.reserve(65536);
        char b[32];

        auto fmt_res = [&](std::string& rs, uint32_t cnt, uint64_t isum, const uint32_t* f10, uint32_t n10) {
            rs.append(b, std::to_chars(b, b + 32, cnt).ptr);
            rs += ' ';
            rs.append(b, std::to_chars(b, b + 32, isum).ptr);
            for (uint32_t i = 0; i < n10; ++i) {
                rs += ' ';
                rs.append(b, std::to_chars(b, b + 32, f10[i]).ptr);
            }
        };

        for (size_t q = qc.fetch_add(1); q < NQ; q = qc.fetch_add(1)) {
            auto& it = qs[q];
            auto& rs = res[q];

            if (!it.ph) {
                bool in_bitset = false;
                cur.clear();
                uint32_t t0 = it.t[0];
                if (t0 != ~0u && df_cnt[t0] > 0) {
                    uint32_t bs0 = bitset_map[t0];
                    if (bs0 != ~0u) {
                        in_bitset = true;
                        std::memcpy(cb.data(), bitsets[bs0].data(), NW * sizeof(uint64_t));
                    } else {
                        auto [p, sz] = get_pos(t0);
                        if (sz > 16384 && !it.op.empty()) {
                            in_bitset = true;
                            std::fill(cb.begin(), cb.end(), 0);
                            for (size_t i = 0; i < sz; ++i) cb[p[i] >> 6] |= (1ULL << (p[i] & 63));
                        } else {
                            cur.assign(p, p + sz);
                        }
                    }
                }

                for (size_t i = 0; i < it.op.size(); ++i) {
                    Op o = it.op[i];
                    uint32_t ti = it.t[i + 1];
                    uint32_t bsi = (ti != ~0u) ? bitset_map[ti] : ~0u;

                    if (bsi != ~0u) {
                        const uint64_t* tb = bitsets[bsi].data();
                        if (!in_bitset) {
                            if (o == Op::OR) {
                                in_bitset = true;
                                std::memcpy(cb.data(), tb, NW * sizeof(uint64_t));
                                for (uint32_t d : cur) cb[d >> 6] |= (1ULL << (d & 63));
                            } else if (o == Op::AND) {
                                size_t out_sz = 0;
                                for (uint32_t d : cur) {
                                    if (tb[d >> 6] & (1ULL << (d & 63))) cur[out_sz++] = d;
                                }
                                cur.resize(out_sz);
                            } else {
                                size_t out_sz = 0;
                                for (uint32_t d : cur) {
                                    if (!(tb[d >> 6] & (1ULL << (d & 63)))) cur[out_sz++] = d;
                                }
                                cur.resize(out_sz);
                            }
                        } else {
                            if (o == Op::AND) {
                                for (size_t w = 0; w < NW; ++w) cb[w] &= tb[w];
                            } else if (o == Op::OR) {
                                for (size_t w = 0; w < NW; ++w) cb[w] |= tb[w];
                            } else {
                                for (size_t w = 0; w < NW; ++w) cb[w] &= ~tb[w];
                            }
                        }
                        continue;
                    }

                    auto [pl, psz] = (ti != ~0u) ? get_pos(ti) : std::pair<const uint32_t*, size_t>{nullptr, 0};

                    if (!in_bitset && o == Op::OR && (cur.size() + psz > 16384)) {
                        in_bitset = true;
                        std::fill(cb.begin(), cb.end(), 0);
                        for (uint32_t d : cur) cb[d >> 6] |= (1ULL << (d & 63));
                    }

                    if (in_bitset) {
                        if (o == Op::AND) {
                            cur.clear();
                            for (size_t k = 0; k < psz; ++k) {
                                uint32_t d = pl[k];
                                if (cb[d >> 6] & (1ULL << (d & 63))) cur.push_back(d);
                            }
                            in_bitset = false;
                        } else if (o == Op::OR) {
                            for (size_t k = 0; k < psz; ++k) cb[pl[k] >> 6] |= (1ULL << (pl[k] & 63));
                        } else {
                            for (size_t k = 0; k < psz; ++k) cb[pl[k] >> 6] &= ~(1ULL << (pl[k] & 63));
                        }
                    } else {
                        nxt.clear();
                        const uint32_t* a = cur.data();
                        size_t asz = cur.size(), ia = 0, ib = 0;
                        if (o == Op::AND) {
                            while (ia < asz && ib < psz) {
                                if (a[ia] == pl[ib]) { nxt.push_back(a[ia]); ++ia; ++ib; }
                                else if (a[ia] < pl[ib]) ++ia;
                                else ++ib;
                            }
                        } else if (o == Op::OR) {
                            while (ia < asz && ib < psz) {
                                if (a[ia] == pl[ib]) { nxt.push_back(a[ia]); ++ia; ++ib; }
                                else if (a[ia] < pl[ib]) nxt.push_back(a[ia++]);
                                else nxt.push_back(pl[ib++]);
                            }
                            while (ia < asz) nxt.push_back(a[ia++]);
                            while (ib < psz) nxt.push_back(pl[ib++]);
                        } else {
                            while (ia < asz && ib < psz) {
                                if (a[ia] == pl[ib]) { ++ia; ++ib; }
                                else if (a[ia] < pl[ib]) nxt.push_back(a[ia++]);
                                else ++ib;
                            }
                            while (ia < asz) nxt.push_back(a[ia++]);
                        }
                        cur.swap(nxt);
                    }
                }

                uint32_t cnt = 0, f10[10], n10 = 0;
                uint64_t isum = 0;
                if (in_bitset) {
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
                } else {
                    cnt = uint32_t(cur.size());
                    n10 = std::min<uint32_t>(10, cnt);
                    for (uint32_t i = 0; i < n10; ++i) f10[i] = cur[i];
                    for (uint32_t d : cur) isum += d;
                }
                fmt_res(rs, cnt, isum, f10, n10);
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

                uint32_t rarest_t = ts[ord[0]];
                uint32_t bsi0 = bitset_map[rarest_t];
                if (bsi0 != ~0u) {
                    cur.clear();
                    const uint64_t* tb = bitsets[bsi0].data();
                    for (size_t w = 0; w < NW; ++w) {
                        uint64_t wrd = tb[w];
                        while (wrd) {
                            cur.push_back(uint32_t(w * 64 + __builtin_ctzll(wrd)));
                            wrd &= wrd - 1;
                        }
                    }
                } else {
                    auto [first_p, first_sz] = get_pos(rarest_t);
                    cur.assign(first_p, first_p + first_sz);
                }

                for (size_t idx = 1; idx < K && !cur.empty(); ++idx) {
                    uint32_t ti = ts[ord[idx]];
                    if (ti == ts[ord[idx - 1]]) continue;
                    uint32_t bsi = bitset_map[ti];
                    if (bsi != ~0u) {
                        const uint64_t* tb = bitsets[bsi].data();
                        size_t o = 0;
                        for (uint32_t d : cur) {
                            if (tb[d >> 6] & (1ULL << (d & 63))) cur[o++] = d;
                        }
                        cur.resize(o);
                    } else {
                        auto [pl, psz] = get_pos(ti);
                        size_t o = 0;
                        const uint32_t* search_beg = pl;
                        const uint32_t* search_end = pl + psz;
                        for (uint32_t d : cur) {
                            search_beg = std::lower_bound(search_beg, search_end, d);
                            if (search_beg == search_end) break;
                            if (*search_beg == d) cur[o++] = d;
                        }
                        cur.resize(o);
                    }
                }

                uint32_t cnt = 0, f10[10], n10 = 0;
                uint64_t isum = 0;
                size_t w0_len = rw[0].size();
                uint8_t w0_c0 = uint8_t(rw[0][0]);

                for (uint32_t d : cur) {
                    const char* tp = dr[d].p, * te = tp + dr[d].l;
                    bool ok = false;
                    while (tp < te) {
                        while (tp < te && !CHAR_MAP[(uint8_t)*tp]) ++tp;
                        if (tp >= te) break;
                        const char* st = tp;
                        while (tp < te && CHAR_MAP[(uint8_t)*tp]) ++tp;
                        if (size_t(tp - st) == w0_len && CHAR_MAP[(uint8_t)st[0]] == w0_c0) {
                            bool m0 = true;
                            for (size_t i = 1; i < w0_len; ++i) {
                                if (CHAR_MAP[(uint8_t)st[i]] != uint8_t(rw[0][i])) { m0 = false; break; }
                            }
                            if (m0) {
                                const char* p2 = tp;
                                bool all_m = true;
                                for (size_t j = 1; j < K; ++j) {
                                    while (p2 < te && !CHAR_MAP[(uint8_t)*p2]) ++p2;
                                    if (p2 >= te) { all_m = false; break; }
                                    const char* st2 = p2;
                                    while (p2 < te && CHAR_MAP[(uint8_t)*p2]) ++p2;
                                    if (size_t(p2 - st2) != rw[j].size()) { all_m = false; break; }
                                    for (size_t i = 0; i < rw[j].size(); ++i) {
                                        if (CHAR_MAP[(uint8_t)st2[i]] != uint8_t(rw[j][i])) { all_m = false; break; }
                                    }
                                    if (!all_m) break;
                                }
                                if (all_m) { ok = true; break; }
                            }
                        }
                    }
                    if (ok) {
                        cnt++;
                        isum += d;
                        if (n10 < 10) f10[n10++] = d;
                    }
                }
                fmt_res(rs, cnt, isum, f10, n10);
            }
        }
    });

    std::string out;
    out.reserve(NQ * 32);
    for (size_t q = 0; q < NQ; ++q) {
        out.append(res[q]);
        out += '\n';
    }
    int ofd = open((std::string(od) + "/results.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    [[maybe_unused]] auto wr = write(ofd, out.data(), out.size());
    close(ofd);
    _exit(0);
}
