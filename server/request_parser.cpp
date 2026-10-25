#include "server/request_parser.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string_view>
#include <system_error>

namespace redis_simple::request_parser {
namespace {
constexpr size_t kMaxCommandElements = size_t{1024} * 1024;
// Longest inline command or RESP header line, matching Redis'
// PROTO_INLINE_MAX_SIZE. Larger lines are rejected instead of rescanned.
constexpr size_t kMaxLineBytes = size_t{64} * 1024;
constexpr size_t kMinimumBulkStringBytes = 6;
constexpr std::string_view kCrlf = "\r\n";

// Finds the CRLF that ends the line starting at `start`, scanning at most
// kMaxLineBytes so a peer cannot force repeated scans of a huge buffer.
ParseResult FindLineEnd(std::string_view input, size_t start) {
  const size_t window = std::min(input.size() - start, kMaxLineBytes);
  const size_t end = input.substr(start, window).find(kCrlf);
  if (end != std::string_view::npos) {
    return {ParseStatus::kComplete, start + end};
  }
  return {window == kMaxLineBytes ? ParseStatus::kInvalid
                                  : ParseStatus::kIncomplete,
          0};
}

bool ParseSize(std::string_view value, size_t* const result) {
  if (value.empty()) {
    return false;
  }
  size_t parsed = 0;
  const auto conversion =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (conversion.ec != std::errc() ||
      conversion.ptr != value.data() + value.size()) {
    return false;
  }
  *result = parsed;
  return true;
}

struct BulkString {
  size_t offset{};
  size_t length{};
  // Offset just past the trailing CRLF.
  size_t end{};
};

// Parses the bulk string at `start`. When only its header has arrived,
// returns kIncomplete with `bulk->end` set to where the payload will end.
ParseStatus ParseBulkString(std::string_view input, size_t start,
                            BulkString* const bulk) {
  bulk->end = 0;
  if (start >= input.size()) {
    return ParseStatus::kIncomplete;
  }
  if (input[start] != '$') {
    return ParseStatus::kInvalid;
  }
  const auto header = FindLineEnd(input, start + 1);
  if (header.status != ParseStatus::kComplete) {
    return header.status;
  }
  size_t length = 0;
  if (!ParseSize(input.substr(start + 1, header.consumed - start - 1),
                 &length)) {
    return ParseStatus::kInvalid;
  }
  const size_t content_start = header.consumed + kCrlf.size();
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
  if (input.substr(content_start + length, kCrlf.size()) != kCrlf) {
    return ParseStatus::kInvalid;
  }
  return ParseStatus::kComplete;
}

ParseResult ParseResp(std::string_view input, ParseState* const state) {
  if (state->element_count == 0) {
    const auto header = FindLineEnd(input, 1);
    if (header.status != ParseStatus::kComplete) {
      return header;
    }
    size_t count = 0;
    if (!ParseSize(input.substr(1, header.consumed - 1), &count) ||
        count == 0 || count > kMaxCommandElements) {
      return {ParseStatus::kInvalid, 0};
    }
    state->element_count = count;
    state->offset = header.consumed + kCrlf.size();
    // Bound the reservation by what the buffered bytes could hold.
    state->elements.reserve(
        std::min(count, (input.size() / kMinimumBulkStringBytes) + 1));
  }

  // Resume after the last complete element instead of re-parsing the request.
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

  const auto result = ParseResp(input, state);
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
