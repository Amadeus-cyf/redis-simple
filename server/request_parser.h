#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "server/commands/command.h"

namespace redis_simple::request_parser {
enum class ParseStatus {
  kComplete,
  kIncomplete,
  kInvalid,
};

struct ParseResult {
  ParseStatus status;
  size_t consumed;
};

// Progress through one partially received RESP array request. Element
// positions are offsets from the start of the request, so the state stays
// valid when the caller's buffer is compacted or reallocated between reads.
struct ParseState {
  void Reset();
  // Bytes still missing from the bulk string currently being received, given
  // `buffered` bytes of the request. Zero when no bulk string is pending.
  size_t PendingBulkBytes(size_t buffered) const {
    return pending_bulk_end > buffered ? pending_bulk_end - buffered : 0;
  }

  // Offset of the first byte not yet parsed.
  size_t offset{};
  // Element count announced by the array header; zero until it is parsed.
  size_t element_count{};
  // End offset of the bulk string whose header was parsed but whose payload
  // has not fully arrived; zero otherwise.
  size_t pending_bulk_end{};
  // (offset, length) of each element parsed so far.
  std::vector<std::pair<size_t, size_t>> elements;
};

// Parses one inline or RESP array request. Returned views borrow from input.
// `state` carries progress for a request split across reads: pass the same
// state, and an input that starts at the same request, until the result is
// complete or invalid.
ParseResult Parse(std::string_view input, ParseState* state,
                  std::string_view* command, command::CommandArgs* args);

// Parses one request that must be fully contained in `input`'s prefix.
ParseResult Parse(std::string_view input, std::string_view* command,
                  command::CommandArgs* args);
}  // namespace redis_simple::request_parser
