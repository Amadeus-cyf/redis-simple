#ifndef REDIS_SIMPLE_CONTAINERS_LISTPACK_READER_H_
#define REDIS_SIMPLE_CONTAINERS_LISTPACK_READER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace redis_simple::containers {
// Reads a listpack from untrusted bytes, such as an RDB file, checking every
// entry as Redis' lpValidateIntegrity does with deep validation: the header's
// total size, each entry's encoding and size within the buffer, its backlen,
// the terminator, and the element count when the header records one.
class ListpackReader {
 public:
  struct Element {
    // A string element's bytes, a view into the buffer.
    std::string_view string;
    int64_t integer{};
    bool is_integer{};
  };

  enum class Status : uint8_t {
    kElement,
    kEnd,
    kCorrupt,
  };

  explicit ListpackReader(std::string_view blob) : blob_(blob) {}

  // Reads the next element into `*element`. kEnd means the listpack was read
  // completely and is valid; kCorrupt that it is not, at this point or in its
  // header.
  Status Next(Element* element);

  // The element count the header records, valid after the first Next. Large
  // listpacks record UINT16_MAX instead; read them to the end to count.
  uint16_t HeaderCount() const { return header_count_; }

 private:
  bool ReadHeader();

  std::string_view blob_;
  size_t offset_{};
  size_t count_{};
  uint16_t header_count_{};
  bool header_read_{};
};
// Redis 8.8 to 8.10 wrote the backlen of an element whose encoded size is
// 16383, 2097151, or 268435455 bytes one byte shorter than every other
// release, and earlier builds of this server did too. Rewrites those
// backlens in `*listpack` in the usual width, which ListpackReader and
// Listpack read, as Redis 8.12's lpUpgradeCompactBacklen does, and returns
// whether it changed anything. Bytes that do not read as a listpack
// throughout are left as they are, for ListpackReader to reject.
bool WidenCompactBacklens(std::string* listpack);
}  // namespace redis_simple::containers

#endif  // REDIS_SIMPLE_CONTAINERS_LISTPACK_READER_H_
