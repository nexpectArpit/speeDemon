#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

constexpr uint64_t INF = UINT64_MAX;

struct Edge {
  uint32_t to = 0;
  uint32_t weight = 0;
};

struct RawEdge {
  uint32_t u = 0;
  uint32_t v = 0;
  uint32_t w = 0;
};

struct Query {
  uint32_t s = 0;
  uint32_t t = 0;
};

struct HeapEntry {
  uint64_t dist;
  uint32_t u;
};

struct FastMinHeap {
  std::vector<HeapEntry> data;

  FastMinHeap() { data.reserve(16384); }
  bool empty() const { return data.empty(); }
  void clear() { data.clear(); }
  size_t size() const { return data.size(); }
  const HeapEntry &top() const { return data[0]; }

  void push(HeapEntry item) {
    data.push_back(item);
    size_t i = data.size() - 1;
    while (i > 0) {
      size_t parent = (i - 1) >> 1;
      if (data[i].dist < data[parent].dist) {
        std::swap(data[i], data[parent]);
        i = parent;
      } else {
        break;
      }
    }
  }

  HeapEntry pop() {
    HeapEntry top = data[0];
    HeapEntry back = data.back();
    data.pop_back();
    if (!data.empty()) {
      data[0] = back;
      size_t i = 0;
      size_t n = data.size();
      while (true) {
        size_t left = (i << 1) + 1;
        size_t right = left + 1;
        size_t smallest = i;
        if (left < n && data[left].dist < data[smallest].dist) smallest = left;
        if (right < n && data[right].dist < data[smallest].dist) smallest = right;
        if (smallest != i) {
          std::swap(data[i], data[smallest]);
          i = smallest;
        } else {
          break;
        }
      }
    }
    return top;
  }
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

inline uint32_t parse_u32(const char *&p, const char *end) {
  uint32_t val = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    val = val * 10 + (*p - '0');
    ++p;
  }
  return val;
}

inline void write_int64(std::string &out, int64_t v) {
  if (v < 0) {
    out.append("-1");
    return;
  }
  char buf[24];
  auto res = std::to_chars(buf, buf + sizeof(buf), static_cast<uint64_t>(v));
  out.append(buf, res.ptr);
}

