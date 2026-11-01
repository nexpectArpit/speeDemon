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

constexpr size_t InitialLookupRegions = 1000000;
constexpr size_t LocalTableSize = 262144;
constexpr uint32_t LocalTableMask = LocalTableSize - 1;
constexpr size_t GlobalTableSize = 524288;
constexpr uint32_t GlobalTableMask = GlobalTableSize - 1;

struct CountryInfo {
  std::string name;
  uint16_t id = 0;
};

struct Aggregates {
  uint64_t count = 0;
  int64_t id_sum = 0;
  int64_t amount_sum = 0;
  int64_t amount_min = INT64_MAX;
  int64_t amount_max = INT64_MIN;
  uint64_t name_bytes = 0;
  uint64_t note_bytes = 0;
  uint64_t names_distinct = 0;
};

struct DictEntry {
  uint32_t hash = 0;
  uint32_t id = 0; // 0 = empty, otherwise (name_id + 1)
};

struct WorkerState {
  std::vector<Aggregates> aggs;
  std::vector<DictEntry> table;
  std::vector<std::string> local_names;
  std::vector<std::vector<uint64_t>> country_seen;
  std::string unescape_buf;

  WorkerState() : table(LocalTableSize, DictEntry{0, 0}) {}

  uint32_t intern(const char *s, uint16_t len) {
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < len; ++i) {
      h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
    }
    uint32_t idx = h & LocalTableMask;
    while (table[idx].id != 0) {
      uint32_t lid = table[idx].id - 1;
      const std::string &existing = local_names[lid];
      if (existing.size() == len && table[idx].hash == h &&
          std::memcmp(existing.data(), s, len) == 0) {
        return lid;
      }
      idx = (idx + 1) & LocalTableMask;
    }
    uint32_t id = static_cast<uint32_t>(local_names.size());
    local_names.emplace_back(s, len);
    table[idx].hash = h;
    table[idx].id = id + 1;
    return id;
  }

  void mark_seen(uint16_t country_id, uint32_t name_id) {
    size_t word_idx = name_id >> 6;
    if (word_idx >= country_seen[country_id].size()) {
      country_seen[country_id].resize(std::max(word_idx + 1, country_seen[country_id].size() * 2), 0);
    }
    country_seen[country_id][word_idx] |= (1ULL << (name_id & 63));
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

inline void write_uint64(std::string &out, uint64_t v) {
  char buf[24];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, res.ptr);
}

inline void write_cents(std::string &out, int64_t cents) {
  if (cents < 0) {
    out.push_back('-');
    cents = -cents;
  }
  uint64_t whole = static_cast<uint64_t>(cents / 100);
  uint64_t frac = static_cast<uint64_t>(cents % 100);
  write_uint64(out, whole);
  out.push_back('.');
  out.push_back(static_cast<char>('0' + (frac / 10)));
  out.push_back(static_cast<char>('0' + (frac % 10)));
}

