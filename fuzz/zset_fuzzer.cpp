#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_types/zset/zset.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
using ScoreMap = std::unordered_map<std::string, double>;
using Entry = std::pair<std::string, double>;

std::vector<Entry> SortedEntries(const ScoreMap& model) {
  std::vector<Entry> entries(model.begin(), model.end());
  std::sort(entries.begin(), entries.end(),
            [](const Entry& lhs, const Entry& rhs) {
              return lhs.second < rhs.second ||
                     (lhs.second == rhs.second && lhs.first < rhs.first);
            });
  return entries;
}

void RequireEntries(const zset::ZSetEntryList& actual,
                    const std::vector<Entry>& expected) {
  Require(actual.size() == expected.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    Require(actual[index] != nullptr);
    Require(actual[index]->member == expected[index].first);
    Require(actual[index]->score == expected[index].second);
  }
}

void Verify(const zset::ZSet& zset, const ScoreMap& model) {
  const auto sorted = SortedEntries(model);
  Require(zset.Size() == sorted.size());
  for (size_t rank = 0; rank < sorted.size(); ++rank) {
    const auto& [key, score] = sorted[rank];
    Require(zset.Score(key) == score);
    Require(zset.Rank(key) == rank);
  }

  if (sorted.empty()) {
    return;
  }
  const zset::RangeByRankSpec forward_spec(
      0, static_cast<int64_t>(sorted.size() - 1), false, false);
  RequireEntries(zset.RangeByRank(&forward_spec), sorted);

  auto reversed = sorted;
  std::reverse(reversed.begin(), reversed.end());
  const zset::RangeByRankSpec reverse_spec(
      0, static_cast<int64_t>(sorted.size() - 1), false, false, std::nullopt,
      true);
  RequireEntries(zset.RangeByRank(&reverse_spec), reversed);

  const zset::RangeByScoreSpec score_spec(
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::infinity(), false, false);
  RequireEntries(zset.RangeByScore(&score_spec), sorted);
  Require(zset.Count(&score_spec) == sorted.size());
}

void SetScore(zset::ZSet* zset, ScoreMap* model, const std::string& key,
              double score) {
  const auto it = model->find(key);
  const bool inserted = it == model->end();
  if (std::isnan(score)) {
    Require(!zset->InsertOrUpdate(key, score));
    return;
  }
  Require(zset->InsertOrUpdate(key, score) == inserted);
  (*model)[key] = score;
}

void RunOperations(FuzzInput* input) {
  auto zset = zset::ZSet::Create();
  ScoreMap model;

  for (size_t operation_count = 0; operation_count < 128 && input->HasData();
       ++operation_count) {
    const uint8_t operation = input->ReadByte() % 6;
    std::string key = input->ReadValue(96);
    const double score = input->ReadScore();
    if (operation == 5) {
      key = PadToSize(std::move(key), 65);
    }

    switch (operation) {
      case 0:
      case 5:
        SetScore(zset.get(), &model, key, score);
        break;
      case 1:
        Require(zset->Delete(key) == (model.erase(key) != 0));
        break;
      case 2: {
        const auto it = model.find(key);
        Require(zset->Score(key) == (it == model.end()
                                         ? std::optional<double>()
                                         : std::optional<double>(it->second)));
        break;
      }
      case 3: {
        const auto sorted = SortedEntries(model);
        const auto it = std::find_if(
            sorted.begin(), sorted.end(),
            [&key](const Entry& entry) { return entry.first == key; });
        const auto expected = it == sorted.end()
                                  ? std::optional<size_t>()
                                  : std::optional<size_t>(static_cast<size_t>(
                                        std::distance(sorted.begin(), it)));
        Require(zset->Rank(key) == expected);
        break;
      }
      case 4: {
        const double other_score = input->ReadScore();
        if (!std::isnan(score) && !std::isnan(other_score)) {
          const double min_score = std::min(score, other_score);
          const double max_score = std::max(score, other_score);
          const zset::RangeByScoreSpec spec(min_score, max_score, false, false);
          const auto sorted = SortedEntries(model);
          std::vector<Entry> expected;
          std::copy_if(
              sorted.begin(), sorted.end(), std::back_inserter(expected),
              [min_score, max_score](const Entry& entry) {
                return entry.second >= min_score && entry.second <= max_score;
              });
          RequireEntries(zset->RangeByScore(&spec), expected);
          Require(zset->Count(&spec) == expected.size());
        }
        break;
      }
      default:
        break;
    }
    Verify(*zset, model);
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
