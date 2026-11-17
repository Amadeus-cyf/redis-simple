#ifndef REDIS_SIMPLE_CLI_RESP_PARSER_H_
#define REDIS_SIMPLE_CLI_RESP_PARSER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace redis_simple::cli::resp_parser {
enum class ParseStatus {
  kComplete,
  kIncomplete,
  kInvalid,
};

struct ParseResult {
  ParseStatus status{ParseStatus::kIncomplete};
  // Bytes of input the response takes; 0 unless `status` is kComplete.
  size_t consumed{};
};

// Parses one response and appends its rendered values to `reply`: one
// string per scalar, "(nil)" for a null, and a "\n" after each array, map,
// or set. Leaves `reply` unchanged unless the response is complete.
ParseResult Parse(std::string_view input, std::vector<std::string>& reply);
}  // namespace redis_simple::cli::resp_parser

#endif  // REDIS_SIMPLE_CLI_RESP_PARSER_H_
