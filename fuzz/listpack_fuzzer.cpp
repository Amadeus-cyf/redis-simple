#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "containers/listpack.h"
#include "containers/listpack_reader.h"
#include "fuzz/fuzz_input.h"
#include "fuzz/sequence_model.h"
#include "utils/strings.h"

namespace redis_simple::fuzz {
namespace {
using containers::ListEnd;
using containers::Listpack;

void Verify(const Listpack& listpack, const std::vector<std::string>& model) {
  Require(listpack.Size() == model.size());
  Require(listpack.TotalBytes() >= Listpack::kEmptyListpackBytes);
  Require(listpack.First().has_value() == !model.empty());
  Require(listpack.Last().has_value() == !model.empty());

  std::vector<std::string> forward;
  const size_t stop = model.empty() ? 0 : model.size() - 1;
  Require(listpack.ForEach(0, stop, [&forward](std::string_view value) {
    forward.emplace_back(value);
    return true;
  }));
  Require(forward == model);

  std::vector<std::string> reverse;
  Require(listpack.ForEachReverse(0, stop, [&reverse](std::string_view value) {
    reverse.emplace_back(value);
    return true;
  }));
  const std::vector<std::string> expected_reverse(model.rbegin(), model.rend());
  Require(reverse == expected_reverse);

  for (size_t index = 0; index < model.size(); ++index) {
    const auto offset = listpack.OffsetAt(index);
    Require(listpack.Get(ValueOrAbort(offset)) == model[index]);
    // An element is stored as an integer exactly when its text is one in
    // canonical form.
    Require(listpack.IntegerAt(ValueOrAbort(offset)) ==
            utils::ToInt64(model[index]));
  }
  Require(!listpack.OffsetAt(model.size()).has_value());
}

void RunOperations(FuzzInput* input) {
  Listpack listpack;
  std::vector<std::string> model;
  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 14;
    const std::string value = input->ReadValue(96);
    const size_t index = input->ReadIndex(model.size() + 2);
    const size_t limit = input->ReadIndex(6);

    switch (operation) {
      case 0:
        Require(listpack.Append(value));
        model.push_back(value);
        break;
      case 1:
        Require(listpack.Prepend(value));
        model.insert(model.begin(), value);
        break;
      case 2: {
        const int64_t integer = input->ReadInt64();
        Require(listpack.Append(integer));
        model.push_back(std::to_string(integer));
        break;
      }
      case 3: {
        const int64_t integer = input->ReadInt64();
        Require(listpack.Prepend(integer));
        model.insert(model.begin(), std::to_string(integer));
        break;
      }
      case 4:
        if (index < model.size()) {
          const auto offset = listpack.OffsetAt(index);
          Require(listpack.Insert(ValueOrAbort(offset), value));
          model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                       value);
        }
        break;
      case 5:
        if (index < model.size()) {
          const auto offset = listpack.OffsetAt(index);
          Require(listpack.Replace(ValueOrAbort(offset), value));
          model[index] = value;
        }
        break;
      case 6:
        if (index < model.size()) {
          const auto offset = listpack.OffsetAt(index);
          listpack.Delete(ValueOrAbort(offset));
          model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
        }
        break;
      case 7:
        if (index < model.size()) {
          const auto offset = listpack.OffsetAt(index);
          const size_t expected = std::min(limit, model.size() - index);
          Require(listpack.DeleteRange(ValueOrAbort(offset), limit) ==
                  expected);
          model.erase(
              model.begin() + static_cast<std::ptrdiff_t>(index),
              model.begin() + static_cast<std::ptrdiff_t>(index + expected));
        }
        break;
      case 8: {
        const bool from_tail = (input->ReadByte() & 1U) != 0;
        const size_t expected =
            RemoveMatchesFromModel(&model, value, limit, from_tail);
        Require(listpack.DeleteMatching(
                    value, limit,
                    from_tail ? ListEnd::kTail : ListEnd::kHead) == expected);
        break;
      }
      case 9: {
        // A string, then an integer given as one, as a hash field and a
        // counter or a sorted-set member and its score are appended.
        const int64_t second = input->ReadInt64();
        const std::vector<Listpack::ListpackEntry> entries = {
            {value, 0, false}, {"", second, true}};
        Require(listpack.BatchAppend(entries));
        model.push_back(value);
        model.push_back(std::to_string(second));
        break;
      }
      case 10: {
        const std::string second = input->ReadValue(96);
        const std::vector<Listpack::ListpackEntry> entries = {
            {value, 0, false}, {second, 0, false}};
        Require(listpack.BatchPrepend(entries));
        model.insert(model.begin(), {value, second});
        break;
      }
      case 11:
        if (index < model.size()) {
          const std::string second = input->ReadValue(96);
          const std::vector<Listpack::ListpackEntry> entries = {
              {value, 0, false}, {second, 0, false}};
          const auto offset = listpack.OffsetAt(index);
          Require(listpack.BatchInsert(ValueOrAbort(offset), entries));
          model.insert(model.begin() + static_cast<std::ptrdiff_t>(index),
                       {value, second});
        }
        break;
      case 12: {
        const auto found = listpack.Find(value);
        const bool expected =
            std::find(model.begin(), model.end(), value) != model.end();
        Require(found.has_value() == expected);
        if (found.has_value()) {
          Require(listpack.Get(found.value()) == value);
        }
        break;
      }
      case 13: {
        // Split at `index`, which may be past the end, and join the two
        // parts again in either order. The bytes move as they are, so the
        // sizes add up.
        const size_t kept = std::min(index, model.size());
        const size_t total_bytes = listpack.TotalBytes();
        Listpack tail = listpack.SplitOff(index);
        Require(listpack.Size() == kept);
        Require(tail.Size() == model.size() - kept);
        Require(size_t{listpack.TotalBytes()} + tail.TotalBytes() ==
                total_bytes + Listpack::kEmptyListpackBytes);
        if (limit % 2 == 0) {
          Require(listpack.AppendEntries(tail));
        } else {
          Require(tail.AppendEntries(listpack));
          listpack = std::move(tail);
          std::rotate(model.begin(),
                      model.begin() + static_cast<std::ptrdiff_t>(kept),
                      model.end());
        }
        Require(listpack.TotalBytes() == total_bytes);
        break;
      }
      default:
        break;
    }
    Verify(listpack, model);
  }
}

