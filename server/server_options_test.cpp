#include "server/server_options.h"

#include <gtest/gtest.h>

#include <array>

namespace redis_simple {
TEST(ServerOptionsTest, UsesDefaults) {
  constexpr std::array kArgv = {"redis_simple"};

  const auto result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.bind_address, "127.0.0.1");
  EXPECT_EQ(result.options.port, 8080);
  EXPECT_FALSE(result.options.append_only);
  EXPECT_EQ(result.options.aof_options.path, "appendonly.aof");
  EXPECT_EQ(result.options.aof_options.fsync, aof::FsyncPolicy::kEverySecond);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_min_bytes,
            size_t{64} * 1024 * 1024);
  EXPECT_EQ(result.options.aof_options.auto_rewrite_percentage, 100);
  EXPECT_TRUE(result.options.require_pass.empty());
  EXPECT_TRUE(result.options.protected_mode);
}

TEST(ServerOptionsTest, ParsesSecurityOptions) {
  constexpr std::array kArgv = {"redis_simple", "--requirepass", "s3cret",
                                "--protected-mode", "no"};

  const auto result = ParseServerOptions(kArgv.size(), kArgv.data());

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

  const auto result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_EQ(result.options.bind_address, "0.0.0.0");
  EXPECT_EQ(result.options.port, 6380);
}

TEST(ServerOptionsTest, ParsesAofOptions) {
  constexpr std::array kArgv = {"redis_simple",
                                "--appendonly",
                                "yes",
                                "--appendfilename",
                                "/tmp/appendonly.aof",
                                "--appendfsync",
                                "always",
                                "--auto-aof-rewrite-min-size",
                                "4096",
                                "--auto-aof-rewrite-percentage",
                                "50"};

  const auto result = ParseServerOptions(kArgv.size(), kArgv.data());

  EXPECT_EQ(result.status, OptionsStatus::kOk);
  EXPECT_TRUE(result.options.append_only);
  EXPECT_EQ(result.options.aof_options.path, "/tmp/appendonly.aof");
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
