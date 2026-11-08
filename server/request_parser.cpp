#include "server/request_parser.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "utils/split_args.h"
#include "utils/string_utils.h"

namespace redis_simple::request_parser {
namespace {
// Redis' PROTO_INLINE_MAX_SIZE: the longest inline request or RESP header
// line without its line ending.
constexpr size_t kMaxLineBytes = size_t{64} * 1024;
// Redis' default proto-max-bulk-len.
constexpr int64_t kMaxBulkBytes = int64_t{512} * 1024 * 1024;
// Redis' limits for clients that have not authenticated yet.
constexpr int64_t kMaxUnauthenticatedElements = 10;
constexpr int64_t kMaxUnauthenticatedBulkBytes = 16384;
constexpr size_t kMinimumBulkStringBytes = 6;
constexpr size_t kCrlfSize = 2;

struct LengthLine {
  int64_t value{};
  // Offset just past the line ending.
  size_t end{};
};

// Parses the length that follows a RESP type byte at `start`, as Redis'
// processMultibulkBuffer does: up to the next CR, as string2ll text. Like
// Redis, the byte after the CR is skipped without checking it. Kept out of
// line, since it is rare, so the fast path stays small enough to inline.
[[gnu::cold, gnu::noinline]] ParseStatus ParseLengthLineSlowly(
    std::string_view input, size_t start, ProtocolError too_long,
    ProtocolError invalid, LengthLine* const line, ProtocolError* const error) {
  const size_t search = std::min(input.size(), start + kMaxLineBytes + 1);
  const void* const found =
      std::memchr(input.data() + start, '\r', search - start);
  if (found == nullptr) {
    if (input.size() - start > kMaxLineBytes) {
      *error = too_long;
      return ParseStatus::kInvalid;
    }
    return ParseStatus::kIncomplete;
  }
  const auto cr =
      static_cast<size_t>(static_cast<const char*>(found) - input.data());
  if (cr + 1 >= input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (!utils::ToInt64(input.substr(start, cr - start), &line->value)) {
    *error = invalid;
    return ParseStatus::kInvalid;
  }
  line->end = cr + kCrlfSize;
  return ParseStatus::kComplete;
}

// The common case in one pass: a canonical non-negative length of at most
// 18 digits, then a CR with a byte after it. Anything else, including a
// line not fully received, takes the slow path, which applies Redis' rules.
// On kInvalid, `*error` says why.
inline ParseStatus ParseLengthLine(std::string_view input, size_t start,
                                   ProtocolError too_long,
                                   ProtocolError invalid,
                                   LengthLine* const line,
                                   ProtocolError* const error) {
  constexpr size_t kMaxFastDigits = 18;
  size_t position = start;
  int64_t value = 0;
  const size_t limit = std::min(input.size(), start + kMaxFastDigits);
  for (; position < limit; ++position) {
    // Wraps for bytes below '0', so one comparison rejects every non-digit.
    const auto digit = static_cast<unsigned char>(input[position] - '0');
    if (digit > 9) {
      break;
    }
    value = (value * 10) + digit;
  }
  const size_t digits = position - start;
  if (digits == 0 || (digits > 1 && input[start] == '0') ||
      position + 1 >= input.size() || input[position] != '\r') {
    return ParseLengthLineSlowly(input, start, too_long, invalid, line, error);
  }
  line->value = value;
  line->end = position + kCrlfSize;
  return ParseStatus::kComplete;
}

struct BulkString {
  size_t offset{};
  size_t length{};
  // Offset just past the trailing line ending.
  size_t end{};
};

// Parses the bulk string at `start`. When only its header has arrived,
// returns kIncomplete with `bulk->end` set to where the payload will end. On
// kInvalid, `*failure` says why.
inline ParseStatus ParseBulkString(std::string_view input, size_t start,
                                   bool authenticated, BulkString* const bulk,
                                   ParseResult* const failure) {
  bulk->end = 0;
  if (start >= input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (input[start] != '$') {
    failure->error = ProtocolError::kExpectedDollar;
    failure->unexpected = input[start];
    return ParseStatus::kInvalid;
  }
  LengthLine line;
  const ParseStatus header = ParseLengthLine(
      input, start + 1, ProtocolError::kTooBigBulkCount,
      ProtocolError::kInvalidBulkLength, &line, &failure->error);
  if (header != ParseStatus::kComplete) {
    return header;
  }
  if (line.value < 0 || line.value > kMaxBulkBytes) {
    failure->error = ProtocolError::kInvalidBulkLength;
    return ParseStatus::kInvalid;
  }
  if (!authenticated && line.value > kMaxUnauthenticatedBulkBytes) {
    failure->error = ProtocolError::kUnauthenticatedBulkLength;
    return ParseStatus::kInvalid;
  }
  bulk->offset = line.end;
  bulk->length = static_cast<size_t>(line.value);
  // Like Redis, the two bytes after the payload are taken as its line
  // ending without checking them.
  bulk->end = line.end + bulk->length + kCrlfSize;
  return bulk->end > input.size() ? ParseStatus::kIncomplete
                                  : ParseStatus::kComplete;
}

// Upper bound for reserving `count` elements, given what the buffered bytes
// could hold, so a forged count cannot force a huge allocation.
size_t ReservableElements(std::string_view input, size_t count) {
  return std::min(count, (input.size() / kMinimumBulkStringBytes) + 1);
}

// Parses a request that starts at the beginning of `input` and has not been
// partially parsed before. Usually the whole request is buffered, so views
// go straight into `command` and `args`. Otherwise the progress so far is
// saved in `state` as offsets, since views would dangle once the caller's
// buffer moves, and ResumeResp continues from there after more bytes arrive.
ParseResult ParseFreshResp(std::string_view input, ParseState* const state,
                           std::string_view* const command,
                           commands::CommandArgs* const args) {
  LengthLine header;
  ParseResult result;
  result.status = ParseLengthLine(
      input, 1, ProtocolError::kTooBigMultibulkCount,
      ProtocolError::kInvalidMultibulkLength, &header, &result.error);
  if (result.status != ParseStatus::kComplete) {
    return result;
  }
  if (header.value > INT_MAX) {
    return {.status = ParseStatus::kInvalid,
            .error = ProtocolError::kInvalidMultibulkLength};
  }
  if (!state->authenticated && header.value > kMaxUnauthenticatedElements) {
    return {.status = ParseStatus::kInvalid,
            .error = ProtocolError::kUnauthenticatedMultibulkLength};
  }
  if (header.value <= 0) {
    // Redis skips empty and null arrays.
    return {.status = ParseStatus::kComplete,
            .empty_request = true,
            .consumed = header.end};
  }
  const auto count = static_cast<size_t>(header.value);
  args->reserve(ReservableElements(input, count - 1));
  size_t offset = header.end;
  for (size_t index = 0; index < count; ++index) {
    BulkString bulk;
    result.status =
        ParseBulkString(input, offset, state->authenticated, &bulk, &result);
    if (result.status == ParseStatus::kInvalid) {
      return result;
    }
    if (result.status == ParseStatus::kIncomplete) {
      state->element_count = count;
      state->offset = offset;
      state->pending_bulk_end = bulk.end;
      state->elements.reserve(ReservableElements(input, count));
      const auto save = [input, state](std::string_view element) {
        state->elements.emplace_back(element.data() - input.data(),
                                     element.size());
      };
      if (index > 0) {
        save(*command);
        std::ranges::for_each(*args, save);
      }
      return result;
    }
    const std::string_view element = input.substr(bulk.offset, bulk.length);
    if (index == 0) {
      *command = element;
    } else {
      args->push_back(element);
    }
    offset = bulk.end;
  }
  return {.status = ParseStatus::kComplete, .consumed = offset};
}

// Continues a request that ParseFreshResp could not finish, after the last
// complete element instead of re-parsing the request.
ParseResult ResumeResp(std::string_view input, ParseState* const state) {
  while (state->elements.size() < state->element_count) {
    BulkString bulk;
    ParseResult result;
    result.status = ParseBulkString(input, state->offset, state->authenticated,
                                    &bulk, &result);
    if (result.status != ParseStatus::kComplete) {
      state->pending_bulk_end = bulk.end;
      return result;
    }
    state->elements.emplace_back(bulk.offset, bulk.length);
    state->offset = bulk.end;
  }
  state->pending_bulk_end = 0;
  return {.status = ParseStatus::kComplete, .consumed = state->offset};
}

// Splits one inline command, a line of space-separated arguments that may be
// quoted, as Redis' processInlineBuffer does.
ParseResult ParseInline(std::string_view input, ParseState* const state,
                        std::string_view* const command,
                        commands::CommandArgs* const args) {
  const size_t newline =
      input.substr(0, std::min(input.size(), kMaxLineBytes + 1)).find('\n');
  if (newline == std::string_view::npos) {
    return {.status = input.size() > kMaxLineBytes ? ParseStatus::kInvalid
                                                   : ParseStatus::kIncomplete,
            .error = ProtocolError::kTooBigInlineRequest};
  }
  size_t line_end = newline;
  if (line_end > 0 && input[line_end - 1] == '\r') {
    --line_end;
  }
  std::vector<std::string_view>& tokens = state->inline_tokens;
  tokens.clear();
  const bool split = utils::SplitArgs(
      input.substr(0, line_end),
      [state, &tokens](std::string_view argument, bool quoted) {
        // Quoted arguments are unescaped copies; the deque keeps each one,
        // and the views into it, in place until the requests have run.
        tokens.push_back(
            quoted ? std::string_view(state->unescaped.emplace_back(argument))
                   : argument);
      });
  if (!split) {
    return {.status = ParseStatus::kInvalid,
            .error = ProtocolError::kUnbalancedQuotes};
  }
  if (tokens.empty()) {
    // An empty line is skipped, as in Redis.
    return {.status = ParseStatus::kComplete,
            .empty_request = true,
            .consumed = newline + 1};
  }
  *command = tokens.front();
  args->assign(tokens.begin() + 1, tokens.end());
  return {.status = ParseStatus::kComplete, .consumed = newline + 1};
}
}  // namespace

void ParseState::Reset() {
  offset = 0;
  element_count = 0;
  pending_bulk_end = 0;
  elements.clear();
}

ParseResult Parse(std::string_view input, std::string_view* const command,
                  commands::CommandArgs* const args) {
  if (input.empty() || input.front() != '*') {
    // Inline requests may need storage that outlives this call.
    *command = {};
    args->clear();
    return {.status = input.empty() ? ParseStatus::kIncomplete
                                    : ParseStatus::kInvalid};
  }
  ParseState state;
  return Parse(input, &state, command, args);
}

std::string ProtocolErrorMessage(const ParseResult& result) {
  switch (result.error) {
    case ProtocolError::kNone:
      break;
    case ProtocolError::kTooBigInlineRequest:
      return "Protocol error: too big inline request";
    case ProtocolError::kUnbalancedQuotes:
      return "Protocol error: unbalanced quotes in request";
    case ProtocolError::kTooBigMultibulkCount:
      return "Protocol error: too big mbulk count string";
    case ProtocolError::kInvalidMultibulkLength:
      return "Protocol error: invalid multibulk length";
    case ProtocolError::kUnauthenticatedMultibulkLength:
      return "Protocol error: unauthenticated multibulk length";
    case ProtocolError::kTooBigBulkCount:
      return "Protocol error: too big bulk count string";
    case ProtocolError::kExpectedDollar:
      return std::string("Protocol error: expected '$', got '") +
             result.unexpected + "'";
    case ProtocolError::kInvalidBulkLength:
      return "Protocol error: invalid bulk length";
    case ProtocolError::kUnauthenticatedBulkLength:
      return "Protocol error: unauthenticated bulk length";
  }
  return "Protocol error";
}

ParseResult Parse(std::string_view input, ParseState* const state,
                  std::string_view* const command,
                  commands::CommandArgs* const args) {
  if (state == nullptr || command == nullptr || args == nullptr) {
    return {.status = ParseStatus::kInvalid};
  }
  *command = {};
  args->clear();
  if (input.empty()) {
    return {.status = ParseStatus::kIncomplete};
  }
  if (state->element_count == 0 && input.front() != '*') {
    const ParseResult result = ParseInline(input, state, command, args);
    if (result.status != ParseStatus::kComplete) {
      *command = {};
      args->clear();
    }
    return result;
  }

  if (state->element_count == 0) {
    const ParseResult result = ParseFreshResp(input, state, command, args);
    if (result.status != ParseStatus::kComplete) {
      *command = {};
      args->clear();
    }
    if (result.status == ParseStatus::kInvalid) {
      state->Reset();
    }
    return result;
  }

  const ParseResult result = ResumeResp(input, state);
  if (result.status == ParseStatus::kIncomplete) {
    return result;
  }
  if (result.status == ParseStatus::kComplete) {
    const std::vector<std::pair<size_t, size_t>>& elements = state->elements;
    *command = input.substr(elements.front().first, elements.front().second);
    args->reserve(elements.size() - 1);
    for (size_t index = 1; index < elements.size(); ++index) {
      args->push_back(
          input.substr(elements[index].first, elements[index].second));
    }
  }
  state->Reset();
  return result;
}
}  // namespace redis_simple::request_parser
