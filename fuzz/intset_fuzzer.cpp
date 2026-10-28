#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "fuzz/fuzz_input.h"
#include "memory/intset.h"

namespace redis_simple::fuzz {
namespace {
void Verify(const in_memory::IntSet& intset, const std::set<int64_t>& model) {
  Require(intset.Size() == model.size());
  size_t index = 0;
  for (const int64_t value : model) {
    Require(intset.Get(static_cast<unsigned int>(index)) == value);
    Require(intset.Find(value));
    ++index;
  }
  if (!model.empty()) {
    Require(intset.Min() == *model.begin());
    Require(intset.Max() == *model.rbegin());
  }

  std::vector<int64_t> visited;
  auto it = in_memory::IntSet::Iterator(&intset);
  it.SeekToFirst();
  while (it.Valid()) {
    visited.push_back(it.Value());
    it.Next();
  }
  Require(
      std::equal(visited.begin(), visited.end(), model.begin(), model.end()));
}

void RunOperations(FuzzInput* input) {
  in_memory::IntSet intset;
  std::set<int64_t> model;

  for (size_t operation_count = 0; operation_count < 256 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 3;
    const int64_t value = input->ReadInt64();
    switch (operation) {
      case 0:
        Require(intset.Add(value) == model.insert(value).second);
        break;
      case 1:
        Require(intset.Remove(value) == (model.erase(value) != 0));
        break;
      case 2:
        Require(intset.Find(value) == model.contains(value));
        break;
      default:
        break;
    }
    Verify(intset, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
