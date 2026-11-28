#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "containers/intset.h"
#include "containers/intset_test_util.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using containers::Intset;

void Verify(const Intset& intset, const std::set<int64_t>& model) {
  Require(intset.Size() == model.size());
  size_t index = 0;
  for (const int64_t value : model) {
    Require(intset.Get(static_cast<uint32_t>(index)) == value);
    Require(intset.Find(value));
    ++index;
  }
  if (!model.empty()) {
    Require(intset.Min() == *model.begin());
    Require(intset.Max() == *model.rbegin());
  }

  std::vector<int64_t> visited;
  auto it = Intset::Iterator(&intset);
  it.SeekToFirst();
  while (it.Valid()) {
    visited.push_back(it.Value());
    it.Next();
  }
  Require(
      std::equal(visited.begin(), visited.end(), model.begin(), model.end()));
}

// The blob `intset` writes loads as the same set, which writes the same
// bytes.
void VerifyBlobRoundTrip(const Intset& intset, const std::set<int64_t>& model) {
  std::string blob;
  intset.AppendRedisBlob(&blob);
  const std::unique_ptr<Intset> loaded = Intset::CreateFromRedisBlob(blob);
  Require(loaded != nullptr);
  Verify(*loaded, model);
  std::string written;
  loaded->AppendRedisBlob(&written);
  Require(written == blob);
}

// A blob in Redis' layout that the input shapes. Each value is the one before
// it plus a byte of the input, so most blobs are in ascending order, and a
// step of zero or one past the width's range gives a blob that is not. About
// one blob in five also names another width or length, or is cut short or
// extended.
std::string ReadBlob(FuzzInput* input) {
  constexpr std::array<uint32_t, 3> kWidths = {2, 4, 8};
  constexpr size_t kMaxValues = 48;
  const uint32_t width = kWidths[input->ReadByte() % kWidths.size()];
  const size_t count = input->ReadByte() % kMaxValues;
  std::vector<int64_t> values;
  values.reserve(count);
  auto value = static_cast<uint64_t>(input->ReadInt64());
  for (size_t i = 0; i < count; ++i) {
    values.push_back(static_cast<int64_t>(value));
    value += input->ReadByte();
  }
  std::string blob = containers::intset_test::Blob(width, values);

  constexpr size_t kLengthOffset = sizeof(uint32_t);
  const uint8_t fault = input->ReadByte();
  if (fault >= 240) {
    blob[0] = static_cast<char>(fault - 240);
  } else if (fault >= 224) {
    blob[kLengthOffset] = static_cast<char>(fault - 224);
  } else if (fault >= 216) {
    blob.resize(blob.size() - std::min<size_t>(blob.size(), fault - 216));
  } else if (fault >= 208) {
    blob.append(fault - 208, 'x');
  }
  return blob;
}

// The set the operations start from: an empty one, or the one a blob from
// the input holds when Intset accepts it. `model` receives its values.
std::unique_ptr<Intset> ReadStartingSet(FuzzInput* input,
                                        std::set<int64_t>* model) {
  if (input->ReadByte() % 2 == 0) {
    return std::make_unique<Intset>();
  }
  const std::string blob = ReadBlob(input);
  std::unique_ptr<Intset> intset = Intset::CreateFromRedisBlob(blob);
  if (intset == nullptr) {
    return std::make_unique<Intset>();
  }
  // An accepted blob holds distinct values in ascending order, and the set
  // writes it back as the same bytes.
  for (uint32_t index = 0; index < intset->Size(); ++index) {
    const int64_t value = intset->Get(index);
    Require(model->empty() || value > *model->rbegin());
    model->insert(value);
  }
  std::string written;
  intset->AppendRedisBlob(&written);
  Require(written == blob);
  return intset;
}

void RunOperations(FuzzInput* input) {
  std::set<int64_t> model;
  const std::unique_ptr<Intset> intset = ReadStartingSet(input, &model);
  Verify(*intset, model);

  for (size_t operation_count = 0; operation_count < 256 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 3;
    const int64_t value = input->ReadInt64();
    switch (operation) {
      case 0:
        Require(intset->Add(value) == model.insert(value).second);
        break;
      case 1:
        Require(intset->Delete(value) == (model.erase(value) != 0));
        break;
      case 2:
        Require(intset->Find(value) == model.contains(value));
        break;
      default:
        break;
    }
    Verify(*intset, model);
    VerifyBlobRoundTrip(*intset, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
