#include "server/server_options.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <string_view>
#include <system_error>

namespace redis_simple {
namespace {
constexpr int kMinPort = 1;
constexpr int kMaxPort = 65535;

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
    {.name = "--appendonly",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           return ParseYesNo(value, &options->append_only);
         },
     .error = "appendonly must be yes or no"},
    {.name = "--appendfilename",
     .apply =
         [](std::string_view value, ServerOptions* options) {
           options->aof_options.path.assign(value.data(), value.size());
           return !value.empty();
         },
     .error = "appendfilename cannot be empty"},
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
         "[--appendonly <yes|no>] [--appendfilename <path>] "
         "[--appendfsync <always|everysec|no>] "
         "[--auto-aof-rewrite-min-size <bytes>] "
         "[--auto-aof-rewrite-percentage <percent>] "
         "[--requirepass <password>] [--protected-mode <yes|no>]\n";
}
}  // namespace redis_simple
