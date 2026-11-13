#include "server/server_options.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <system_error>
#include <vector>

#include "utils/strings.h"

namespace redis_simple {
namespace {
constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;
// Matches Redis' io-threads limit.
constexpr int kMaxIoThreads = 128;

bool ParsePort(std::string_view value, int* const port) {
  if (value.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
      parsed < kMinPort || parsed > kMaxPort) {
    return false;
  }
  *port = parsed;
  return true;
}

bool ParseIoThreads(std::string_view value, int* const io_threads) {
  int parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (value.empty() || result.ec != std::errc() ||
      result.ptr != value.data() + value.size() || parsed < 1 ||
      parsed > kMaxIoThreads) {
    return false;
  }
  *io_threads = parsed;
  return true;
}

bool ParseYesNo(std::string_view value, bool* const enabled) {
  if (value == "yes") {
    *enabled = true;
    return true;
  }
  if (value == "no") {
    *enabled = false;
    return true;
  }
  return false;
}

bool ParseFsyncPolicy(std::string_view value, aof::FsyncPolicy* const policy) {
  if (value == "always") {
    *policy = aof::FsyncPolicy::kAlways;
    return true;
  }
  if (value == "everysec") {
    *policy = aof::FsyncPolicy::kEverySecond;
    return true;
  }
  if (value == "no") {
    *policy = aof::FsyncPolicy::kNo;
    return true;
  }
  return false;
}

// Redis' save: "<seconds> <changes>" pairs separated by spaces, with
// seconds at least 1, or "" for no save points.
bool ParseSavePoints(std::string_view value, ServerOptions* const options) {
  if (!options->save_points_given) {
    options->save_points_given = true;
    options->rdb_options.save_points.clear();
  }
  if (value.empty()) {
    options->rdb_options.save_points.clear();
    return true;
  }
  std::vector<int64_t> numbers;
  for (const auto part : std::views::split(value, ' ')) {
    int64_t number = 0;
    if (!utils::ToInt64(std::string_view(part.begin(), part.end()), &number)) {
      return false;
    }
    numbers.push_back(number);
  }
  if (numbers.size() % 2 != 0) {
    return false;
  }
  for (size_t i = 0; i < numbers.size(); i += 2) {
    if (numbers[i] < 1 || numbers[i + 1] < 0) {
      return false;
    }
  }
  for (size_t i = 0; i < numbers.size(); i += 2) {
    options->rdb_options.save_points.push_back(
        {.seconds = numbers[i], .changes = numbers[i + 1]});
  }
  return true;
}

bool ParseSize(std::string_view value, size_t* const result) {
  if (value.empty()) {
    return false;
  }
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), *result);
  return parsed.ec == std::errc() && parsed.ptr == value.data() + value.size();
}

// Each option takes one value. `apply` stores it and returns false if it is
// invalid, in which case parsing fails with `error`.
struct OptionSpec {
  std::string_view name;
  bool (*apply)(std::string_view value, ServerOptions* options);
  std::string_view error;
};

