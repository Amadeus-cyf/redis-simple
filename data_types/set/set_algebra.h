#ifndef REDIS_SIMPLE_DATA_TYPES_SET_SET_ALGEBRA_H_
#define REDIS_SIMPLE_DATA_TYPES_SET_SET_ALGEBRA_H_

#include <algorithm>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "containers/dict.h"
#include "data_types/set/set.h"
#include "utils/check.h"

// Intersection and difference of sets, as SINTER and SDIFF compute them. The
// visitors take each result member as a std::string_view that is valid only
// during the call, in no particular order.
namespace redis_simple::data_types {
// Visits the members every set of `sets` holds. `sets` must not be empty; it
// is reordered.
//
// Like Redis' sinterGenericCommand, this sorts the sets by size, walks the
// smallest and probes the others from small to large: a member missing from
// the result is most cheaply ruled out by a set with few members.
template <typename Visitor>
void ForEachCommonMember(std::span<const Set*> sets, Visitor&& visitor) {
  RS_CHECK(!sets.empty());
  std::ranges::sort(sets, {}, &Set::Size);
  const std::span<const Set*> others = sets.subspan(1);
  sets.front()->ForEachMemberWithHash(
      [others, &visitor](std::string_view member,
                         const containers::HashedKey* hashed) {
        const bool common =
            std::ranges::all_of(others, [member, hashed](const Set* set) {
              return set->HasMember(member, hashed);
            });
        if (common) {
          visitor(member);
        }
        return true;
      });
}

// Visits the members of `first` that no set of `others` holds, by probing
// every set of `others` for each member of `first` (the first algorithm of
// Redis' sunionDiffGenericCommand). `others` is reordered from large to
// small, since a large set is the likeliest to hold a member and end its
// probes.
template <typename Visitor>
void ForEachDifferenceMemberByProbing(const Set& first,
                                      std::span<const Set*> others,
                                      Visitor&& visitor) {
  std::ranges::sort(others, std::ranges::greater{}, &Set::Size);
  first.ForEachMemberWithHash(
      [others, &visitor](std::string_view member,
                         const containers::HashedKey* hashed) {
        const bool removed =
            std::ranges::any_of(others, [member, hashed](const Set* set) {
              return set->HasMember(member, hashed);
            });
        if (!removed) {
          visitor(member);
        }
        return true;
      });
}

// The same result, by copying `first` and deleting every member of `others`
// from the copy (the second algorithm of Redis' sunionDiffGenericCommand).
// It costs one step per member of any set, however many sets there are.
template <typename Visitor>
void ForEachDifferenceMemberBySubtracting(const Set& first,
                                          std::span<const Set* const> others,
                                          Visitor&& visitor) {
  using MemberDict = containers::Dict<std::string, containers::NoValue>;
  const auto remaining = MemberDict::Create(first.Size());
  // Sizing fails only past the address space, which an existing set cannot
  // reach.
  RS_CHECK(remaining != nullptr);
  first.ForEachMemberWithHash(
      [&remaining](std::string_view member,
                   const containers::HashedKey* hashed) {
        if (hashed != nullptr) {
          remaining->FindOrInsert(*hashed);
        } else {
          remaining->FindOrInsert(member);
        }
        return true;
      });
  for (const Set* set : others) {
    set->ForEachMemberWithHash(
        [&remaining](std::string_view member,
                     const containers::HashedKey* hashed) {
          if (hashed != nullptr) {
            remaining->Delete(*hashed);
          } else {
            remaining->Delete(member);
          }
          // Nothing is left to delete once the copy is empty.
          return remaining->Size() > 0;
        });
    if (remaining->Size() == 0) {
      return;
    }
  }
  auto it = MemberDict::Iterator(remaining.get());
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    visitor(std::string_view(it.Key()));
  }
}

// Whether probing is expected to cost no more than subtracting, by the
// estimate of Redis' sunionDiffGenericCommand: probing takes one step per
// member of `first` and set, halved because a probe that finds the member
// ends early; subtracting takes one step per member of every set. Unlike
// Redis, a member of `first` counts as kCopyCostInProbes steps when
// subtracting: SDIFF here writes its reply while probing, so only
// subtracting pays for a copy, which the SetDifference benchmark
// (benchmarks/set_benchmark.cpp) measures at about two probes.
inline bool DifferenceProbingIsCheaper(const Set& first,
                                       std::span<const Set* const> others) {
  constexpr size_t kCopyCostInProbes = 2;
  const size_t probing_steps = first.Size() * (others.size() + 1) / 2;
  size_t subtracting_steps = first.Size() * kCopyCostInProbes;
  for (const Set* set : others) {
    subtracting_steps += set->Size();
  }
  return probing_steps <= subtracting_steps;
}

// Visits the members of `first` that no set of `others` holds, with the
// cheaper of the two algorithms above. `others` is reordered.
template <typename Visitor>
void ForEachDifferenceMember(const Set& first, std::span<const Set*> others,
                             Visitor&& visitor) {
  if (DifferenceProbingIsCheaper(first, others)) {
    ForEachDifferenceMemberByProbing(first, others, visitor);
  } else {
    ForEachDifferenceMemberBySubtracting(first, others, visitor);
  }
}
}  // namespace redis_simple::data_types

#endif  // REDIS_SIMPLE_DATA_TYPES_SET_SET_ALGEBRA_H_
