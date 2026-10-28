#include "cli/resp_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/string_utils.h"

namespace redis_simple::cli::resp_parser {
namespace {
constexpr std::string_view kNilReply = "(nil)";

ParseResult ParseAt(std::string_view resp, size_t start,
                    std::vector<std::string>* reply);

ParseResult FindLineEnd(std::string_view resp, size_t start) {
  const size_t end = resp.find('\r', start);
  if (end == std::string_view::npos || end + 1 >= resp.size()) {
    return {ParseStatus::kIncomplete, 0};
  }
  if (resp[end + 1] != '\n') {
    return {ParseStatus::kInvalid, 0};
  }
  return {ParseStatus::kComplete, end};
}

bool ParseSize(std::string_view value, size_t* const result) {
  if (value.empty()) {
    return false;
  }
  size_t parsed = 0;
  for (const char character : value) {
    if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
      return false;
    }
    const auto digit = static_cast<size_t>(character - '0');
    if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;
    }
    parsed = (parsed * 10) + digit;
  }
  *result = parsed;
  return true;
}

ParseResult ParseLine(std::string_view resp, size_t start,
                      std::vector<std::string>* const reply) {
  const auto line = FindLineEnd(resp, start + 1);
  if (line.status != ParseStatus::kComplete) {
    return line;
  }
  reply->emplace_back(resp.substr(start + 1, line.consumed - start - 1));
  return {ParseStatus::kComplete, line.consumed - start + 2};
}

ParseResult ParseBulkString(std::string_view resp, size_t start,
                            std::vector<std::string>* const reply) {
  const auto header = FindLineEnd(resp, start + 1);
  if (header.status != ParseStatus::kComplete) {
    return header;
  }
  const std::string_view length_text =
      resp.substr(start + 1, header.consumed - start - 1);
  if (length_text == "-1") {
    reply->emplace_back(kNilReply);
    return {ParseStatus::kComplete, header.consumed - start + 2};
  }
  size_t length = 0;
  if (!ParseSize(length_text, &length)) {
    return {ParseStatus::kInvalid, 0};
  }
  const size_t content_start = header.consumed + 2;
  if (length > std::numeric_limits<size_t>::max() - content_start - 2) {
    return {ParseStatus::kInvalid, 0};
  }
  const size_t response_end = content_start + length + 2;
  if (response_end > resp.size()) {
    return {ParseStatus::kIncomplete, 0};
  }
  if (resp[response_end - 2] != '\r' || resp[response_end - 1] != '\n') {
    return {ParseStatus::kInvalid, 0};
  }
  reply->emplace_back(resp.substr(content_start, length));
  return {ParseStatus::kComplete, response_end - start};
}

ParseResult ParseInteger(std::string_view resp, size_t start,
                         std::vector<std::string>* const reply) {
  const auto line = FindLineEnd(resp, start + 1);
  if (line.status != ParseStatus::kComplete) {
    return line;
  }
  int64_t value = 0;
  if (!utils::ToInt64(resp.substr(start + 1, line.consumed - start - 1),
                      &value)) {
    return {ParseStatus::kInvalid, 0};
  }
  reply->push_back(std::to_string(value));
  return {ParseStatus::kComplete, line.consumed - start + 2};
}

ParseResult ParseAggregate(std::string_view resp, size_t start,
                           size_t element_multiplier,
                           std::vector<std::string>* const reply) {
  const auto header = FindLineEnd(resp, start + 1);
  if (header.status != ParseStatus::kComplete) {
    return header;
  }
  size_t length = 0;
  if (!ParseSize(resp.substr(start + 1, header.consumed - start - 1),
                 &length) ||
      length > std::numeric_limits<size_t>::max() / element_multiplier) {
    return {ParseStatus::kInvalid, 0};
  }
  size_t consumed = header.consumed - start + 2;
  const size_t element_count = length * element_multiplier;
  for (size_t index = 0; index < element_count; ++index) {
    const auto element = ParseAt(resp, start + consumed, reply);
    if (element.status != ParseStatus::kComplete) {
      return element;
    }
    if (element.consumed > std::numeric_limits<size_t>::max() - consumed) {
      return {ParseStatus::kInvalid, 0};
    }
    consumed += element.consumed;
  }
  reply->emplace_back("\n");
  return {ParseStatus::kComplete, consumed};
}

ParseResult ParseArray(std::string_view resp, size_t start,
                       std::vector<std::string>* const reply) {
  return ParseAggregate(resp, start, 1, reply);
}

ParseResult ParseMap(std::string_view resp, size_t start,
                     std::vector<std::string>* const reply) {
  return ParseAggregate(resp, start, 2, reply);
}

ParseResult ParseSet(std::string_view resp, size_t start,
                     std::vector<std::string>* const reply) {
  return ParseAggregate(resp, start, 1, reply);
}