constexpr auto kOptionSpecs = std::to_array<OptionSpec>({
    {.name = "--bind",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->bind_address.assign(value.data(), value.size());
           return !value.empty();
         },
     .error = "bind address cannot be empty"},
    {.name = "--port",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParsePort(value, &options->port);
         },
     .error = "port must be between 1 and 65535"},
    {.name = "--dir",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->dir.assign(value.data(), value.size());
           return !value.empty();
         },
     .error = "dir cannot be empty"},
    {.name = "--dbfilename",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->rdb_options.file_name.assign(value.data(), value.size());
           // A name only, as in Redis; --dir picks the directory.
           return !value.empty() &&
                  value.find_first_of("/\\") == std::string_view::npos;
         },
     .error = "dbfilename can't be a path, just a filename"},
    {.name = "--rdbcompression",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->rdb_options.compression);
         },
     .error = "rdbcompression must be yes or no"},
    {.name = "--rdbchecksum",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->rdb_options.checksum);
         },
     .error = "rdbchecksum must be yes or no"},
    {.name = "--aof-use-rdb-preamble",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->aof_options.use_rdb_preamble);
         },
     .error = "aof-use-rdb-preamble must be yes or no"},
    {.name = "--save",
     .apply = ParseSavePoints,
     .error = "Invalid save parameters"},
    {.name = "--stop-writes-on-bgsave-error",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->rdb_options.stop_writes_on_error);
         },
     .error = "stop-writes-on-bgsave-error must be yes or no"},
    {.name = "--appendonly",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->append_only);
         },
     .error = "appendonly must be yes or no"},
    {.name = "--appendfilename",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->aof_options.file_name.assign(value.data(), value.size());
           // A name only, as in Redis; --appenddirname picks the directory.
           return !value.empty() &&
                  value.find_first_of("/\\") == std::string_view::npos;
         },
     .error = "appendfilename can't be a path, just a filename"},
    {.name = "--aof-load-truncated",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->aof_options.load_truncated);
         },
     .error = "aof-load-truncated must be yes or no"},
    {.name = "--aof-timestamp-enabled",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->aof_options.timestamp_enabled);
         },
     .error = "aof-timestamp-enabled must be yes or no"},
    {.name = "--aof-rewrite-incremental-fsync",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value,
                             &options->aof_options.rewrite_incremental_fsync);
         },
     .error = "aof-rewrite-incremental-fsync must be yes or no"},
    {.name = "--no-appendfsync-on-rewrite",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->aof_options.no_fsync_on_rewrite);
         },
     .error = "no-appendfsync-on-rewrite must be yes or no"},
    {.name = "--aof-load-corrupt-tail-max-size",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseSize(value,
                            &options->aof_options.load_corrupt_tail_max_bytes);
         },
     .error = "aof-load-corrupt-tail-max-size must be a byte count"},
    {.name = "--appenddirname",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->aof_options.directory.assign(value.data(), value.size());
           return !value.empty();
         },
     .error = "appenddirname cannot be empty"},
    {.name = "--appendfsync",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseFsyncPolicy(value, &options->aof_options.fsync);
         },
     .error = "appendfsync must be always, everysec, or no"},
    {.name = "--auto-aof-rewrite-min-size",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseSize(value,
                            &options->aof_options.auto_rewrite_min_bytes);
         },
     .error = "auto AOF rewrite minimum size must be a byte count"},
    {.name = "--auto-aof-rewrite-percentage",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseSize(value,
                            &options->aof_options.auto_rewrite_percentage);
         },
     .error = "auto AOF rewrite percentage must be an integer"},
    {.name = "--requirepass",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->require_pass.assign(value.data(), value.size());
           return true;
         },
     .error = ""},
    {.name = "--protected-mode",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->protected_mode);
         },
     .error = "protected-mode must be yes or no"},
    {.name = "--io-threads",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseIoThreads(value, &options->io_threads);
         },
     .error = "io-threads must be between 1 and 128"},
});

const OptionSpec* FindOption(std::string_view name) {
  const auto* const spec =
      std::ranges::find(kOptionSpecs, name, &OptionSpec::name);
  return spec == kOptionSpecs.end() ? nullptr : spec;
}

OptionsResult Fail(std::string_view error) {
  OptionsResult result;
  result.status = OptionsStatus::kError;
  result.error = error;
  return result;
}
}  // namespace

OptionsResult ParseServerOptions(int argc, const char* const* argv) {
  OptionsResult result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help" || option == "-h") {
      result.status = OptionsStatus::kHelp;
      return result;
    }
    const OptionSpec* const spec = FindOption(option);
    if (spec == nullptr) {
      return Fail("unknown option");
    }
    if (++index >= argc) {
      return Fail("missing option value");
    }
    if (!spec->apply(argv[index], &result.options)) {
      return Fail(spec->error);
    }
  }
  return result;
}

std::string_view ServerUsage() {
  return "Usage: redis_simple [--bind <address>] [--port <port>] "
         "[--dir <directory>] [--dbfilename <name>] "
         "[--rdbcompression <yes|no>] [--rdbchecksum <yes|no>] "
         "[--save <\"seconds changes ...\">] "
         "[--stop-writes-on-bgsave-error <yes|no>] "
         "[--appendonly <yes|no>] [--appendfilename <name>] "
         "[--appenddirname <dir>] "
         "[--aof-load-truncated <yes|no>] "
         "[--aof-use-rdb-preamble <yes|no>] "
         "[--aof-load-corrupt-tail-max-size <bytes>] "
         "[--aof-timestamp-enabled <yes|no>] "
         "[--aof-rewrite-incremental-fsync <yes|no>] "
         "[--no-appendfsync-on-rewrite <yes|no>] "
         "[--appendfsync <always|everysec|no>] "
         "[--auto-aof-rewrite-min-size <bytes>] "
         "[--auto-aof-rewrite-percentage <percent>] "
         "[--requirepass <password>] [--protected-mode <yes|no>] "
         "[--io-threads <count>]\n";
}
}  // namespace redis_simple