void run(const std::string &data_dir, const std::string &out_dir) {
  // 1. Load regions.csv
  MappedFile regions_file(data_dir + "/regions.csv");
  std::vector<uint16_t> region_to_country(InitialLookupRegions, 65535);
  std::vector<CountryInfo> countries;
  uint16_t unmatched_id = 0;

  {
    const char *p = regions_file.data();
    const char *end = p + regions_file.size();
    // Skip header "region_id,country\n"
    while (p < end && *p != '\n') ++p;
    if (p < end && *p == '\n') ++p;

    auto get_or_add_country = [&](std::string_view name) -> uint16_t {
      for (size_t i = 0; i < countries.size(); ++i) {
        if (countries[i].name == name) return countries[i].id;
      }
      uint16_t id = static_cast<uint16_t>(countries.size());
      countries.push_back({std::string(name), id});
      return id;
    };

    while (p < end) {
      uint32_t reg_id = 0;
      while (p < end && *p >= '0' && *p <= '9') {
        reg_id = reg_id * 10 + (*p - '0');
        ++p;
      }
      if (p >= end || *p != ',') break;
      ++p;
      const char *c_start = p;
      while (p < end && *p != '\n' && *p != '\r') ++p;
      std::string_view c_name(c_start, p - c_start);
      if (p < end && *p == '\r') ++p;
      if (p < end && *p == '\n') ++p;

      uint16_t cid = get_or_add_country(c_name);
      if (reg_id >= region_to_country.size()) {
        region_to_country.resize(std::max(static_cast<size_t>(reg_id + 1), region_to_country.size() * 2), 65535);
      }
      region_to_country[reg_id] = cid;
    }

    // Add _UNMATCHED country
    unmatched_id = static_cast<uint16_t>(countries.size());
    countries.push_back({"_UNMATCHED", unmatched_id});
  }

  const size_t num_countries = countries.size();

  // 2. Map records.csv
  MappedFile records_file(data_dir + "/records.csv");
  const char *data = records_file.data();
  size_t size = records_file.size();

  size_t workers = 12;
  if (const char *setting = std::getenv("SD_THREADS")) {
    std::string_view text(setting);
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || workers < 1 || workers > 12)
      throw std::runtime_error("SD_THREADS must be an integer from 1 through 12");
  }

  // Skip leading BOM and header on chunk 0
  size_t file_start = 0;
  if (size >= 3 && static_cast<uint8_t>(data[0]) == 0xEF &&
      static_cast<uint8_t>(data[1]) == 0xBB && static_cast<uint8_t>(data[2]) == 0xBF) {
    file_start = 3;
  }
  // Skip header "id,name,note,region_id,amount" + terminator
  while (file_start < size && data[file_start] != '\n') ++file_start;
  if (file_start < size && data[file_start] == '\n') ++file_start;

  if (size - file_start < 65536) {
    workers = 1;
  }

  // Split boundaries
  std::vector<const char *> boundaries(workers + 1, data + size);
  boundaries[0] = data + file_start;

  for (size_t i = 1; i < workers; ++i) {
    size_t pos = file_start + (size - file_start) * i / workers;
    bool found = false;
    while (pos < size) {
      if (data[pos - 1] == '\n') {
        size_t p = pos;
        if (p + 3 <= size && static_cast<uint8_t>(data[p]) == 0xEF &&
            static_cast<uint8_t>(data[p + 1]) == 0xBB && static_cast<uint8_t>(data[p + 2]) == 0xBF) {
          p += 3;
        }
        size_t q = p;
        while (q < size && data[q] >= '0' && data[q] <= '9') ++q;
        if (q > p && q < size && data[q] == ',') {
          size_t prev = pos - 1;
          if (prev > 0 && data[prev - 1] == '\r') --prev;
          if (prev >= 3 && data[prev - 3] == '.' &&
              data[prev - 2] >= '0' && data[prev - 2] <= '9' &&
              data[prev - 1] >= '0' && data[prev - 1] <= '9') {
            boundaries[i] = data + pos;
            found = true;
            break;
          }
        }
      }
      ++pos;
    }
    if (!found) {
      boundaries[i] = data + size;
    }
  }

  for (size_t i = 1; i < workers; ++i) {
    if (boundaries[i] < boundaries[i - 1]) {
      boundaries[i] = boundaries[i - 1];
    }
  }

  std::vector<WorkerState> shards(workers);
  for (size_t w = 0; w < workers; ++w) {
    shards[w].aggs.assign(num_countries, Aggregates{});
    shards[w].country_seen.resize(num_countries);
  }

  // Parse in parallel
  parallel(workers, [&](size_t wid) {
    WorkerState &shard = shards[wid];
    const char *cursor = boundaries[wid];
    const char *end = boundaries[wid + 1];

    while (cursor < end) {
      // Check for mid-file BOM
      if (cursor + 3 <= end && static_cast<uint8_t>(cursor[0]) == 0xEF &&
          static_cast<uint8_t>(cursor[1]) == 0xBB && static_cast<uint8_t>(cursor[2]) == 0xBF) {
        cursor += 3;
      }

      // 1. id
      uint64_t id = 0;
      while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        id = id * 10 + (*cursor - '0');
        ++cursor;
      }
      if (cursor < end && *cursor == ',') ++cursor; // skip ','

      // 2. name
      const char *name_ptr = cursor;
      uint16_t name_len = 0;
      if (cursor < end && *cursor == '"') {
        ++cursor;
        const char *start = cursor;
        bool has_escapes = false;
        while (cursor < end) {
          if (*cursor == '"') {
            if (cursor + 1 < end && cursor[1] == '"') {
              has_escapes = true;
              cursor += 2;
            } else {
              break;
            }
          } else {
            ++cursor;
          }
        }
        const char *close_quote = cursor;
        if (cursor < end && *cursor == '"') ++cursor; // skip '"'
        if (!has_escapes) {
          name_ptr = start;
          name_len = static_cast<uint16_t>(close_quote - start);
        } else {
          shard.unescape_buf.clear();
          shard.unescape_buf.reserve(close_quote - start);
          for (const char *scan = start; scan < close_quote; ++scan) {
            if (*scan == '"' && scan + 1 < close_quote && scan[1] == '"') {
              shard.unescape_buf.push_back('"');
              ++scan;
            } else {
              shard.unescape_buf.push_back(*scan);
            }
          }
          name_ptr = shard.unescape_buf.data();
          name_len = static_cast<uint16_t>(shard.unescape_buf.size());
        }
      } else {
        const char *start = cursor;
        while (cursor < end && *cursor != ',') ++cursor;
        name_ptr = start;
        name_len = static_cast<uint16_t>(cursor - start);
      }
      if (cursor < end && *cursor == ',') ++cursor; // skip ','

      // 3. note
      uint32_t note_len = 0;
      if (cursor < end && *cursor == '"') {
        ++cursor;
        while (cursor < end) {
          if (*cursor == '"') {
            if (cursor + 1 < end && cursor[1] == '"') {
              note_len++;
              cursor += 2;
            } else {
              break;
            }
          } else {
            note_len++;
            ++cursor;
          }
        }
        if (cursor < end && *cursor == '"') ++cursor; // skip '"'
      } else {
        const char *start = cursor;
        while (cursor < end && *cursor != ',') ++cursor;
        note_len = static_cast<uint32_t>(cursor - start);
      }
      if (cursor < end && *cursor == ',') ++cursor; // skip ','

      // 4. region_id
      uint32_t region_id = 0;
      while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        region_id = region_id * 10 + (*cursor - '0');
        ++cursor;
      }
      if (cursor < end && *cursor == ',') ++cursor; // skip ','

      uint16_t cid = (region_id < region_to_country.size()) ? region_to_country[region_id] : unmatched_id;
      if (cid == 65535) cid = unmatched_id;

      // 5. amount
      bool neg = false;
      if (cursor < end && *cursor == '-') {
        neg = true;
        ++cursor;
      }
      int64_t cents = 0;
      while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        cents = cents * 10 + (*cursor - '0');
        ++cursor;
      }
      if (cursor < end && *cursor == '.') {
        ++cursor;
        if (cursor < end) {
          cents = cents * 10 + (*cursor - '0');
          ++cursor;
        }
        if (cursor < end) {
          cents = cents * 10 + (*cursor - '0');
          ++cursor;
        }
      }
      if (neg) cents = -cents;

      // Terminator
      if (cursor < end && *cursor == '\r') ++cursor;
      if (cursor < end && *cursor == '\n') ++cursor;

      // Intern name and record
      uint32_t name_id = shard.intern(name_ptr, name_len);
      shard.mark_seen(cid, name_id);

      Aggregates &a = shard.aggs[cid];
      a.count++;
      a.id_sum += static_cast<int64_t>(id);
      a.amount_sum += cents;
      if (cents < a.amount_min) a.amount_min = cents;
      if (cents > a.amount_max) a.amount_max = cents;
      a.name_bytes += name_len;
      a.note_bytes += note_len;
    }
  });

  // Consolidate scalar aggregates across shards
  std::vector<Aggregates> total_aggs(num_countries);
  for (uint16_t c = 0; c < num_countries; ++c) {
    for (size_t w = 0; w < workers; ++w) {
      const Aggregates &sa = shards[w].aggs[c];
      if (sa.count == 0) continue;
      total_aggs[c].count += sa.count;
      total_aggs[c].id_sum += sa.id_sum;
      total_aggs[c].amount_sum += sa.amount_sum;
      if (sa.amount_min < total_aggs[c].amount_min) total_aggs[c].amount_min = sa.amount_min;
      if (sa.amount_max > total_aggs[c].amount_max) total_aggs[c].amount_max = sa.amount_max;
      total_aggs[c].name_bytes += sa.name_bytes;
      total_aggs[c].note_bytes += sa.note_bytes;
    }
  }

  // Unify global names
  std::vector<std::string> global_names;
  std::vector<DictEntry> global_table(GlobalTableSize, DictEntry{0, 0});

  auto global_intern = [&](const std::string &s) -> uint32_t {
    uint32_t h = 2166136261u;
    for (char ch : s) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    uint32_t idx = h & GlobalTableMask;
    while (global_table[idx].id != 0) {
      uint32_t gid = global_table[idx].id - 1;
      const std::string &existing = global_names[gid];
      if (existing.size() == s.size() && global_table[idx].hash == h &&
          std::memcmp(existing.data(), s.data(), s.size()) == 0) {
        return gid;
      }
      idx = (idx + 1) & GlobalTableMask;
    }
    uint32_t id = static_cast<uint32_t>(global_names.size());
    global_names.push_back(s);
    global_table[idx].hash = h;
    global_table[idx].id = id + 1;
    return id;
  };

  std::vector<std::vector<uint32_t>> shard_remap(workers);
  for (size_t w = 0; w < workers; ++w) {
    shard_remap[w].resize(shards[w].local_names.size());
    for (size_t i = 0; i < shards[w].local_names.size(); ++i) {
      shard_remap[w][i] = global_intern(shards[w].local_names[i]);
    }
  }

  const size_t num_global_names = global_names.size();
  const size_t words_per_country = (num_global_names + 63) / 64;

  // Process distinct names in parallel across countries
  std::atomic<size_t> country_idx{0};
  parallel(workers, [&](size_t) {
    std::vector<uint64_t> country_bits(words_per_country, 0);

    for (size_t c = country_idx.fetch_add(1); c < num_countries; c = country_idx.fetch_add(1)) {
      if (total_aggs[c].count == 0) continue;

      std::fill(country_bits.begin(), country_bits.end(), 0);

      for (size_t w = 0; w < workers; ++w) {
        const auto &seen = shards[w].country_seen[c];
        const auto &remap = shard_remap[w];
        for (size_t widx = 0; widx < seen.size(); ++widx) {
          uint64_t mask = seen[widx];
          if (!mask) continue;
          size_t base_lid = widx << 6;
          while (mask) {
            int bit = __builtin_ctzll(mask);
            uint32_t lid = static_cast<uint32_t>(base_lid + bit);
            uint32_t gid = remap[lid];
            country_bits[gid >> 6] |= (1ULL << (gid & 63));
            mask &= (mask - 1);
          }
        }
      }

      uint64_t distinct = 0;
      for (uint64_t word : country_bits) {
        distinct += static_cast<uint64_t>(__builtin_popcountll(word));
      }
      total_aggs[c].names_distinct = distinct;
    }
  });

  // Sort countries bytewise ascending
  std::vector<uint16_t> sorted_countries;
  for (uint16_t c = 0; c < num_countries; ++c) {
    if (total_aggs[c].count > 0) {
      sorted_countries.push_back(c);
    }
  }

  std::sort(sorted_countries.begin(), sorted_countries.end(), [&](uint16_t a, uint16_t b) {
    return countries[a].name < countries[b].name;
  });

  // Format output
  std::string out;
  out.reserve(sorted_countries.size() * 120);

  for (uint16_t c : sorted_countries) {
    const Aggregates &a = total_aggs[c];
    out.append(countries[c].name);
    out.push_back(' ');
    write_uint64(out, a.count);
    out.push_back(' ');
    write_uint64(out, a.id_sum);
    out.push_back(' ');
    write_cents(out, a.amount_sum);
    out.push_back(' ');
    write_cents(out, a.amount_min);
    out.push_back(' ');
    write_cents(out, a.amount_max);
    out.push_back(' ');
    write_uint64(out, a.names_distinct);
    out.push_back(' ');
    write_uint64(out, a.name_bytes);
    out.push_back(' ');
    write_uint64(out, a.note_bytes);
    out.push_back('\n');
  }

  std::filesystem::create_directories(out_dir);
  FILE *file = std::fopen((out_dir + "/aggregates.txt").c_str(), "wb");
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
      throw std::runtime_error("usage: join [data-directory output-directory]");
    run(argc == 3 ? argv[1] : "/data", argc == 3 ? argv[2] : "/out");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}

