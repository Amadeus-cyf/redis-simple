#include "server/aof/manifest.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace redis_simple::aof {
namespace {
TEST(ManifestTest, EncodesAndParsesRedisFormat) {
  Manifest manifest;
  manifest.base = MakeManifestFile("appendonly.aof.3.base.aof", 3,
                                   ManifestFile::Type::kBase);
  manifest.history.push_back(MakeManifestFile("appendonly.aof.2.base.aof", 2,
                                              ManifestFile::Type::kHistory));
  manifest.increments.push_back(MakeManifestFile(
      "appendonly.aof.5.incr.aof", 5, ManifestFile::Type::kIncrement));
  manifest.increments.push_back(MakeManifestFile(
      "appendonly.aof.6.incr.aof", 6, ManifestFile::Type::kIncrement));
  const std::string text = manifest.Encode();
  EXPECT_EQ(text,
            "file appendonly.aof.3.base.aof seq 3 type b\n"
            "file appendonly.aof.2.base.aof seq 2 type h\n"
            "file appendonly.aof.5.incr.aof seq 5 type i\n"
            "file appendonly.aof.6.incr.aof seq 6 type i\n");

  const std::optional<Manifest> result = Manifest::Parse(text);
  ASSERT_TRUE(result.has_value());
  const Manifest parsed = result.value_or(Manifest{});
  ASSERT_TRUE(parsed.base.has_value());
  EXPECT_EQ(parsed.base.value_or(ManifestFile{}).name,
            "appendonly.aof.3.base.aof");
  ASSERT_EQ(parsed.increments.size(), 2);
  EXPECT_EQ(parsed.increments.back().sequence, 6);
  ASSERT_EQ(parsed.history.size(), 1);
  EXPECT_EQ(parsed.base_sequence, 3);
  EXPECT_EQ(parsed.increment_sequence, 6);
}

TEST(ManifestTest, AcceptsAnyKeyOrderCommentsAndUnknownKeys) {
  const std::optional<Manifest> result = Manifest::Parse(
      "# written by a newer version\n"
      "TYPE b Seq 1 file a.1.base.rdb extra x\r\n"
      "file a.4.incr.aof seq 4 type i startoffset 10 endoffset 20\n");
  ASSERT_TRUE(result.has_value());
  const Manifest parsed = result.value_or(Manifest{});
  EXPECT_EQ(parsed.base.value_or(ManifestFile{}).name, "a.1.base.rdb");
  ASSERT_EQ(parsed.increments.size(), 1);
  // Redis' replication offsets are kept.
  EXPECT_EQ(parsed.increments.front().start_offset, 10);
  EXPECT_EQ(parsed.increments.front().end_offset, 20);
  EXPECT_EQ(parsed.Encode(),
            "file a.1.base.rdb seq 1 type b\n"
            "file a.4.incr.aof seq 4 type i startoffset 10 endoffset 20\n");
}

TEST(ManifestTest, QuotesNamesLikeRedis) {
  Manifest manifest;
  ManifestFile file;
  file.name = "my aof\x01\"x\".1.incr.aof";
  file.sequence = 1;
  file.type = ManifestFile::Type::kIncrement;
  manifest.increments.push_back(file);
  const std::string text = manifest.Encode();
  EXPECT_EQ(text, "file \"my aof\\x01\\\"x\\\".1.incr.aof\" seq 1 type i\n");
  const std::optional<Manifest> parsed = Manifest::Parse(text);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value_or(Manifest{}).increments.front().name, file.name);
  // Single quotes work too, as in Redis' sdssplitargs.
  EXPECT_EQ(Manifest::Parse("file 'a b' seq 1 type i\n")
                .value_or(Manifest{})
                .increments.front()
                .name,
            "a b");
}

TEST(ManifestTest, TakesTheNextSequenceNumbersFromLiveFiles) {
  // As in Redis, history files do not count.
  const Manifest parsed = Manifest::Parse(
                              "file a.3.base.aof seq 3 type b\n"
                              "file a.9.base.aof seq 9 type h\n"
                              "file a.8.incr.aof seq 8 type h\n"
                              "file a.5.incr.aof seq 5 type i\n")
                              .value_or(Manifest{});
  EXPECT_EQ(parsed.base_sequence, 3);
  EXPECT_EQ(parsed.increment_sequence, 5);
  // A manifest of comments lists no files.
  EXPECT_TRUE(Manifest::Parse("# nothing yet\n").has_value());
}

TEST(ManifestTest, RejectsMalformedManifests) {
  for (const std::string_view text : {
           "",                                            // empty
           "file a seq 1\n",                              // no type
           "file a type b\n",                             // no sequence
           "seq 1 type b\n",                              // no file
           "file a seq x type b\n",                       // bad sequence
           "file a seq 0 type b\n",                       // zero sequence
           "file a seq 1 type z\n",                       // bad type
           "file dir/a seq 1 type b\n",                   // a directory
           "file dir\\a seq 1 type b\n",                  // a directory
           "file a seq 1 type\n",                         // missing value
           "file a seq 1 type b\nfile c seq 2 type b\n",  // two bases
           "file a seq 2 type i\nfile c seq 1 type i\n",  // out of order
           "file a seq 1 type b\n\n",                     // a blank line
           "file a seq 1 type b",                         // no newline
           "file \"a seq 1 type b\n",                     // open quote
           "file \"a\"b seq 1 type b\n",                  // text after quote
       }) {
    EXPECT_FALSE(Manifest::Parse(text).has_value()) << text;
  }
  // Redis reads lines of at most 1024 bytes.
  const std::string long_name(1024, 'a');
  EXPECT_FALSE(
      Manifest::Parse("file " + long_name + " seq 1 type b\n").has_value());
}
}  // namespace
}  // namespace redis_simple::aof
