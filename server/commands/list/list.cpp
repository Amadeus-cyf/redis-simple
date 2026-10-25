#include "data_types/list/list.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "server/client.h"
#include "server/commands/handlers.h"
#include "server/commands/lookup.h"
#include "server/db/db.h"
#include "server/reply.h"
#include "utils/string_utils.h"

namespace redis_simple::command::lists {
namespace {
using List = ::redis_simple::list::List;
constexpr auto kList = db::RedisObject::ObjectType::kList;
constexpr std::string_view kNotAnInteger =
    "-ERR value is not an integer or out of range\r\n";

enum class Side : std::uint8_t {
  kLeft,
  kRight,
};

// Looks up the list at `key`, replying WRONGTYPE when another type is there.
// Returns false when the command must stop.
bool LookupList(Client* const client, std::string_view key,
                const List** const list) {
  const auto [object, wrong_type] = LookupTyped(client->Db(), key, kList);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return false;
  }
  *list = object == nullptr ? nullptr : object->List();
  return true;
}

bool LookupMutableList(Client* const client, std::string_view key,
                       List** const list) {
  const auto [object, wrong_type] =
      MutableLookupTyped(client->Db(), key, kList);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return false;
  }
  *list = object == nullptr ? nullptr : object->List();
  return true;
}

// Redis deletes a list key once its last element is removed.
void DeleteIfEmpty(Client* const client, std::string_view key,
                   const List* const list) {
  if (list->Size() == 0) {
    client->Db()->DeleteKey(key);
  }
}

bool ParseInteger(Client* const client, std::string_view text,
                  int64_t* const value) {
  if (!utils::ToInt64(text, value)) {
    client->AddReply(kNotAnInteger);
    return false;
  }
  return true;
}

std::optional<size_t> NormalizeIndex(int64_t index, size_t size) {
  const auto list_size = static_cast<int64_t>(size);
  if (index < 0) {
    index += list_size;
  }
  if (index < 0 || index >= list_size) {
    return std::nullopt;
  }
  return static_cast<size_t>(index);
}

std::optional<std::pair<size_t, size_t>> NormalizeRange(int64_t start,
                                                        int64_t stop,
                                                        size_t size) {
  if (size == 0) {
    return std::nullopt;
  }
  const auto list_size = static_cast<int64_t>(size);
  if (start < 0) {
    start += list_size;
  }
  if (stop < 0) {
    stop += list_size;
  }
  start = std::max<int64_t>(start, 0);
  stop = std::min<int64_t>(stop, list_size - 1);
  if (start > stop || start >= list_size) {
    return std::nullopt;
  }
  return std::pair<size_t, size_t>(static_cast<size_t>(start),
                                   static_cast<size_t>(stop));
}

std::pair<size_t, List::RemoveDirection> NormalizeRemoveCount(int64_t count) {
  if (count >= 0) {
    return {static_cast<size_t>(count), List::RemoveDirection::kFromHead};
  }
  if (count == std::numeric_limits<int64_t>::min()) {
    return {std::numeric_limits<size_t>::max(),
            List::RemoveDirection::kFromTail};
  }
  return {static_cast<size_t>(-count), List::RemoveDirection::kFromTail};
}

void HandlePush(Client* const client, Side side) {
  const auto& args = client->Args();
  auto [object, wrong_type] = MutableLookupTyped(client->Db(), args[0], kList);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (object == nullptr) {
    // LLVM 18 cannot trace unique_ptr ownership through Dict::FindOrInsert.
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    auto created = db::RedisObject::CreateWithList(List::Create());
    object = created.get();
    client->Db()->SetKey(args[0], std::move(created), 0);
  }
  List* const list = object->List();
  for (size_t i = 1; i < args.size(); ++i) {
    const bool pushed =
        side == Side::kLeft ? list->LPush(args[i]) : list->RPush(args[i]);
    if (!pushed) {
      client->AddReply(reply::FromError("ERR list push failed"));
      return;
    }
  }
  client->MarkModified();
  client->AddReply(reply::FromInt64(static_cast<int64_t>(list->Size())));
}

