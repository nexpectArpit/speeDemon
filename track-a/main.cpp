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
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class Input {
  const char *data_ = nullptr;
  size_t size_ = 0;

public:
  explicit Input(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::system_error(errno, std::generic_category(), "open input");
    struct stat st {};
    if (fstat(fd, &st) != 0) {
      int error = errno;
      close(fd);
      throw std::system_error(error, std::generic_category(), "stat input");
    }
    size_ = static_cast<size_t>(st.st_size);
    if (size_) {
      void *mapped = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
      int error = errno;
      close(fd);
      if (mapped == MAP_FAILED)
        throw std::system_error(error, std::generic_category(), "map input");
      data_ = static_cast<const char *>(mapped);
    } else {
      close(fd);
    }
  }
  ~Input() {
    if (data_)
      munmap(const_cast<char *>(data_), size_);
  }
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;
  const char *data() const { return data_; }
  size_t size() const { return size_; }
};

constexpr size_t Hours = 72;
constexpr size_t TableSize = 8192;
constexpr uint32_t TableMask = TableSize - 1;

struct DictEntry {
  uint32_t hash = 0;
  uint16_t len = 0;
  uint16_t id = 0;
  const char *ptr = nullptr;
};

struct EndpointItem {
  std::string_view name;
};

struct Record {
  uint32_t latency;
  uint16_t ep_id;
  uint8_t error;
};

struct WorkerShard {
  DictEntry table[TableSize];
  std::vector<EndpointItem> endpoints;
  std::array<std::vector<Record>, Hours> hour_records;
  std::array<std::string, Hours> hours;

  uint16_t intern(const char *s, uint16_t len) {
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < len; ++i) {
      h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
    }
    uint32_t idx = h & TableMask;
    while (table[idx].ptr != nullptr) {
      if (table[idx].len == len && table[idx].hash == h &&
          std::memcmp(table[idx].ptr, s, len) == 0) {
        return table[idx].id;
      }
      idx = (idx + 1) & TableMask;
    }
    uint16_t id = static_cast<uint16_t>(endpoints.size());
    endpoints.push_back({std::string_view(s, len)});
    table[idx].hash = h;
    table[idx].len = len;
    table[idx].id = id;
    table[idx].ptr = s;
    return id;
  }
};

inline size_t hour_index(const char *ts) {
  int day = (ts[8] - '0') * 10 + ts[9] - '0';
  int days = (ts[6] == '0') ? (day - 30) : (day + 1);
  int hour = (ts[11] - '0') * 10 + ts[12] - '0';
  return static_cast<size_t>(days * 24 + hour - 18);
}

void parse_chunk(const char *cursor, const char *end, WorkerShard &shard) {
  for (size_t h = 0; h < Hours; ++h) {
    shard.hour_records[h].reserve(4096);
  }
  while (cursor < end) {
    const char *line = cursor;
    size_t hour = hour_index(line);
    if (shard.hours[hour].empty()) {
      shard.hours[hour] = std::string(line, 13) + ":00:00Z";
    }
    cursor += 28;
    while (*cursor != ' ') ++cursor;
    const char *ep_start = ++cursor;
    while (*cursor != ' ') ++cursor;
    uint16_t ep_len = static_cast<uint16_t>(cursor - ep_start);
    uint16_t ep_id = shard.intern(ep_start, ep_len);

    bool is_error = (cursor[1] == '5');
    cursor += 5;
    uint32_t latency = 0;
    while (*cursor != '\n') {
      latency = latency * 10 + static_cast<uint32_t>(*cursor - '0');
      ++cursor;
    }
    ++cursor;
    shard.hour_records[hour].push_back({latency, ep_id, static_cast<uint8_t>(is_error)});
  }
}

template <class Function> void parallel(size_t workers, Function work) {
  std::vector<std::thread> threads;
  for (size_t i = 1; i < workers; ++i)
    threads.emplace_back(work, i);
  work(0);
  for (auto &thread : threads)
    thread.join();
}

inline void write_uint64(std::string &out, uint64_t v) {
  char buf[24];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, res.ptr);
}

