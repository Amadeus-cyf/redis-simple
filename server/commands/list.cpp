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
#include "utils/strings.h"

namespace redis_simple::commands::list {
namespace {
using List = ::redis_simple::data_types::List;
using ListEnd = ::redis_simple::containers::ListEnd;
constexpr auto kList = db::RedisObject::ObjectType::kList;

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
  const std::optional<int64_t> parsed = utils::ToInt64(text);
  if (!parsed.has_value()) {
    client->AddReply(reply::kNotAnIntegerError);
    return false;
  }
  *value = *parsed;
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

std::pair<size_t, ListEnd> NormalizeRemoveCount(int64_t count) {
  if (count >= 0) {
    return {static_cast<size_t>(count), ListEnd::kHead};
  }
  if (count == std::numeric_limits<int64_t>::min()) {
    return {std::numeric_limits<size_t>::max(), ListEnd::kTail};
  }
  return {static_cast<size_t>(-count), ListEnd::kTail};
}

void HandlePush(Client* const client, ListEnd end) {
  const commands::CommandArgs& args = client->Args();
  const auto [object, created, wrong_type] =
      LookupOrCreateTyped(client->Db(), args[0], kList);
  if (wrong_type) {
    client->AddReply(reply::kWrongTypeError);
    return;
  }
  if (created) {
    *object = std::move(*db::RedisObject::CreateWithList(List::Create()));
  }
  List* const list = object->List();
  for (size_t i = 1; i < args.size(); ++i) {
    const bool pushed =
        end == ListEnd::kHead ? list->LPush(args[i]) : list->RPush(args[i]);
    if (!pushed) {
      client->AddReply(reply::FromError("ERR list push failed"));
      return;
    }
  }
  client->MarkModified(static_cast<int64_t>(args.size() - 1));
  client->AddIntegerReply(static_cast<int64_t>(list->Size()));
}

// Redis' popGenericCommand: with a count, pops up to that many elements
// into an array, from the end popped first.
void HandlePopCount(Client* const client, ListEnd end) {
  const commands::CommandArgs& args = client->Args();
  const std::optional<int64_t> count = utils::ToInt64(args[1]);
  if (!count.has_value() || *count < 0) {
    client->AddReply(
        reply::FromError("ERR value is out of range, must be positive"));
    return;
  }
  List* list = nullptr;
  if (!LookupMutableList(client, args[0], &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::NullArrayView(client->Protocol()));
    return;
  }
  const size_t size = list->Size();
  const size_t popped = std::min(static_cast<size_t>(*count), size);
  std::string encoded = reply::FromArrayHeader(popped);
  reply::ReserveBulkStrings(popped, &encoded);
  if (popped == 0) {
    client->AddReply(std::move(encoded));
    return;
  }
  const auto append = [&encoded](std::string_view value) {
    reply::AppendBulkString(value, &encoded);
    return true;
  };
  if (end == ListEnd::kHead) {
    list->ForEach(0, popped - 1, append);
  } else {
    list->ForEachReverse(size - popped, size - 1, append);
  }
  if (popped == size) {
    client->Db()->DeleteKey(args[0]);
  } else if (end == ListEnd::kHead) {
    list->Trim(popped, size - 1);
  } else {
    list->Trim(0, size - popped - 1);
  }
  client->MarkModified(static_cast<int64_t>(popped));
  client->AddReply(std::move(encoded));
}

void HandlePop(Client* const client, ListEnd end) {
  if (client->Args().size() == 2) {
    HandlePopCount(client, end);
    return;
  }
  const std::string_view key = client->Args()[0];
  List* list = nullptr;
  if (!LookupMutableList(client, key, &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  // Encode the element where it is stored instead of copying it out first.
  const bool popped = list->Pop(
      end, [client](std::string_view value) { client->AddBulkReply(value); });
  DeleteIfEmpty(client, key, list);
  if (!popped) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  client->MarkModified();
}
}  // namespace

void HandleLPush(Client* const client) { HandlePush(client, ListEnd::kHead); }

void HandleRPush(Client* const client) { HandlePush(client, ListEnd::kTail); }

void HandleLPop(Client* const client) { HandlePop(client, ListEnd::kHead); }

void HandleRPop(Client* const client) { HandlePop(client, ListEnd::kTail); }

void HandleLLen(Client* const client) {
  const List* list = nullptr;
  if (!LookupList(client, client->Args()[0], &list)) {
    return;
  }
  const size_t size = list == nullptr ? 0 : list->Size();
  client->AddIntegerReply(static_cast<int64_t>(size));
}

void HandleLRange(Client* const client) {
  const commands::CommandArgs& args = client->Args();
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
  const std::optional<std::pair<size_t, size_t>> range =
      list == nullptr ? std::nullopt
                      : NormalizeRange(start, stop, list->Size());
  if (!range.has_value()) {
    client->AddReply(reply::FromArrayHeader(0));
    return;
  }
  const size_t count = range->second - range->first + 1;
  std::string encoded = reply::FromArrayHeader(count);
  reply::ReserveBulkStrings(count, &encoded);
  list->ForEach(range->first, range->second,
                [&encoded](std::string_view value) {
                  reply::AppendBulkString(value, &encoded);
                  return true;
                });
  client->AddReply(std::move(encoded));
}

void HandleLIndex(Client* const client) {
  // Redis checks the key before the index.
  const commands::CommandArgs& args = client->Args();
  const List* list = nullptr;
  if (!LookupList(client, args[0], &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::NullView(client->Protocol()));
    return;
  }
  int64_t index = 0;
  if (!ParseInteger(client, args[1], &index)) {
    return;
  }
  const std::optional<size_t> position = NormalizeIndex(index, list->Size());
  // Encode the element where it is stored instead of copying it out first.
  bool found = false;
  if (position.has_value()) {
    list->ForEach(*position, *position,
                  [client, &found](std::string_view value) {
                    client->AddBulkReply(value);
                    found = true;
                    return true;
                  });
  }
  if (!found) {
    client->AddReply(reply::NullView(client->Protocol()));
  }
}

void HandleLSet(Client* const client) {
  // Redis checks the key before the index.
  const commands::CommandArgs& args = client->Args();
  List* list = nullptr;
  if (!LookupMutableList(client, args[0], &list)) {
    return;
  }
  if (list == nullptr) {
    client->AddReply(reply::FromError("ERR no such key"));
    return;
  }
  int64_t index = 0;
  if (!ParseInteger(client, args[1], &index)) {
    return;
  }
  const std::optional<size_t> position = NormalizeIndex(index, list->Size());
  if (!position.has_value() || !list->Set(*position, args[2])) {
    client->AddReply(reply::FromError("ERR index out of range"));
    return;
  }
  client->MarkModified();
  client->AddReply(reply::kOk);
}

void HandleLRem(Client* const client) {
  const commands::CommandArgs& args = client->Args();
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
    const auto [limit, from] = NormalizeRemoveCount(count);
    removed = list->Remove(args[2], limit, from);
    DeleteIfEmpty(client, args[0], list);
  }
  if (removed > 0) {
    client->MarkModified(static_cast<int64_t>(removed));
  }
  client->AddIntegerReply(static_cast<int64_t>(removed));
}

void HandleLTrim(Client* const client) {
  const commands::CommandArgs& args = client->Args();
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
  if (list == nullptr) {
    // A missing key is left alone: no WATCHer is touched and nothing
    // reaches the AOF.
    client->AddReply(reply::kOk);
    return;
  }
  const size_t size = list->Size();
  const std::optional<std::pair<size_t, size_t>> range =
      NormalizeRange(start, stop, size);
  if (range.has_value()) {
    list->Trim(range->first, range->second);
  } else {
    // An empty range trims every element.
    list->Trim(1, 0);
  }
  const size_t removed = size - list->Size();
  DeleteIfEmpty(client, args[0], list);
  if (removed > 0) {
    client->MarkModified(static_cast<int64_t>(removed));
  } else {
    // Redis' ltrimCommand signals the key even when nothing was trimmed,
    // which fails a WATCH on it, but counts no change, so the command is
    // not written to the AOF.
    client->Db()->TouchWatchedKey(args[0]);
  }
  client->AddReply(reply::kOk);
}
}  // namespace redis_simple::commands::list