void HandlePop(Client* const client, Side side) {
  const std::string_view key = client->Args()[0];
  List* list = nullptr;
  if (!LookupMutableList(client, key, &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  const std::optional<std::string> value =
      side == Side::kLeft ? list->LPop() : list->RPop();
  DeleteIfEmpty(client, key, list);
  if (!value.has_value()) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  client->MarkModified();
  client->AddBulkReply(*value);
}
}  // namespace

void HandleLPush(Client* const client) { HandlePush(client, Side::kLeft); }

void HandleRPush(Client* const client) { HandlePush(client, Side::kRight); }

void HandleLPop(Client* const client) { HandlePop(client, Side::kLeft); }

void HandleRPop(Client* const client) { HandlePop(client, Side::kRight); }

void HandleLLen(Client* const client) {
  const List* list = nullptr;
  if (!LookupList(client, client->Args()[0], &list)) {
    return;
  }
  const size_t size = list == nullptr ? 0 : list->Size();
  client->AddReply(reply::FromInt64(static_cast<int64_t>(size)));
}

void HandleLRange(Client* const client) {
  const auto& args = client->Args();
  int64_t start = 0;
  int64_t stop = 0;
  if (!ParseInteger(client, args[1], &start) ||
      !ParseInteger(client, args[2], &stop)) {
    return;
  }
  const List* list = nullptr;
  if (!LookupList(client, args[0], &list)) {
    return;
  }
  const auto range = list == nullptr
                         ? std::nullopt
                         : NormalizeRange(start, stop, list->Size());
  if (!range.has_value()) {
    client->AddReply(reply::FromArrayHeader(0));
    return;
  }
  std::string encoded =
      reply::FromArrayHeader(range->second - range->first + 1);
  list->ForEach(range->first, range->second,
                [&encoded](std::string_view value) {
                  reply::AppendBulkString(value, &encoded);
                  return true;
                });
  client->AddReply(std::move(encoded));
}

void HandleLIndex(Client* const client) {
  const auto& args = client->Args();
  int64_t index = 0;
  if (!ParseInteger(client, args[1], &index)) {
    return;
  }
  const List* list = nullptr;
  if (!LookupList(client, args[0], &list)) {
    return;
  }
  const auto position =
      list == nullptr ? std::nullopt : NormalizeIndex(index, list->Size());
  const auto value = position.has_value() ? list->At(*position) : std::nullopt;
  if (value.has_value()) {
    client->AddBulkReply(*value);
  } else {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}

void HandleLSet(Client* const client) {
  const auto& args = client->Args();
  int64_t index = 0;
  if (!ParseInteger(client, args[1], &index)) {
    return;
  }
  List* list = nullptr;
  if (!LookupMutableList(client, args[0], &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::FromError("ERR no such key"));
    return;
  }
  const auto position = NormalizeIndex(index, list->Size());
  if (!position.has_value() || !list->Set(*position, args[2])) {
    client->AddReply(reply::FromError("ERR index out of range"));
    return;
  }
  client->MarkModified();
  client->AddReply(reply::kOk);
}

void HandleLRem(Client* const client) {
  const auto& args = client->Args();
  int64_t count = 0;
  if (!ParseInteger(client, args[1], &count)) {
    return;
  }
  List* list = nullptr;
  if (!LookupMutableList(client, args[0], &list)) {
    return;
  }
  size_t removed = 0;
  if (list != nullptr) {
    const auto [limit, direction] = NormalizeRemoveCount(count);
    removed = list->Remove(args[2], limit, direction).value_or(0);
    DeleteIfEmpty(client, args[0], list);
  }
  if (removed > 0) {
    client->MarkModified();
  }
  client->AddReply(reply::FromInt64(static_cast<int64_t>(removed)));
}

void HandleLTrim(Client* const client) {
  const auto& args = client->Args();
  int64_t start = 0;
  int64_t stop = 0;
  if (!ParseInteger(client, args[1], &start) ||
      !ParseInteger(client, args[2], &stop)) {
    return;
  }
  List* list = nullptr;
  if (!LookupMutableList(client, args[0], &list)) {
    return;
  }
  if (list != nullptr) {
    const auto range = NormalizeRange(start, stop, list->Size());
    // An empty range trims every element.
    const bool trimmed = range.has_value()
                             ? list->Trim(range->first, range->second)
                             : list->Trim(1, 0);
    if (!trimmed) {
      client->AddReply(reply::FromError("ERR list trim failed"));
      return;
    }
    DeleteIfEmpty(client, args[0], list);
  }
  client->MarkModified();
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::command::lists