ParseResult ParseNull(std::string_view resp, size_t start,
                      std::vector<std::string>* const reply) {
  const auto line = FindLineEnd(resp, start + 1);
  if (line.status != ParseStatus::kComplete) {
    return line;
  }
  if (line.consumed != start + 1) {
    return {ParseStatus::kInvalid, 0};
  }
  reply->emplace_back(kNilReply);
  return {ParseStatus::kComplete, 3};
}

bool ParseFloatText(std::string_view value, std::string* const rendered) {
  bool negative = false;
  if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
    negative = value.front() == '-';
    value.remove_prefix(1);
  }
  if (value.empty()) {
    return false;
  }

  const size_t exponent_pos = value.find_first_of("eE");
  if (exponent_pos != std::string_view::npos &&
      value.find_first_of("eE", exponent_pos + 1) != std::string_view::npos) {
    return false;
  }
  std::string_view mantissa = value.substr(0, exponent_pos);
  std::string_view exponent = exponent_pos == std::string_view::npos
                                  ? std::string_view()
                                  : value.substr(exponent_pos + 1);
  const size_t decimal_pos = mantissa.find('.');
  if (decimal_pos != std::string_view::npos &&
      mantissa.find('.', decimal_pos + 1) != std::string_view::npos) {
    return false;
  }
  std::string_view integral = mantissa.substr(0, decimal_pos);
  std::string_view fractional = decimal_pos == std::string_view::npos
                                    ? std::string_view()
                                    : mantissa.substr(decimal_pos + 1);
  if (integral.empty() && fractional.empty()) {
    return false;
  }
  const auto all_digits = [](std::string_view text) {
    return std::ranges::all_of(text, [](unsigned char character) {
      return std::isdigit(character) != 0;
    });
  };
  if ((!integral.empty() && !all_digits(integral)) ||
      (!fractional.empty() && !all_digits(fractional))) {
    return false;
  }

  bool negative_exponent = false;
  if (exponent_pos != std::string_view::npos) {
    if (!exponent.empty() &&
        (exponent.front() == '+' || exponent.front() == '-')) {
      negative_exponent = exponent.front() == '-';
      exponent.remove_prefix(1);
    }
    if (exponent.empty() || !all_digits(exponent)) {
      return false;
    }
  }

  while (integral.size() > 1 && integral.front() == '0') {
    integral.remove_prefix(1);
  }
  while (!fractional.empty() && fractional.back() == '0') {
    fractional.remove_suffix(1);
  }
  while (exponent.size() > 1 && exponent.front() == '0') {
    exponent.remove_prefix(1);
  }

  if (negative) {
    rendered->push_back('-');
  }
  rendered->append(integral.empty() ? "0" : integral);
  if (!fractional.empty()) {
    rendered->push_back('.');
    rendered->append(fractional);
  }
  if (!exponent.empty() && exponent != "0") {
    rendered->push_back('e');
    if (negative_exponent) {
      rendered->push_back('-');
    }
    rendered->append(exponent);
  }
  return true;
}

ParseResult ParseFloat(std::string_view resp, size_t start,
                       std::vector<std::string>* const reply) {
  const auto line = FindLineEnd(resp, start + 1);
  if (line.status != ParseStatus::kComplete) {
    return line;
  }
  std::string rendered;
  if (!ParseFloatText(resp.substr(start + 1, line.consumed - start - 1),
                      &rendered)) {
    return {ParseStatus::kInvalid, 0};
  }
  reply->push_back(std::move(rendered));
  return {ParseStatus::kComplete, line.consumed - start + 2};
}

ParseResult ParseAt(std::string_view resp, size_t start,
                    std::vector<std::string>* const reply) {
  if (start >= resp.size()) {
    return {ParseStatus::kIncomplete, 0};
  }
  switch (resp[start]) {
    case '+':
    case '-':
      return ParseLine(resp, start, reply);
    case '$':
      return ParseBulkString(resp, start, reply);
    case ':':
      return ParseInteger(resp, start, reply);
    case '*':
      return ParseArray(resp, start, reply);
    case '%':
      return ParseMap(resp, start, reply);
    case '~':
      return ParseSet(resp, start, reply);
    case '_':
      return ParseNull(resp, start, reply);
    case ',':
      return ParseFloat(resp, start, reply);
    default:
      return {ParseStatus::kInvalid, 0};
  }
}
}  // namespace

ParseResult Parse(std::string_view resp, std::vector<std::string>& reply) {
  const size_t original_size = reply.size();
  const auto result = ParseAt(resp, 0, &reply);
  if (result.status != ParseStatus::kComplete) {
    reply.resize(original_size);
  }
  return result;
}
}  // namespace redis_simple::cli::resp_parser
