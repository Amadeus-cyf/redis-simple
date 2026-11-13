#include "containers/listpack_reader.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "containers/listpack.h"

namespace redis_simple::containers {
namespace {
// Reads every element as text, or nullopt if the reader reports corruption.
std::optional<std::vector<std::string>> ReadAll(std::string_view blob) {
  ListpackReader reader(blob);
  std::vector<std::string> elements;
  ListpackReader::Element element;
  while (true) {
    switch (reader.Next(&element)) {
      case ListpackReader::Status::kElement:
        elements.push_back(element.is_integer ? std::to_string(element.integer)
                                              : std::string(element.string));
        break;
      case ListpackReader::Status::kEnd:
        return elements;
      case ListpackReader::Status::kCorrupt:
        return std::nullopt;
    }
  }
}

std::vector<std::string> SampleElements() {
  std::vector<std::string> elements = {"",
                                       "a",
                                       "0",
                                       "127",
                                       "128",
                                       "-1",
                                       "4095",
                                       "-4096",
                                       "32767",
                                       "-32768",
                                       "8388607",
                                       "-8388608",
                                       "2147483647",
                                       "-2147483648",
                                       "9223372036854775807",
                                       "-9223372036854775808",
                                       "007",
                                       "+1"};
  for (const size_t length : {63, 64, 126, 127, 200, 4095, 4096, 20000}) {
    elements.emplace_back(length, 'x');
  }
  return elements;
}

TEST(ListpackReaderTest, ReadsWhatListpackWrites) {
  Listpack listpack;
  const std::vector<std::string> elements = SampleElements();
  for (const std::string& element : elements) {
    listpack.Append(element);
  }
  EXPECT_EQ(ReadAll(listpack.Bytes()), elements);
  EXPECT_EQ(ListpackReader(listpack.Bytes()).HeaderCount(), 0);
  Listpack empty;
  EXPECT_EQ(ReadAll(empty.Bytes()), std::vector<std::string>{});
}

TEST(ListpackReaderTest, ReadsRedisBytes) {
  // RPUSH l 0 -4097 5000-byte-string, as Redis 8.10 encodes it.
  std::string blob = {'\0', '\0', '\0', '\0', '\x03', '\0'};
  blob += std::string("\x00\x01", 2);              // 0
  blob += std::string("\xf1\xff\xef\x03", 4);      // -4097
  blob += std::string("\xf0\x88\x13\x00\x00", 5);  // 5000 bytes
  blob += std::string(5000, 'q') + "\x27\x8d";
  blob.push_back('\xff');
  const auto total = static_cast<uint32_t>(blob.size());
  for (int i = 0; i < 4; ++i) {
    blob[i] = static_cast<char>((total >> (8 * i)) & 0xff);
  }
  EXPECT_EQ(ReadAll(blob),
            (std::vector<std::string>{"0", "-4097", std::string(5000, 'q')}));
}

TEST(ListpackReaderTest, RejectsEveryTruncationAndBadHeader) {
  Listpack listpack;
  for (const std::string& element : SampleElements()) {
    listpack.Append(element);
  }
  const std::string blob(listpack.Bytes());
  for (size_t size = 0; size < blob.size(); ++size) {
    EXPECT_EQ(ReadAll(std::string_view(blob).substr(0, size)), std::nullopt)
        << size;
  }
  std::string wrong_count = blob;
  wrong_count[4] = static_cast<char>(wrong_count[4] + 1);
  EXPECT_EQ(ReadAll(wrong_count), std::nullopt);
}

TEST(ListpackReaderTest, NeverReadsOutsideCorruptedBytes) {
  Listpack listpack;
  for (const std::string& element : SampleElements()) {
    listpack.Append(element);
  }
  const std::string blob(listpack.Bytes());
  // A fixed seed keeps failures reproducible.
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937 rng(11);
  for (int round = 0; round < 20000; ++round) {
    std::string corrupted = blob;
    for (int flips = 1 + static_cast<int>(rng() % 3); flips > 0; --flips) {
      corrupted[rng() % corrupted.size()] = static_cast<char>(rng());
    }
    // Any result is fine; sanitizers catch reads outside the buffer.
    ReadAll(corrupted);
  }
}
}  // namespace
}  // namespace redis_simple::containers
