#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class MappedFile {
  const char *data_ = nullptr;
  size_t size_ = 0;

public:
  explicit MappedFile(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::system_error(errno, std::generic_category(), "open " + path);
    struct stat st {};
    if (fstat(fd, &st) != 0) {
      int error = errno;
      close(fd);
      throw std::system_error(error, std::generic_category(), "stat " + path);
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_) {
      void *mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
      int error = errno;
      close(fd);
      if (mapped == MAP_FAILED)
        throw std::system_error(error, std::generic_category(), "mmap " + path);
      data_ = static_cast<const char *>(mapped);
    } else {
      close(fd);
    }
  }
  ~MappedFile() {
    if (data_)
      munmap(const_cast<char *>(data_), size_);
  }
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;
  const char *data() const { return data_; }
  size_t size() const { return size_; }
};

template <class Function> void parallel(size_t workers, Function work) {
  if (workers <= 1) {
    work(0);
    return;
  }
  std::vector<std::thread> threads;
  threads.reserve(workers - 1);
  for (size_t i = 1; i < workers; ++i)
    threads.emplace_back(work, i);
  work(0);
  for (auto &thread : threads)
    thread.join();
}

inline uint8_t token_char(char c) {
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c);
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c);
  if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c + 32);
  return 0;
}

struct TokenSpan {
  const char *ptr = nullptr;
  uint16_t len = 0;
};

inline bool match_token(TokenSpan span, std::string_view term) {
  if (span.len != term.size()) return false;
  for (size_t i = 0; i < span.len; ++i) {
    if (token_char(span.ptr[i]) != static_cast<uint8_t>(term[i])) {
      return false;
    }
  }
  return true;
}

enum class Op : uint8_t { AND, OR, NOT };

struct BoolQuery {
  std::vector<uint32_t> term_ids;
  std::vector<Op> ops;
};

struct PhraseQuery {
  std::vector<uint32_t> term_ids;
  std::vector<std::string> raw_terms;
};

struct QueryItem {
  bool is_phrase = false;
  BoolQuery bq;
  PhraseQuery pq;
};

struct DictEntry {
  uint32_t hash = 0;
  uint32_t id = 0; // 0 = empty, else id + 1
};

constexpr size_t TableSize = 262144;
constexpr uint32_t TableMask = TableSize - 1;

struct TermDict {
  std::vector<DictEntry> table;
  std::vector<std::string> terms;

  TermDict() : table(TableSize, DictEntry{0, 0}) {}

  uint32_t intern(std::string_view s) {
    uint32_t h = 2166136261u;
    for (char c : s) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    uint32_t idx = h & TableMask;
    while (table[idx].id != 0) {
      uint32_t tid = table[idx].id - 1;
      if (table[idx].hash == h && terms[tid] == s) {
        return tid;
      }
      idx = (idx + 1) & TableMask;
    }
    uint32_t id = static_cast<uint32_t>(terms.size());
    terms.emplace_back(s);
    table[idx].hash = h;
    table[idx].id = id + 1;
    return id;
  }

  uint32_t find(const char *s, size_t len) const {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
    uint32_t idx = h & TableMask;
    while (table[idx].id != 0) {
      uint32_t tid = table[idx].id - 1;
      if (table[idx].hash == h && terms[tid].size() == len &&
          std::memcmp(terms[tid].data(), s, len) == 0) {
        return tid;
      }
      idx = (idx + 1) & TableMask;
    }
    return 0xFFFFFFFF;
  }
};

struct DocRef {
  const char *ptr = nullptr;
  uint32_t len = 0;
};

