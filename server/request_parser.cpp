#include "server/request_parser.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

namespace redis_simple::request_parser {
namespace {
constexpr size_t kMaxCommandElements = size_t{1024} * 1024;
// Longest inline command or RESP header line, matching Redis'
// PROTO_INLINE_MAX_SIZE. Larger lines are rejected instead of rescanned.
constexpr size_t kMaxLineBytes = size_t{64} * 1024;
constexpr size_t kMinimumBulkStringBytes = 6;
// Longest accepted length field. Any number this long fits in a size_t, so
// digits accumulate without overflow checks. Longer fields, at least 10^19
// bytes or elements, could never be satisfied and are rejected as invalid.
constexpr size_t kMaxLengthDigits = std::numeric_limits<size_t>::digits10;
constexpr std::string_view kCrlf = "\r\n";

// Parses the decimal length that follows a RESP type byte, starting at
// `start` and ending in CRLF, in one pass without searching for the CRLF
// first. At most kMaxLengthDigits digits are read, which also stops a peer
// from making each read rescan a long run of digits. On success, `*line_end`
// is the offset of the CR.
inline ParseStatus ParseLengthLine(std::string_view input, size_t start,
                                   size_t* const value,
                                   size_t* const line_end) {
  constexpr size_t kBase = 10;
  size_t parsed = 0;
  size_t position = start;
  for (; position < input.size(); ++position) {
    const char character = input[position];
    if (character < '0' || character > '9') {
      break;
    }
    if (position - start == kMaxLengthDigits) {
      return ParseStatus::kInvalid;
    }
    parsed = (parsed * kBase) + static_cast<size_t>(character - '0');
  }
  if (position == input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (position == start || input[position] != '\r') {
    return ParseStatus::kInvalid;
  }
  if (position + 1 == input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (input[position + 1] != '\n') {
    return ParseStatus::kInvalid;
  }
  *value = parsed;
  *line_end = position;
  return ParseStatus::kComplete;
}

struct BulkString {
  size_t offset{};
  size_t length{};
  // Offset just past the trailing CRLF.
  size_t end{};
};

// Parses the bulk string at `start`. When only its header has arrived,
// returns kIncomplete with `bulk->end` set to where the payload will end.
inline ParseStatus ParseBulkString(std::string_view input, size_t start,
                                   BulkString* const bulk) {
  bulk->end = 0;
  if (start >= input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (input[start] != '$') {
    return ParseStatus::kInvalid;
  }
  size_t length = 0;
  size_t header_end = 0;
  const ParseStatus header =
      ParseLengthLine(input, start + 1, &length, &header_end);
  if (header != ParseStatus::kComplete) {
    return header;
  }
  const size_t content_start = header_end + kCrlf.size();
  if (length >
      std::numeric_limits<size_t>::max() - content_start - kCrlf.size()) {
    return ParseStatus::kInvalid;
  }
  bulk->offset = content_start;
  bulk->length = length;
  bulk->end = content_start + length + kCrlf.size();
  if (bulk->end > input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (input[content_start + length] != '\r' ||
      input[content_start + length + 1] != '\n') {
    return ParseStatus::kInvalid;
  }
  return ParseStatus::kComplete;
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
                           command::CommandArgs* const args) {
  size_t count = 0;
  size_t header_end = 0;
  const ParseStatus header = ParseLengthLine(input, 1, &count, &header_end);
  if (header != ParseStatus::kComplete) {
    return {header, 0};
  }
  if (count == 0 || count > kMaxCommandElements) {
    return {ParseStatus::kInvalid, 0};
  }
  args->reserve(ReservableElements(input, count - 1));
  size_t offset = header_end + kCrlf.size();
  for (size_t index = 0; index < count; ++index) {
    BulkString bulk;
    const ParseStatus status = ParseBulkString(input, offset, &bulk);
    if (status == ParseStatus::kInvalid) {
      return {status, 0};
    }
    if (status == ParseStatus::kIncomplete) {
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
      return {status, 0};
    }
    const std::string_view element = input.substr(bulk.offset, bulk.length);
    if (index == 0) {
      *command = element;
    } else {
      args->push_back(element);
    }
    offset = bulk.end;
  }
  return {ParseStatus::kComplete, offset};
}

// Continues a request that ParseFreshResp could not finish, after the last
// complete element instead of re-parsing the request.
ParseResult ResumeResp(std::string_view input, ParseState* const state) {
  while (state->elements.size() < state->element_count) {
    BulkString bulk;
    const ParseStatus status = ParseBulkString(input, state->offset, &bulk);
    if (status != ParseStatus::kComplete) {
      state->pending_bulk_end = bulk.end;
      return {status, 0};
    }
    state->elements.emplace_back(bulk.offset, bulk.length);
    state->offset = bulk.end;
  }
  state->pending_bulk_end = 0;
  return {ParseStatus::kComplete, state->offset};
}

ParseResult ParseInline(std::string_view input, std::string_view* command,
                        command::CommandArgs* args) {
  const size_t newline =
      input.substr(0, std::min(input.size(), kMaxLineBytes)).find('\n');
  if (newline == std::string_view::npos) {
    return {input.size() >= kMaxLineBytes ? ParseStatus::kInvalid
                                          : ParseStatus::kIncomplete,
            0};
  }
  size_t line_end = newline;
  if (line_end > 0 && input[line_end - 1] == '\r') {
    --line_end;
  }
  const std::string_view line = input.substr(0, line_end);
  size_t position = line.find_first_not_of(' ');
  if (position == std::string_view::npos) {
    return {ParseStatus::kComplete, newline + 1};
  }

  const size_t command_end = line.find(' ', position);
  *command = line.substr(position, command_end == std::string_view::npos
                                       ? std::string_view::npos
                                       : command_end - position);
  position = command_end;
  while (position != std::string_view::npos) {
    const size_t start = line.find_first_not_of(' ', position);
    if (start == std::string_view::npos) {
      break;
    }
    const size_t end = line.find(' ', start);
    args->push_back(line.substr(start, end == std::string_view::npos
                                           ? std::string_view::npos
                                           : end - start));
    position = end;
  }
  return {ParseStatus::kComplete, newline + 1};
}
}  // namespace

void ParseState::Reset() {
  offset = 0;
  element_count = 0;
  pending_bulk_end = 0;
  elements.clear();
}

ParseResult Parse(std::string_view input, ParseState* const state,
                  std::string_view* const command,
                  command::CommandArgs* const args) {
  if (state == nullptr || command == nullptr || args == nullptr) {
    return {ParseStatus::kInvalid, 0};
  }
  *command = {};
  args->clear();
  if (input.empty()) {
    return {ParseStatus::kIncomplete, 0};
  }
  if (state->element_count == 0 && input.front() != '*') {
    const auto result = ParseInline(input, command, args);
    if (result.status != ParseStatus::kComplete) {
      *command = {};
      args->clear();
    }
    return result;
  }

  if (state->element_count == 0) {
    const auto result = ParseFreshResp(input, state, command, args);
    if (result.status != ParseStatus::kComplete) {
      *command = {};
      args->clear();
    }
    if (result.status == ParseStatus::kInvalid) {
      state->Reset();
    }
    return result;
  }

  const auto result = ResumeResp(input, state);
  if (result.status == ParseStatus::kIncomplete) {
    return result;
  }
  if (result.status == ParseStatus::kComplete) {
    const auto& elements = state->elements;
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

ParseResult Parse(std::string_view input, std::string_view* const command,
                  command::CommandArgs* const args) {
  ParseState state;
  return Parse(input, &state, command, args);
}
}  // namespace redis_simple::request_parser