void run(const std::string &data_dir, const std::string &out_dir) {
  Input input(data_dir + "/requests.log");
  size_t workers = 12;
  if (const char *setting = std::getenv("SD_THREADS")) {
    std::string_view text(setting);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || workers < 1 || workers > 12)
      throw std::runtime_error("SD_THREADS must be an integer from 1 through 12");
  }

  std::vector<WorkerShard> shards(workers);
  if (input.size()) {
    std::vector<const char *> boundaries(workers + 1);
    boundaries[0] = input.data();
    boundaries[workers] = input.data() + input.size();
    for (size_t i = 1; i < workers; ++i) {
      const char *p = input.data() + input.size() * i / workers;
      while (p < boundaries[workers] && p[-1] != '\n')
        ++p;
      boundaries[i] = p;
    }
    parallel(workers, [&](size_t i) {
      parse_chunk(boundaries[i], boundaries[i + 1], shards[i]);
    });
  }

  // Phase 1: Unify endpoints
  struct GlobalEndpoint {
    std::string_view name;
  };
  std::vector<GlobalEndpoint> global_endpoints;
  std::unordered_map<std::string_view, uint16_t> ep_dict;
  std::array<std::string, Hours> global_hours;

  for (auto &shard : shards) {
    for (size_t h = 0; h < Hours; ++h) {
      if (!shard.hours[h].empty())
        global_hours[h] = shard.hours[h];
    }
    for (auto &ep : shard.endpoints) {
      if (ep_dict.try_emplace(ep.name, static_cast<uint16_t>(global_endpoints.size())).second) {
        global_endpoints.push_back({ep.name});
      }
    }
  }

  // Sort global endpoints alphabetically
  std::sort(global_endpoints.begin(), global_endpoints.end(), [](const auto &a, const auto &b) {
    return a.name < b.name;
  });

  // Re-map ep_dict to sorted index
  for (uint16_t i = 0; i < static_cast<uint16_t>(global_endpoints.size()); ++i) {
    ep_dict[global_endpoints[i].name] = i;
  }

  // Build per-shard local_id -> global_sorted_id lookup tables
  std::vector<std::vector<uint16_t>> shard_remap(workers);
  for (size_t w = 0; w < workers; ++w) {
    shard_remap[w].resize(shards[w].endpoints.size());
    for (size_t i = 0; i < shards[w].endpoints.size(); ++i) {
      shard_remap[w][i] = ep_dict[shards[w].endpoints[i].name];
    }
  }

  // Phase 2: Process hours in parallel
  const size_t num_endpoints = global_endpoints.size();
  std::array<std::string, Hours> output_buffers;
  std::atomic<size_t> hour_counter{0};

  parallel(workers, [&](size_t) {
    std::vector<uint32_t> ep_count(num_endpoints, 0);
    std::vector<uint32_t> ep_errors(num_endpoints, 0);
    std::vector<uint32_t> ep_offset(num_endpoints + 1, 0);
    std::vector<uint32_t> latencies;

    for (size_t h = hour_counter.fetch_add(1); h < Hours; h = hour_counter.fetch_add(1)) {
      if (global_hours[h].empty())
        continue;

      // Count total records and records per endpoint
      size_t total_hour_records = 0;
      std::fill(ep_count.begin(), ep_count.end(), 0);
      std::fill(ep_errors.begin(), ep_errors.end(), 0);

      for (size_t w = 0; w < workers; ++w) {
        const auto &records = shards[w].hour_records[h];
        total_hour_records += records.size();
        const auto &remap = shard_remap[w];
        for (const auto &rec : records) {
          uint16_t gid = remap[rec.ep_id];
          ep_count[gid]++;
          ep_errors[gid] += rec.error;
        }
      }

      if (total_hour_records == 0)
        continue;

      // Compute prefix offsets
      ep_offset[0] = 0;
      for (size_t e = 0; e < num_endpoints; ++e) {
        ep_offset[e + 1] = ep_offset[e] + ep_count[e];
      }

      latencies.resize(total_hour_records);
      // Scatter latencies into continuous endpoint slots
      // Temporarily use ep_count as current write offsets
      std::vector<uint32_t> cur_offset = ep_offset;
      for (size_t w = 0; w < workers; ++w) {
        const auto &records = shards[w].hour_records[h];
        const auto &remap = shard_remap[w];
        for (const auto &rec : records) {
          uint16_t gid = remap[rec.ep_id];
          latencies[cur_offset[gid]++] = rec.latency;
        }
      }

      // Format output for this hour
      std::string &buf = output_buffers[h];
      buf.reserve(total_hour_records * 40);

      for (size_t e = 0; e < num_endpoints; ++e) {
        uint32_t count = ep_count[e];
        if (count == 0) continue;

        uint32_t start_idx = ep_offset[e];
        uint32_t *vals = latencies.data() + start_idx;

        size_t r50 = (50ULL * count + 99) / 100 - 1;
        size_t r95 = (95ULL * count + 99) / 100 - 1;
        size_t r99 = (99ULL * count + 99) / 100 - 1;

        uint32_t p50_val, p95_val, p99_val;
        if (count <= 32) {
          std::sort(vals, vals + count);
          p50_val = vals[r50];
          p95_val = vals[r95];
          p99_val = vals[r99];
        } else {
          std::nth_element(vals, vals + r50, vals + count);
          p50_val = vals[r50];
          if (r95 > r50) {
            std::nth_element(vals + r50 + 1, vals + r95, vals + count);
          }
          p95_val = vals[r95];
          if (r99 > r95) {
            std::nth_element(vals + r95 + 1, vals + r99, vals + count);
          }
          p99_val = vals[r99];
        }

        uint32_t errors = ep_errors[e];
        uint64_t ppm = (static_cast<uint64_t>(errors) * 1000000ULL) / count;

        buf.append(global_hours[h]);
        buf.push_back(' ');
        buf.append(global_endpoints[e].name);
        buf.push_back(' ');
        write_uint64(buf, count);
        buf.push_back(' ');
        write_uint64(buf, p50_val);
        buf.push_back(' ');
        write_uint64(buf, p95_val);
        buf.push_back(' ');
        write_uint64(buf, p99_val);
        buf.push_back(' ');
        write_uint64(buf, errors);
        buf.push_back(' ');
        write_uint64(buf, ppm);
        buf.push_back('\n');
      }
    }
  });

  std::filesystem::create_directories(out_dir);
  FILE *file = std::fopen((out_dir + "/summary.txt").c_str(), "wb");
  if (!file)
    throw std::runtime_error("open output failed");
  bool ok = true;
  for (auto &buffer : output_buffers) {
    if (!buffer.empty()) {
      if (std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size())
        ok = false;
    }
  }
  if (std::fclose(file) != 0 || !ok)
    throw std::runtime_error("write output failed");
}

int main(int argc, char **argv) {
  try {
    if (argc != 1 && argc != 3)
      throw std::runtime_error("usage: ingest [data-directory output-directory]");
    run(argc == 3 ? argv[1] : "/data", argc == 3 ? argv[2] : "/out");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
