#ifndef REDIS_SIMPLE_UTILS_SPLIT_ARGS_H_
#define REDIS_SIMPLE_UTILS_SPLIT_ARGS_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace redis_simple::utils {
namespace split_args_internal {
constexpr bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

constexpr int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

constexpr char Unescape(char c) {
  switch (c) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case 'b':
      return '\b';
    case 'a':
      return '\a';
    default:
      return c;
  }
}
}  // namespace split_args_internal

// Splits a line into arguments as Redis' sdssplitargs does, for inline
// requests and AOF manifests: whitespace separates arguments, which may be
// "double quoted", with \n, \r, \t, \b, \a, \xHH, and backslash escapes, or
// 'single quoted', where only \' is an escape. A closing quote must be
// followed by whitespace or the end of the line. A NUL byte ends the line,
// since sdssplitargs reads a C string.
//
// Calls `on_argument(argument, quoted)` for each argument in order. An
// unquoted argument is a view into `line`; a quoted one is unescaped into a
// buffer that is reused for the next argument, so copy it to keep it.
// Returns false, possibly after some calls, if quotes are unbalanced.
template <typename OnArgument>
bool SplitArgs(std::string_view line, OnArgument&& on_argument) {
  using split_args_internal::HexValue;
  using split_args_internal::IsSpace;
  line = line.substr(0, line.find('\0'));
  const auto at = [line](size_t index) {
    return index < line.size() ? line[index] : '\0';
  };
  // A closing quote must end the argument.
  const auto ends_argument = [&at](size_t index) {
    return at(index) == '\0' || IsSpace(at(index));
  };
  std::string unescaped;
  size_t position = 0;
  while (true) {
    while (position < line.size() && IsSpace(line[position])) {
      ++position;
    }
    if (position >= line.size()) {
      return true;
    }
    // An argument is plain text, optionally ending in one quoted part that
    // runs to the argument's end: foo"bar baz" is the argument foobar baz.
    // sdssplitargs ends plain text at these four characters only.
    const size_t start = position;
    while (position < line.size() && line[position] != ' ' &&
           line[position] != '\n' && line[position] != '\r' &&
           line[position] != '\t' && line[position] != '"' &&
           line[position] != '\'') {
      ++position;
    }
    if (position >= line.size() ||
        (line[position] != '"' && line[position] != '\'')) {
      on_argument(line.substr(start, position - start), false);
      continue;
    }
    const char opening = line[position];
    unescaped.assign(line.substr(start, position - start));
    ++position;
    while (true) {
      const char c = at(position);
      if (c == '\0') {
        return false;
      }
      if (c == opening) {
        if (!ends_argument(position + 1)) {
          return false;
        }
        ++position;
        break;
      }
      if (opening == '"' && c == '\\' && at(position + 1) == 'x' &&
          HexValue(at(position + 2)) >= 0 && HexValue(at(position + 3)) >= 0) {
        unescaped.push_back(static_cast<char>(
            (HexValue(at(position + 2)) * 16) + HexValue(at(position + 3))));
        position += 4;
      } else if (opening == '"' && c == '\\' && at(position + 1) != '\0') {
        unescaped.push_back(split_args_internal::Unescape(at(position + 1)));
        position += 2;
      } else if (opening == '\'' && c == '\\' && at(position + 1) == '\'') {
        unescaped.push_back('\'');
        position += 2;
      } else {
        unescaped.push_back(c);
        ++position;
      }
    }
    on_argument(std::string_view(unescaped), true);
  }
}
}  // namespace redis_simple::utils

#endif  // REDIS_SIMPLE_UTILS_SPLIT_ARGS_H_
