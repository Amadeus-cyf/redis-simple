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
  size_t consumed{};
};

// Parse one response and append its rendered values to reply.
ParseResult Parse(std::string_view resp, std::vector<std::string>& reply);
}  // namespace redis_simple::cli::resp_parser

#endif  // REDIS_SIMPLE_CLI_RESP_PARSER_H_