void run(const std::string &data_dir, const std::string &out_dir) {
  size_t workers = 12;
  if (const char *setting = std::getenv("SD_THREADS")) {
    std::string_view text(setting);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || workers < 1 || workers > 12)
      throw std::runtime_error("SD_THREADS must be an integer from 1 through 12");
  }

  // 1. Read queries.txt
  MappedFile queries_file(data_dir + "/queries.txt");
  const char *qp = queries_file.data();
  const char *qend = qp + queries_file.size();

  TermDict dict;
  std::vector<QueryItem> queries;
  queries.reserve(50000);

  while (qp < qend) {
    while (qp < qend && (*qp == '\n' || *qp == '\r')) ++qp;
    if (qp >= qend) break;

    const char *line_start = qp;
    while (qp < qend && *qp != '\n') ++qp;
    const char *line_end = qp;
    if (qp < qend && *qp == '\n') ++qp;

    std::string_view line(line_start, line_end - line_start);
    if (line.starts_with("BOOL ")) {
      QueryItem q;
      q.is_phrase = false;
      size_t pos = 5;
      bool expect_op = false;
      while (pos < line.size()) {
        size_t next_sp = line.find(' ', pos);
        if (next_sp == std::string_view::npos) next_sp = line.size();
        std::string_view word = line.substr(pos, next_sp - pos);
        if (expect_op) {
          if (word == "AND") q.bq.ops.push_back(Op::AND);
          else if (word == "OR") q.bq.ops.push_back(Op::OR);
          else if (word == "NOT") q.bq.ops.push_back(Op::NOT);
          expect_op = false;
        } else {
          uint32_t tid = dict.intern(word);
          q.bq.term_ids.push_back(tid);
          expect_op = true;
        }
        pos = next_sp + 1;
      }
      queries.push_back(std::move(q));
    } else if (line.starts_with("PHRASE \"")) {
      QueryItem q;
      q.is_phrase = true;
      size_t pos = 8;
      size_t quote_end = line.rfind('"');
      if (quote_end == std::string_view::npos) quote_end = line.size();
      while (pos < quote_end) {
        size_t next_sp = line.find(' ', pos);
        if (next_sp == std::string_view::npos || next_sp > quote_end) next_sp = quote_end;
        std::string_view word = line.substr(pos, next_sp - pos);
        uint32_t tid = dict.intern(word);
        q.pq.term_ids.push_back(tid);
        q.pq.raw_terms.emplace_back(word);
        pos = next_sp + 1;
      }
      queries.push_back(std::move(q));
    }
  }

  const size_t num_terms = dict.terms.size();

  // 2. Read docs.txt
  MappedFile docs_file(data_dir + "/docs.txt");
  const char *dp = docs_file.data();
  const char *dend = dp + docs_file.size();

  std::vector<DocRef> doc_refs;
  doc_refs.reserve(260000);
  {
    const char *p = dp;
    while (p < dend) {
      while (p < dend && *p != '\t') ++p;
      if (p >= dend) break;
      ++p; // skip '\t'
      const char *text_start = p;
      while (p < dend && *p != '\n') ++p;
      const char *text_end = p;
      if (p < dend && *p == '\n') ++p;
      doc_refs.push_back({text_start, static_cast<uint32_t>(text_end - text_start)});
    }
  }

  const size_t num_docs = doc_refs.size();
  const size_t num_words = (num_docs + 63) / 64;

  std::vector<std::vector<std::vector<uint32_t>>> worker_postings(workers);
  for (size_t w = 0; w < workers; ++w) {
    worker_postings[w].resize(num_terms);
  }

  parallel(workers, [&](size_t wid) {
    size_t d_start = num_docs * wid / workers;
    size_t d_end = num_docs * (wid + 1) / workers;

    std::vector<uint32_t> local_doc_terms;
    local_doc_terms.reserve(256);
    char token_buf[64];

    for (size_t doc_id = d_start; doc_id < d_end; ++doc_id) {
      local_doc_terms.clear();
      const char *tp = doc_refs[doc_id].ptr;
      const char *tend = tp + doc_refs[doc_id].len;

      while (tp < tend) {
        while (tp < tend && token_char(*tp) == 0) ++tp;
        if (tp >= tend) break;

        size_t tlen = 0;
        while (tp < tend) {
          uint8_t c = token_char(*tp);
          if (c == 0) break;
          if (tlen < 63) token_buf[tlen++] = static_cast<char>(c);
          ++tp;
        }

        uint32_t tid = dict.find(token_buf, tlen);
        if (tid != 0xFFFFFFFF) {
          local_doc_terms.push_back(tid);
        }
      }

      std::sort(local_doc_terms.begin(), local_doc_terms.end());
      local_doc_terms.erase(std::unique(local_doc_terms.begin(), local_doc_terms.end()),
                            local_doc_terms.end());

      for (uint32_t tid : local_doc_terms) {
        worker_postings[wid][tid].push_back(static_cast<uint32_t>(doc_id));
      }
    }
  });

  // Consolidate postings
  std::vector<std::vector<uint32_t>> postings(num_terms);
  for (size_t t = 0; t < num_terms; ++t) {
    size_t total_size = 0;
    for (size_t w = 0; w < workers; ++w) total_size += worker_postings[w][t].size();
    postings[t].reserve(total_size);
    for (size_t w = 0; w < workers; ++w) {
      postings[t].insert(postings[t].end(), worker_postings[w][t].begin(),
                         worker_postings[w][t].end());
    }
  }

  // Prebuild bitsets for frequent terms (df >= 256)
  std::vector<std::vector<uint64_t>> bitsets(num_terms);
  for (size_t t = 0; t < num_terms; ++t) {
    if (postings[t].size() >= 256) {
      bitsets[t].assign(num_words, 0);
      for (uint32_t doc_id : postings[t]) {
        bitsets[t][doc_id >> 6] |= (1ULL << (doc_id & 63));
      }
    }
  }

  const size_t num_queries = queries.size();
  std::vector<std::string> results(num_queries);
  std::atomic<size_t> query_counter{0};

  parallel(workers, [&](size_t) {
    std::vector<uint64_t> cur_bits(num_words, 0);
    std::vector<uint64_t> tmp_bits(num_words, 0);
    std::vector<uint32_t> phrase_candidates;
    std::vector<TokenSpan> doc_tokens;
    doc_tokens.reserve(512);
    char buf[32];

    for (size_t q = query_counter.fetch_add(1); q < num_queries; q = query_counter.fetch_add(1)) {
      const QueryItem &item = queries[q];
      std::string &res = results[q];

      if (!item.is_phrase) {
        // BOOL query
        const auto &terms = item.bq.term_ids;
        const auto &ops = item.bq.ops;

        // Init cur_bits with first term
        std::fill(cur_bits.begin(), cur_bits.end(), 0);
        uint32_t t0 = terms[0];
        if (t0 != 0xFFFFFFFF) {
          if (!bitsets[t0].empty()) {
            std::copy(bitsets[t0].begin(), bitsets[t0].end(), cur_bits.begin());
          } else {
            for (uint32_t doc_id : postings[t0]) {
              cur_bits[doc_id >> 6] |= (1ULL << (doc_id & 63));
            }
          }
        }

        for (size_t i = 0; i < ops.size(); ++i) {
          Op op = ops[i];
          uint32_t ti = terms[i + 1];

          if (ti == 0xFFFFFFFF || postings[ti].empty()) {
            if (op == Op::AND) {
              std::fill(cur_bits.begin(), cur_bits.end(), 0);
            }
            continue;
          }

          if (!bitsets[ti].empty()) {
            const auto &b = bitsets[ti];
            if (op == Op::AND) {
              for (size_t w = 0; w < num_words; ++w) cur_bits[w] &= b[w];
            } else if (op == Op::OR) {
              for (size_t w = 0; w < num_words; ++w) cur_bits[w] |= b[w];
            } else if (op == Op::NOT) {
              for (size_t w = 0; w < num_words; ++w) cur_bits[w] &= ~b[w];
            }
          } else {
            const auto &plist = postings[ti];
            if (op == Op::AND) {
              std::fill(tmp_bits.begin(), tmp_bits.end(), 0);
              for (uint32_t doc_id : plist) {
                if (cur_bits[doc_id >> 6] & (1ULL << (doc_id & 63))) {
                  tmp_bits[doc_id >> 6] |= (1ULL << (doc_id & 63));
                }
              }
              std::copy(tmp_bits.begin(), tmp_bits.end(), cur_bits.begin());
            } else if (op == Op::OR) {
              for (uint32_t doc_id : plist) {
                cur_bits[doc_id >> 6] |= (1ULL << (doc_id & 63));
              }
            } else if (op == Op::NOT) {
              for (uint32_t doc_id : plist) {
                cur_bits[doc_id >> 6] &= ~(1ULL << (doc_id & 63));
              }
            }
          }
        }

        // Extract answers
        uint32_t count = 0;
        uint64_t id_sum = 0;
        uint32_t first10[10];
        uint32_t n10 = 0;

        for (size_t w = 0; w < num_words; ++w) {
          uint64_t mask = cur_bits[w];
          if (!mask) continue;
          count += static_cast<uint32_t>(__builtin_popcountll(mask));
          size_t base = w << 6;
          while (mask) {
            int bit = __builtin_ctzll(mask);
            uint32_t doc_id = static_cast<uint32_t>(base + bit);
            id_sum += doc_id;
            if (n10 < 10) first10[n10++] = doc_id;
            mask &= (mask - 1);
          }
        }

        auto r1 = std::to_chars(buf, buf + sizeof(buf), count);
        res.append(buf, r1.ptr);
        res.push_back(' ');
        auto r2 = std::to_chars(buf, buf + sizeof(buf), id_sum);
        res.append(buf, r2.ptr);
        for (uint32_t idx = 0; idx < n10; ++idx) {
          res.push_back(' ');
          auto r3 = std::to_chars(buf, buf + sizeof(buf), first10[idx]);
          res.append(buf, r3.ptr);
        }
      } else {
        // PHRASE query
        const auto &terms = item.pq.term_ids;
        const auto &raw_terms = item.pq.raw_terms;
        const size_t k = terms.size();

        bool has_empty = false;
        for (uint32_t tid : terms) {
          if (tid == 0xFFFFFFFF || postings[tid].empty()) {
            has_empty = true;
            break;
          }
        }

        if (has_empty) {
          res = "0 0";
          continue;
        }

        // Find rarest term to seed candidate list
        size_t min_idx = 0;
        size_t min_sz = postings[terms[0]].size();
        for (size_t i = 1; i < k; ++i) {
          if (postings[terms[i]].size() < min_sz) {
            min_sz = postings[terms[i]].size();
            min_idx = i;
          }
        }

        phrase_candidates = postings[terms[min_idx]];

        // Filter candidates against other terms
        for (size_t i = 0; i < k; ++i) {
          if (i == min_idx) continue;
          uint32_t tid = terms[i];
          if (!bitsets[tid].empty()) {
            const auto &b = bitsets[tid];
            size_t w_out = 0;
            for (uint32_t doc_id : phrase_candidates) {
              if (b[doc_id >> 6] & (1ULL << (doc_id & 63))) {
                phrase_candidates[w_out++] = doc_id;
              }
            }
            phrase_candidates.resize(w_out);
          } else {
            const auto &plist = postings[tid];
            size_t w_out = 0;
            for (uint32_t doc_id : phrase_candidates) {
              if (std::binary_search(plist.begin(), plist.end(), doc_id)) {
                phrase_candidates[w_out++] = doc_id;
              }
            }
            phrase_candidates.resize(w_out);
          }
          if (phrase_candidates.empty()) break;
        }

        uint32_t count = 0;
        uint64_t id_sum = 0;
        uint32_t first10[10];
        uint32_t n10 = 0;

        // Verify candidates by scanning tokens in document
        for (uint32_t doc_id : phrase_candidates) {
          const char *tp = doc_refs[doc_id].ptr;
          const char *tend = tp + doc_refs[doc_id].len;

          doc_tokens.clear();

          while (tp < tend) {
            while (tp < tend && token_char(*tp) == 0) ++tp;
            if (tp >= tend) break;
            const char *t_start = tp;
            while (tp < tend && token_char(*tp) != 0) ++tp;
            doc_tokens.push_back({t_start, static_cast<uint16_t>(tp - t_start)});
          }

          if (doc_tokens.size() < k) continue;

          bool matched = false;
          for (size_t p = 0; p + k <= doc_tokens.size(); ++p) {
            bool eq = true;
            for (size_t j = 0; j < k; ++j) {
              if (!match_token(doc_tokens[p + j], raw_terms[j])) {
                eq = false;
                break;
              }
            }
            if (eq) {
              matched = true;
              break;
            }
          }

          if (matched) {
            count++;
            id_sum += doc_id;
            if (n10 < 10) first10[n10++] = doc_id;
          }
        }

        auto r1 = std::to_chars(buf, buf + sizeof(buf), count);
        res.append(buf, r1.ptr);
        res.push_back(' ');
        auto r2 = std::to_chars(buf, buf + sizeof(buf), id_sum);
        res.append(buf, r2.ptr);
        for (uint32_t idx = 0; idx < n10; ++idx) {
          res.push_back(' ');
          auto r3 = std::to_chars(buf, buf + sizeof(buf), first10[idx]);
          res.append(buf, r3.ptr);
        }
      }
    }
  });

  // Write output
  std::string out;
  out.reserve(num_queries * 32);
  for (size_t q = 0; q < num_queries; ++q) {
    out.append(results[q]);
    out.push_back('\n');
  }

  std::filesystem::create_directories(out_dir);
  FILE *file = std::fopen((out_dir + "/results.txt").c_str(), "wb");
  if (!file)
    throw std::runtime_error("open output failed");
  if (std::fwrite(out.data(), 1, out.size(), file) != out.size())
    throw std::runtime_error("fwrite failed");
  if (std::fclose(file) != 0)
    throw std::runtime_error("fclose failed");
}

int main(int argc, char **argv) {
  try {
    if (argc != 1 && argc != 3)
      throw std::runtime_error("usage: index [data-directory output-directory]");
    run(argc == 3 ? argv[1] : "/data", argc == 3 ? argv[2] : "/out");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

