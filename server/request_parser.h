#ifndef REDIS_SIMPLE_SERVER_REQUEST_PARSER_H_
#define REDIS_SIMPLE_SERVER_REQUEST_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "server/commands/command.h"

namespace redis_simple::request_parser {
enum class ParseStatus : std::uint8_t {
  kComplete,
  kIncomplete,
  kInvalid,
};

// Why a request is invalid, for Redis' "Protocol error" replies.
enum class ProtocolError : std::uint8_t {
  kNone,
  kTooBigInlineRequest,
  kUnbalancedQuotes,
  kTooBigMultibulkCount,
  kInvalidMultibulkLength,
  kUnauthenticatedMultibulkLength,
  kTooBigBulkCount,
  kExpectedDollar,
  kInvalidBulkLength,
  kUnauthenticatedBulkLength,
};

// Small enough, at 16 bytes, to return in registers on the hot path.
struct ParseResult {
  ParseStatus status{};
  ProtocolError error{ProtocolError::kNone};
  // The byte found where kExpectedDollar expected a '$'.
  char unexpected{};
  // A complete request with nothing to run, which Redis skips: an empty
  // inline line, or an empty or null array.
  bool empty_request{};
  size_t consumed{};
};

// Redis' error text for an invalid request, without the "ERR " prefix.
std::string ProtocolErrorMessage(const ParseResult& result);

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
  // Frees the unescaped inline arguments, once nothing views them.
  void ReleaseUnescaped() { unescaped.clear(); }

  // Offset of the first byte not yet parsed.
  size_t offset{};
  // Element count announced by the array header; zero until it is parsed.
  size_t element_count{};
  // End offset of the bulk string whose header was parsed but whose payload
  // has not fully arrived; zero otherwise.
  size_t pending_bulk_end{};
  // (offset, length) of each element parsed so far.
  std::vector<std::pair<size_t, size_t>> elements;
  // Whether the client has authenticated; Redis limits request sizes until
  // it has.
  bool authenticated{true};
  // Inline arguments that were quoted, unescaped. Views returned by Parse
  // point here until ReleaseUnescaped().
  std::deque<std::string> unescaped;
  // Scratch space for splitting an inline request.
  std::vector<std::string_view> inline_tokens;
};

// Parses one inline or RESP array request, framed as Redis'
// processInlineBuffer and processMultibulkBuffer do. Returned views borrow
// from input, or for quoted inline arguments from `state`. `state` carries
// progress for a request split across reads: pass the same state, and an
// input that starts at the same request, until the result is complete or
// invalid.
ParseResult Parse(std::string_view input, ParseState* state,
                  std::string_view* command, command::CommandArgs* args);

// Parses one RESP array request that must be fully contained in `input`'s
// prefix, as the AOF stores them; inline requests are invalid here.
ParseResult Parse(std::string_view input, std::string_view* command,
                  command::CommandArgs* args);
}  // namespace redis_simple::request_parser

#endif  // REDIS_SIMPLE_SERVER_REQUEST_PARSER_H_
