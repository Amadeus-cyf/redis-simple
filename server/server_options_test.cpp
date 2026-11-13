#include "server/server_options.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace redis_simple {
TEST(ServerOptionsTest, UsesDefaults) {
  constexpr std::array kArgv = {"redis_simple"};

  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.bind_address, "127.0.0.1");
  EXPECT_EQ(result.options.port, 8080);
  EXPECT_FALSE(result.options.append_only);
  EXPECT_EQ(result.options.aof_options.file_name, "appendonly.aof");
  EXPECT_EQ(result.options.aof_options.directory, "appendonlydir");
  EXPECT_EQ(result.options.aof_options.fsync, aof::FsyncPolicy::kEverySecond);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_min_bytes,
            size_t{64} * 1024 * 1024);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_percentage, 100);
  EXPECT_TRUE(result.options.require_pass.empty());
  EXPECT_TRUE(result.options.protected_mode);
  EXPECT_EQ(result.options.io_threads, 1);
  // Redis 8's save points when started without a config file.
  EXPECT_EQ(result.options.rdb_options.save_points,
            (std::vector<rdb::SavePoint>{{3600, 1}, {300, 100}, {60, 10000}}));
  EXPECT_TRUE(result.options.rdb_options.stop_writes_on_error);
  EXPECT_TRUE(result.options.aof_options.use_rdb_preamble);
}

TEST(ServerOptionsTest, ParsesAofUseRdbPreamble) {
  constexpr std::array kArgv = {"redis_simple", "--aof-use-rdb-preamble", "no"};
  OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());
  ASSERT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_FALSE(result.options.aof_options.use_rdb_preamble);

  constexpr std::array kInvalid = {"redis_simple", "--aof-use-rdb-preamble",
                                   "maybe"};
  result = ParseServerOptions(kInvalid.size(), kInvalid.data());
  EXPECT_EQ(result.status, OptionsStatus::kError);
  EXPECT_EQ(result.error, "aof-use-rdb-preamble must be yes or no");
}

TEST(ServerOptionsTest, ParsesSavePoints) {
  // The first --save replaces the defaults and later ones add to them, as
  // save lines in a Redis config file do.
  constexpr std::array kArgv = {"redis_simple", "--save", "900 1 300 10",
                                "--save", "60 0"};
  OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());
  ASSERT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.rdb_options.save_points,
            (std::vector<rdb::SavePoint>{{900, 1}, {300, 10}, {60, 0}}));

  // An empty --save turns them off.
  constexpr std::array kOff = {"redis_simple", "--save", "",
                               "--stop-writes-on-bgsave-error", "no"};
  result = ParseServerOptions(kOff.size(), kOff.data());
  ASSERT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_TRUE(result.options.rdb_options.save_points.empty());
  EXPECT_FALSE(result.options.rdb_options.stop_writes_on_error);

  // Pairs only, seconds at least 1, and changes at least 0.
  for (const char* const save : {"60", "0 1", "60 -1", "60 x", "60  1"}) {
    const std::array argv = {"redis_simple", "--save", save};
    result = ParseServerOptions(argv.size(), argv.data());
    EXPECT_EQ(result.status, OptionsStatus::kError) << save;
    EXPECT_EQ(result.error, "Invalid save parameters");
  }
}

TEST(ServerOptionsTest, ParsesIoThreads) {
  constexpr std::array kArgv = {"redis_simple", "--io-threads", "4"};
  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());
  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.io_threads, 4);

  constexpr std::array kMax = {"redis_simple", "--io-threads", "128"};
  EXPECT_EQ(ParseServerOptions(kMax.size(), kMax.data()).options.io_threads,
            128);
  for (const char* const invalid : {"0", "129", "-1", "four", "4x", ""}) {
    const std::array argv = {"redis_simple", "--io-threads", invalid};
    EXPECT_EQ(ParseServerOptions(argv.size(), argv.data()).status,
              OptionsStatus::kError)
        << invalid;
  }
}

TEST(ServerOptionsTest, ParsesSecurityOptions) {
  constexpr std::array kArgv = {"redis_simple", "--requirepass", "s3cret",
                                "--protected-mode", "no"};

  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.require_pass, "s3cret");
  EXPECT_FALSE(result.options.protected_mode);

  constexpr std::array kInvalid = {"redis_simple", "--protected-mode", "off"};
  EXPECT_EQ(ParseServerOptions(kInvalid.size(), kInvalid.data()).status,
            OptionsStatus::kError);
}

TEST(ServerOptionsTest, ParsesBindAddressAndPort) {
  constexpr std::array kArgv = {"redis_simple", "--bind", "0.0.0.0", "--port",
                                "6380"};

  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.bind_address, "0.0.0.0");
  EXPECT_EQ(result.options.port, 6380);
}

