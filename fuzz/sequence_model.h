#ifndef REDIS_SIMPLE_FUZZ_SEQUENCE_MODEL_H_
#define REDIS_SIMPLE_FUZZ_SEQUENCE_MODEL_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_simple::fuzz {
inline void TrimModel(std::vector<std::string>* model, size_t start,
                      size_t stop) {
  if (model->empty() || start > stop || start >= model->size()) {
    model->clear();
    return;
  }
  stop = std::min(stop, model->size() - 1);
  std::vector<std::string> trimmed(
      model->begin() + static_cast<std::ptrdiff_t>(start),
      model->begin() + static_cast<std::ptrdiff_t>(stop + 1));
  *model = std::move(trimmed);
}

inline size_t RemoveMatchesFromModel(std::vector<std::string>* model,
                                     std::string_view value, size_t limit,
                                     bool from_tail) {
  size_t removed = 0;
  if (from_tail && limit != 0) {
    for (size_t index = model->size(); index > 0 && removed < limit;) {
      --index;
      if ((*model)[index] == value) {
        model->erase(model->begin() + static_cast<std::ptrdiff_t>(index));
        ++removed;
      }
    }
    return removed;
  }

  for (size_t index = 0;
       index < model->size() && (limit == 0 || removed < limit);) {
    if ((*model)[index] == value) {
      model->erase(model->begin() + static_cast<std::ptrdiff_t>(index));
      ++removed;
    } else {
      ++index;
    }
  }
  return removed;
}
}  // namespace redis_simple::fuzz

#endif  // REDIS_SIMPLE_FUZZ_SEQUENCE_MODEL_H_