// Redis 8.8 to 8.10 wrote the backlen of an element whose encoded size is
// 16383 bytes as 7f ff where Listpack writes 00 ff ff. Shortening them and
// widening them again gives back the listpack's bytes, widening leaves a
// listpack without them alone, and changed bytes never crash it.
void CheckBacklenWidening(FuzzInput* input) {
  // The value's length and, with its 5-byte string header, its encoded size.
  constexpr size_t kBoundaryLength = 16378;
  constexpr size_t kBoundaryBytes = kBoundaryLength + 5;
  constexpr std::string_view kUsualBacklen("\x00\xff\xff", 3);
  constexpr std::string_view kShortBacklen("\x7f\xff", 2);
  Listpack listpack;
  std::vector<bool> boundary;
  for (size_t count = 1 + input->ReadIndex(4); count > 0; --count) {
    const bool big = input->ReadByte() % 2 == 0;
    const std::string value =
        big ? std::string(kBoundaryLength, static_cast<char>(input->ReadByte()))
            : input->ReadValue(96);
    Require(listpack.Append(value));
    boundary.push_back(big);
  }
  const std::string usual(listpack.Bytes());
  std::string bytes = usual;
  Require(!containers::WidenCompactBacklens(&bytes) && bytes == usual);

  // Shorten from the last element back, so earlier offsets stay valid.
  bool shortened = false;
  for (size_t index = boundary.size(); index > 0; --index) {
    if (!boundary[index - 1]) {
      continue;
    }
    const size_t backlen_offset =
        ValueOrAbort(listpack.OffsetAt(index - 1)) + kBoundaryBytes;
    Require(bytes.compare(backlen_offset, kUsualBacklen.size(),
                          kUsualBacklen) == 0);
    bytes.replace(backlen_offset, kUsualBacklen.size(), kShortBacklen);
    shortened = true;
  }
  for (size_t i = 0; i < 4; ++i) {
    bytes[i] = static_cast<char>((bytes.size() >> (8 * i)) & 0xff);
  }
  std::string widened = bytes;
  Require(containers::WidenCompactBacklens(&widened) == shortened);
  Require(widened == usual);
  if (!input->HasData()) {
    return;
  }
  // A changed byte: anything may happen but a crash.
  const size_t offset = input->ReadIndex(bytes.size());
  bytes[offset] =
      static_cast<char>(bytes[offset] ^ (1 + (input->ReadByte() % 255)));
  if (containers::WidenCompactBacklens(&bytes)) {
    containers::ListpackReader reader(bytes);
    containers::ListpackReader::Element element;
    while (reader.Next(&element) ==
           containers::ListpackReader::Status::kElement) {
    }
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  // One input in 16 checks backlen widening, whose 16 KB elements would
  // slow the operations below.
  if (size > 0 && data[0] % 16 == 0) {
    input.ReadByte();
    redis_simple::fuzz::CheckBacklenWidening(&input);
    return 0;
  }
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
