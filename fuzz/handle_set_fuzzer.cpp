#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>

#include "containers/handle_set.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
struct Item {
  size_t hash;
};

struct ItemRef {
  Item* item{};
};

struct ItemRefHash {
  size_t operator()(const ItemRef& ref) const { return ref.item->hash; }
};

using ItemSet = containers::HandleSet<ItemRef, ItemRefHash>;

void Verify(const ItemSet& set, const std::set<Item*>& model) {
  Require(set.Size() == model.size());
  std::set<Item*> visited;
  std::optional<size_t> cursor = 0;
  while (cursor.has_value()) {
    cursor = set.Scan(*cursor, [&visited](ItemRef ref) {
      Require(visited.insert(ref.item).second);
    });
  }
  Require(visited == model);
}

void RunOperations(FuzzInput* input) {
  constexpr size_t kItemCount = 512;
  std::array<Item, kItemCount> items{};
  for (Item& item : items) {
    // Only eight distinct hashes, so that many items compete for the same
    // slots.
    item.hash = input->ReadByte() % 8;
  }
  ItemSet set;
  std::set<Item*> model;

  for (size_t operation_count = 0; operation_count < 4096 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 4;
    Item* const item = &items[input->ReadIndex(kItemCount)];
    const ItemRef ref{.item = item};
    switch (operation) {
      case 0:
      case 1:
        Require(set.Insert(ref) == model.insert(item).second);
        break;
      case 2:
        Require(set.Erase(ref) == (model.erase(item) != 0));
        break;
      case 3:
        Require(set.Contains(ref) == model.contains(item));
        break;
      default:
        break;
    }
    Verify(set, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
