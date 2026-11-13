#ifndef REDIS_SIMPLE_SERVER_SERVER_OPTIONS_H_
#define REDIS_SIMPLE_SERVER_SERVER_OPTIONS_H_

#include <string>
#include <string_view>

#include "server/aof/aof.h"
#include "server/rdb/rdb.h"

namespace redis_simple {
struct ServerOptions {
  std::string bind_address{"127.0.0.1"};
  int port{8080};
  // Redis' dir: the working directory, where the RDB file and the AOF live.
  // Empty keeps the directory the server started in.
  std::string dir;
  bool append_only{};
  aof::Options aof_options;
  rdb::Options rdb_options;
  // Whether --save was given: the first replaces Redis' default save points,
  // and later ones add to them, as save lines in a Redis config file do.
  bool save_points_given{};
  // Clients must AUTH with this password when it is non-empty.
  std::string require_pass;
  // Without a password, refuse clients that do not connect over loopback.
  bool protected_mode{true};
  // Threads that read requests and write replies, counting the main thread,
  // which still runs every command. 1 does all I/O on the main thread.
  int io_threads{1};
};

enum class OptionsStatus {
  kOk,
  kHelp,
  kError,
};

struct OptionsResult {
  ServerOptions options;
  OptionsStatus status{OptionsStatus::kOk};
  std::string_view error;
};

OptionsResult ParseServerOptions(int argc, const char* const* argv);
std::string_view ServerUsage();
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_SERVER_OPTIONS_H_
