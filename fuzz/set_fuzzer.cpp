#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "data_types/set/set.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using MemberSet = std::unordered_set<std::string>;

void Verify(const set::Set& set, const MemberSet& model) {
  Require(set.Size() == model.size());
  for (const auto& member : model) {
    Require(set.HasMember(member));
  }

  MemberSet visited;
  Require(set.ForEachMember([&visited](std::string_view member) {
    visited.emplace(member);
    return true;
  }));
  Require(visited == model);
}

void RunOperations(FuzzInput* input) {
  auto set = set::Set::Create();
  MemberSet model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 5;
    std::string value = input->ReadValue(96);
    if (operation == 4) {
      value = PadToSize(std::move(value), 65);
    }

    switch (operation) {
      case 0:
      case 4:
        Require(set->Add(value) == model.emplace(value).second);
        break;
      case 1:
        Require(set->Remove(value) == (model.erase(value) != 0));
        break;
      case 2:
        Require(set->HasMember(value) == model.contains(value));
        break;
      case 3: {
        const auto members = set->ListAllMembers();
        const MemberSet actual(members.begin(), members.end());
        Require(actual == model);
        break;
      }
      default:
        break;
    }
    Verify(*set, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