void run(const std::string &data_dir, const std::string &out_dir) {
  size_t workers = 12;
  if (const char *setting = std::getenv("SD_THREADS")) {
    std::string_view text(setting);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || workers < 1 || workers > 12)
      throw std::runtime_error("SD_THREADS must be an integer from 1 through 12");
  }

  // 1. Read graph.txt
  MappedFile graph_file(data_dir + "/graph.txt");
  const char *gp = graph_file.data();
  const char *gend = gp + graph_file.size();

  uint32_t n = parse_u32(gp, gend);
  if (gp < gend && *gp == ' ') ++gp;
  uint32_t m = parse_u32(gp, gend);
  while (gp < gend && *gp != '\n') ++gp;
  if (gp < gend && *gp == '\n') ++gp;

  const char *edges_start = gp;
  size_t edges_bytes = gend - edges_start;

  // Split edges_bytes across workers at newline boundaries
  std::vector<const char *> boundaries(workers + 1, gend);
  boundaries[0] = edges_start;
  for (size_t i = 1; i < workers; ++i) {
    const char *p = edges_start + edges_bytes * i / workers;
    while (p < gend && *(p - 1) != '\n') ++p;
    boundaries[i] = p;
  }

  std::vector<std::vector<RawEdge>> local_raw_edges(workers);
  parallel(workers, [&](size_t wid) {
    const char *scan = boundaries[wid];
    const char *limit = boundaries[wid + 1];
    auto &vec = local_raw_edges[wid];
    vec.reserve((m / workers) + 4096);

    while (scan < limit) {
      if (*scan == '\n' || *scan == '\r') {
        ++scan;
        continue;
      }
      uint32_t u = parse_u32(scan, limit);
      if (scan < limit && *scan == ' ') ++scan;
      uint32_t v = parse_u32(scan, limit);
      if (scan < limit && *scan == ' ') ++scan;
      uint32_t w = parse_u32(scan, limit);
      while (scan < limit && *scan != '\n') ++scan;
      if (scan < limit && *scan == '\n') ++scan;

      vec.push_back({u, v, w});
    }
  });

  std::vector<uint32_t> out_degree(n, 0);
  std::vector<uint32_t> in_degree(n, 0);

  for (size_t w = 0; w < workers; ++w) {
    for (const auto &re : local_raw_edges[w]) {
      if (re.u < n) out_degree[re.u]++;
      if (re.v < n) in_degree[re.v]++;
    }
  }

  // Build forward and reverse CSR offsets
  std::vector<uint32_t> fwd_offset(n + 1, 0);
  std::vector<uint32_t> rev_offset(n + 1, 0);
  for (uint32_t i = 0; i < n; ++i) {
    fwd_offset[i + 1] = fwd_offset[i] + out_degree[i];
    rev_offset[i + 1] = rev_offset[i] + in_degree[i];
  }

  std::vector<uint32_t> fwd_cur = fwd_offset;
  std::vector<uint32_t> rev_cur = rev_offset;
  std::vector<Edge> fwd_edges(m);
  std::vector<Edge> rev_edges(m);

  for (size_t w = 0; w < workers; ++w) {
    for (const auto &re : local_raw_edges[w]) {
      if (re.u < n && re.v < n) {
        uint32_t fpos = fwd_cur[re.u]++;
        fwd_edges[fpos] = {re.v, re.w};
        uint32_t rpos = rev_cur[re.v]++;
        rev_edges[rpos] = {re.u, re.w};
      }
    }
    // Free local memory as soon as consumed
    std::vector<RawEdge>().swap(local_raw_edges[w]);
  }

  // 2. Read queries.txt
  MappedFile queries_file(data_dir + "/queries.txt");
  const char *qp = queries_file.data();
  const char *qend = qp + queries_file.size();

  std::vector<Query> queries;
  queries.reserve(4096);
  while (qp < qend) {
    if (*qp == '\n' || *qp == '\r') {
      ++qp;
      continue;
    }
    uint32_t s = parse_u32(qp, qend);
    if (qp < qend && *qp == ' ') ++qp;
    uint32_t t = parse_u32(qp, qend);
    while (qp < qend && *qp != '\n') ++qp;
    if (qp < qend && *qp == '\n') ++qp;
    queries.push_back({s, t});
  }

  const size_t num_queries = queries.size();
  std::vector<int64_t> answers(num_queries, -1);

  struct WorkerState {
    std::vector<uint64_t> fwd_dist;
    std::vector<uint64_t> rev_dist;
    std::vector<uint32_t> visited;
    FastMinHeap fwd_heap;
    FastMinHeap rev_heap;

    explicit WorkerState(size_t num_nodes)
        : fwd_dist(num_nodes, INF), rev_dist(num_nodes, INF) {
      visited.reserve(16384);
    }
  };

  std::vector<WorkerState> worker_states;
  worker_states.reserve(workers);
  for (size_t w = 0; w < workers; ++w) {
    worker_states.emplace_back(n);
  }

  std::atomic<size_t> query_idx{0};

  parallel(workers, [&](size_t wid) {
    WorkerState &state = worker_states[wid];
    auto &fwd_dist = state.fwd_dist;
    auto &rev_dist = state.rev_dist;
    auto &visited = state.visited;
    auto &fwd_heap = state.fwd_heap;
    auto &rev_heap = state.rev_heap;

    for (size_t q = query_idx.fetch_add(1); q < num_queries; q = query_idx.fetch_add(1)) {
      uint32_t s = queries[q].s;
      uint32_t t = queries[q].t;

      if (s == t) {
        answers[q] = 0;
        continue;
      }
      if (s >= n || t >= n || out_degree[s] == 0 || in_degree[t] == 0) {
        answers[q] = -1;
        continue;
      }

      fwd_dist[s] = 0;
      rev_dist[t] = 0;
      visited.push_back(s);
      visited.push_back(t);
      fwd_heap.push({0, s});
      rev_heap.push({0, t});

      uint64_t mu = INF;

      while (!fwd_heap.empty() && !rev_heap.empty()) {
        uint64_t fwd_min = fwd_heap.top().dist;
        uint64_t rev_min = rev_heap.top().dist;

        if (fwd_min + rev_min >= mu) {
          break;
        }

        if (fwd_min <= rev_min) {
          HeapEntry top = fwd_heap.pop();
          uint64_t d = top.dist;
          uint32_t u = top.u;
          if (d > fwd_dist[u]) continue;
          if (d >= mu) continue;

          uint32_t start_e = fwd_offset[u];
          uint32_t end_e = fwd_offset[u + 1];
          for (uint32_t e = start_e; e < end_e; ++e) {
            uint32_t v = fwd_edges[e].to;
            uint64_t nd = d + fwd_edges[e].weight;
            if (nd < fwd_dist[v]) {
              if (fwd_dist[v] == INF && rev_dist[v] == INF) {
                visited.push_back(v);
              }
              fwd_dist[v] = nd;
              fwd_heap.push({nd, v});
              if (rev_dist[v] != INF) {
                uint64_t path_len = nd + rev_dist[v];
                if (path_len < mu) mu = path_len;
              }
            }
          }
        } else {
          HeapEntry top = rev_heap.pop();
          uint64_t d = top.dist;
          uint32_t v = top.u;
          if (d > rev_dist[v]) continue;
          if (d >= mu) continue;

          uint32_t start_e = rev_offset[v];
          uint32_t end_e = rev_offset[v + 1];
          for (uint32_t e = start_e; e < end_e; ++e) {
            uint32_t u = rev_edges[e].to;
            uint64_t nd = d + rev_edges[e].weight;
            if (nd < rev_dist[u]) {
              if (fwd_dist[u] == INF && rev_dist[u] == INF) {
                visited.push_back(u);
              }
              rev_dist[u] = nd;
              rev_heap.push({nd, u});
              if (fwd_dist[u] != INF) {
                uint64_t path_len = fwd_dist[u] + nd;
                if (path_len < mu) mu = path_len;
              }
            }
          }
        }
      }

      answers[q] = (mu == INF) ? -1 : static_cast<int64_t>(mu);

      for (uint32_t node : visited) {
        fwd_dist[node] = INF;
        rev_dist[node] = INF;
      }
      visited.clear();
      fwd_heap.clear();
      rev_heap.clear();
    }
  });

  // Format output
  std::string out;
  out.reserve(num_queries * 12);
  for (size_t q = 0; q < num_queries; ++q) {
    write_int64(out, answers[q]);
    out.push_back('\n');
  }

  std::filesystem::create_directories(out_dir);
  FILE *file = std::fopen((out_dir + "/distances.txt").c_str(), "wb");
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
      throw std::runtime_error("usage: traverse [data-directory output-directory]");
    run(argc == 3 ? argv[1] : "/data", argc == 3 ? argv[2] : "/out");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