TEST(ServerOptionsTest, ParsesAofLoadCorruptTailMaxSize) {
  const std::array argv = {"redis_simple", "--aof-load-corrupt-tail-max-size",
                           "4096"};
  const OptionsResult result = ParseServerOptions(argv.size(), argv.data());
  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.aof_options.load_corrupt_tail_max_bytes, 4096);
  EXPECT_EQ(ParseServerOptions(1, argv.data())
                .options.aof_options.load_corrupt_tail_max_bytes,
            0);
}

TEST(ServerOptionsTest, RejectsAppendFileNamesWithDirectories) {
  // As in Redis: a file name, not a path. Spaces are fine; the manifest
  // quotes them.
  for (const char* name : {"dir/appendonly.aof", "/tmp/appendonly.aof",
                           "dir\\appendonly.aof", ""}) {
    const std::array argv = {"redis_simple", "--appendfilename", name};
    EXPECT_EQ(ParseServerOptions(argv.size(), argv.data()).status,
              OptionsStatus::kError)
        << name;
  }
  const std::array argv = {"redis_simple", "--appendfilename",
                           "append only.aof"};
  EXPECT_EQ(ParseServerOptions(argv.size(), argv.data()).status,
            OptionsStatus::kOk);
}

TEST(ServerOptionsTest, ParsesRedisAofSwitches) {
  constexpr std::array kArgv = {
      "redis_simple", "--aof-load-truncated",
      "no",           "--aof-timestamp-enabled",
      "yes",          "--aof-rewrite-incremental-fsync",
      "no",           "--no-appendfsync-on-rewrite",
      "yes"};
  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());
  ASSERT_EQ(result.status, OptionsStatus::kOk);
  const aof::Options& options = result.options.aof_options;
  EXPECT_FALSE(options.load_truncated);
  EXPECT_TRUE(options.timestamp_enabled);
  EXPECT_FALSE(options.rewrite_incremental_fsync);
  EXPECT_TRUE(options.no_fsync_on_rewrite);
  // Redis' defaults.
  const aof::Options defaults =
      ParseServerOptions(1, kArgv.data()).options.aof_options;
  EXPECT_TRUE(defaults.load_truncated);
  EXPECT_FALSE(defaults.timestamp_enabled);
  EXPECT_TRUE(defaults.rewrite_incremental_fsync);
  EXPECT_FALSE(defaults.no_fsync_on_rewrite);
}

TEST(ServerOptionsTest, ParsesAofOptions) {
  constexpr std::array kArgv = {"redis_simple",
                                "--appendonly",
                                "yes",
                                "--appendfilename",
                                "data.aof",
                                "--appenddirname",
                                "/tmp/aof",
                                "--appendfsync",
                                "always",
                                "--auto-aof-rewrite-min-size",
                                "4096",
                                "--auto-aof-rewrite-percentage",
                                "50"};

  const OptionsResult result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_TRUE(result.options.append_only);
  EXPECT_EQ(result.options.aof_options.file_name, "data.aof");
  EXPECT_EQ(result.options.aof_options.directory, "/tmp/aof");
  EXPECT_EQ(result.options.aof_options.fsync, aof::FsyncPolicy::kAlways);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_min_bytes, 4096);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_percentage, 50);
}

TEST(ServerOptionsTest, HandlesHelpAndInvalidArguments) {
  constexpr std::array kHelp = {"redis_simple", "--help"};
  EXPECT_EQ(ParseServerOptions(kHelp.size(), kHelp.data()).status,
            OptionsStatus::kHelp);

  constexpr std::array kInvalidPort = {"redis_simple", "--port", "65536"};
  EXPECT_EQ(ParseServerOptions(kInvalidPort.size(), kInvalidPort.data()).status,
            OptionsStatus::kError);

  constexpr std::array kUnknown = {"redis_simple", "--unknown"};
  EXPECT_EQ(ParseServerOptions(kUnknown.size(), kUnknown.data()).status,
            OptionsStatus::kError);

  constexpr std::array kMissing = {"redis_simple", "--port"};
  EXPECT_EQ(ParseServerOptions(kMissing.size(), kMissing.data()).status,
            OptionsStatus::kError);

  constexpr std::array kInvalidAof = {"redis_simple", "--appendonly", "on"};
  EXPECT_EQ(ParseServerOptions(kInvalidAof.size(), kInvalidAof.data()).status,
            OptionsStatus::kError);

  constexpr std::array kInvalidFsync = {"redis_simple", "--appendfsync",
                                        "sometimes"};
  EXPECT_EQ(
      ParseServerOptions(kInvalidFsync.size(), kInvalidFsync.data()).status,
      OptionsStatus::kError);

  constexpr std::array kInvalidRewriteSize = {
      "redis_simple", "--auto-aof-rewrite-min-size", "many"};
  EXPECT_EQ(
      ParseServerOptions(kInvalidRewriteSize.size(), kInvalidRewriteSize.data())
          .status,
      OptionsStatus::kError);
}
}  // namespace redis_simple
