#include "server/aof/manifest.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/check.h"
#include "utils/split_args.h"

namespace redis_simple::aof {
namespace {
constexpr std::string_view kBaseSuffix = ".base";
constexpr std::string_view kAofFormatSuffix = ".aof";
constexpr std::string_view kRdbFormatSuffix = ".rdb";
constexpr std::string_view kIncrementSuffix = ".incr.aof";
constexpr std::string_view kManifestSuffix = ".manifest";

char TypeCode(ManifestFile::Type type) {
  switch (type) {
    case ManifestFile::Type::kBase:
      return 'b';
    case ManifestFile::Type::kIncrement:
      return 'i';
    case ManifestFile::Type::kHistory:
      return 'h';
  }
  utils::Unreachable();
}

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

bool IsPrint(char c) { return c >= 0x20 && c < 0x7f; }

// Whether a manifest field must be quoted (Redis' sdsneedsrepr).
bool NeedsQuoting(std::string_view value) {
  return std::ranges::any_of(value, [](char c) {
    return c == '\\' || c == '"' || !IsPrint(c) || IsSpace(c);
  });
}

// Appends `value` in double quotes, escaping quotes, backslashes, and
// unprintable bytes (Redis' sdscatrepr).
void AppendQuoted(std::string_view value, std::string* const output) {
  output->push_back('"');
  for (const char c : value) {
    switch (c) {
      case '\\':
      case '"':
        output->push_back('\\');
        output->push_back(c);
        break;
      case '\n':
        output->append("\\n");
        break;
      case '\r':
        output->append("\\r");
        break;
      case '\t':
        output->append("\\t");
        break;
      case '\a':
        output->append("\\a");
        break;
      case '\b':
        output->append("\\b");
        break;
      default:
        if (IsPrint(c)) {
          output->push_back(c);
        } else {
          constexpr std::string_view kHex = "0123456789abcdef";
          const auto byte = static_cast<unsigned char>(c);
          output->append("\\x");
          output->push_back(kHex[byte >> 4]);
          output->push_back(kHex[byte & 0xf]);
        }
        break;
    }
  }
  output->push_back('"');
}

// Reads a number as C's atoll does, as Redis reads manifest numbers: leading
// whitespace and a sign, then digits up to the first other character; 0 if
// there are none, and clamped to the int64_t range.
int64_t ParseLeadingInteger(std::string_view text) {
  size_t position = 0;
  while (position < text.size() && IsSpace(text[position])) {
    ++position;
  }
  bool negative = false;
  if (position < text.size() &&
      (text[position] == '+' || text[position] == '-')) {
    negative = text[position] == '-';
    ++position;
  }
  uint64_t magnitude = 0;
  constexpr uint64_t kLimit = uint64_t{1} << 63;
  for (;
       position < text.size() && text[position] >= '0' && text[position] <= '9';
       ++position) {
    const auto digit = static_cast<uint64_t>(text[position] - '0');
    magnitude =
        magnitude > (kLimit - digit) / 10 ? kLimit : (magnitude * 10) + digit;
  }
  if (negative) {
    return magnitude >= kLimit ? std::numeric_limits<int64_t>::min()
                               : -static_cast<int64_t>(magnitude);
  }
  return magnitude >= kLimit ? std::numeric_limits<int64_t>::max()
                             : static_cast<int64_t>(magnitude);
}

bool EqualsIgnoringCase(std::string_view left, std::string_view right) {
  return std::ranges::equal(left, right, [](char a, char b) {
    const auto lower = [](char c) {
      return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return lower(a) == lower(b);
  });
}

std::string_view TrimBlanks(std::string_view line) {
  constexpr std::string_view kBlanks = " \t\r";
  const size_t first = line.find_first_not_of(kBlanks);
  if (first == std::string_view::npos) {
    return {};
  }
  return line.substr(first, line.find_last_not_of(kBlanks) - first + 1);
}

// Parses one manifest line, such as "file appendonly.aof.1.incr.aof seq 1
// type i": key/value pairs in any order. Redis requires the name, a nonzero
// sequence, and the type, and ignores keys it does not know, for forward
// compatibility.
std::optional<ManifestFile> ParseLine(std::string_view line) {
  line = TrimBlanks(line);
  if (line.empty()) {
    return std::nullopt;
  }
  std::vector<std::string> fields;
  if (!utils::SplitArgs(line,
                        [&fields](std::string_view field, bool /*quoted*/) {
                          fields.emplace_back(field);
                        }) ||
      fields.size() < 6 || fields.size() % 2 != 0) {
    return std::nullopt;
  }
  ManifestFile file;
  bool has_name = false;
  char type_code = '\0';
  for (size_t index = 0; index < fields.size(); index += 2) {
    const std::string& key = fields[index];
    std::string& value = fields[index + 1];
    if (EqualsIgnoringCase(key, "file")) {
      // Redis' pathIsBaseName: a name, not a path.
      if (value.find_first_of("/\\") != std::string::npos) {
        return std::nullopt;
      }
      file.name = std::move(value);
      has_name = true;
    } else if (EqualsIgnoringCase(key, "seq")) {
      file.sequence = ParseLeadingInteger(value);
    } else if (EqualsIgnoringCase(key, "type")) {
      type_code = value.empty() ? '\0' : value.front();
    } else if (EqualsIgnoringCase(key, "startoffset")) {
      file.start_offset = ParseLeadingInteger(value);
    } else if (EqualsIgnoringCase(key, "endoffset")) {
      file.end_offset = ParseLeadingInteger(value);
    }
  }
  if (!has_name || file.sequence == 0) {
    return std::nullopt;
  }
  switch (type_code) {
    case 'b':
      file.type = ManifestFile::Type::kBase;
      break;
    case 'h':
      file.type = ManifestFile::Type::kHistory;
      break;
    case 'i':
      file.type = ManifestFile::Type::kIncrement;
      break;
    default:
      return std::nullopt;
  }
  return file;
}
}  // namespace

ManifestFile MakeManifestFile(std::string name, int64_t sequence,
                              ManifestFile::Type type) {
  ManifestFile file;
  file.name = std::move(name);
  file.sequence = sequence;
  file.type = type;
  return file;
}

std::string BaseFileName(std::string_view prefix, int64_t sequence,
                         bool rdb_format) {
  std::string name(prefix);
  name.append(".")
      .append(std::to_string(sequence))
      .append(kBaseSuffix)
      .append(rdb_format ? kRdbFormatSuffix : kAofFormatSuffix);
  return name;
}

std::string IncrementFileName(std::string_view prefix, int64_t sequence) {
  std::string name(prefix);
  name.append(".").append(std::to_string(sequence)).append(kIncrementSuffix);
  return name;
}

std::string ManifestFileName(std::string_view prefix) {
  std::string name(prefix);
  name.append(kManifestSuffix);
  return name;
}

bool ValidFileName(std::string_view name) {
  return !name.empty() && name.find_first_of("/\\") == std::string_view::npos;
}

std::string Manifest::Encode() const {
  std::string text;
  const auto append = [&text](const ManifestFile& file) {
    text.append("file ");
    if (NeedsQuoting(file.name)) {
      AppendQuoted(file.name, &text);
    } else {
      text.append(file.name);
    }
    text.append(" seq ")
        .append(std::to_string(file.sequence))
        .append(" type ")
        .push_back(TypeCode(file.type));
    if (file.start_offset.has_value()) {
      text.append(" startoffset ").append(std::to_string(*file.start_offset));
      if (file.end_offset.has_value()) {
        text.append(" endoffset ").append(std::to_string(*file.end_offset));
      }
    }
    text.push_back('\n');
  };
  if (base.has_value()) {
    append(*base);
  }
  for (const ManifestFile& file : history) {
    append(file);
  }
  for (const ManifestFile& file : increments) {
    append(file);
  }
  return text;
}

std::optional<Manifest> Manifest::Parse(std::string_view text) {
  // Redis reads lines of at most this many bytes.
  constexpr size_t kMaxLineBytes = 1024;
  if (text.empty()) {
    return std::nullopt;
  }
  Manifest manifest;
  while (!text.empty()) {
    const size_t line_end = text.find('\n');
    const bool has_newline = line_end != std::string_view::npos;
    const std::string_view line = text.substr(0, line_end);
    text = has_newline ? text.substr(line_end + 1) : std::string_view();
    if (line.starts_with('#')) {
      continue;
    }
    if (!has_newline || line.size() + 1 > kMaxLineBytes) {
      return std::nullopt;
    }
    std::optional<ManifestFile> file = ParseLine(line);
    if (!file.has_value()) {
      return std::nullopt;
    }
    switch (file->type) {
      case ManifestFile::Type::kBase:
        if (manifest.base.has_value()) {
          return std::nullopt;
        }
        manifest.base_sequence = file->sequence;
        manifest.base = std::move(*file);
        break;
      case ManifestFile::Type::kHistory:
        manifest.history.push_back(std::move(*file));
        break;
      case ManifestFile::Type::kIncrement:
        // Incremental files replay in order.
        if (file->sequence <= manifest.increment_sequence) {
          return std::nullopt;
        }
        manifest.increment_sequence = file->sequence;
        manifest.increments.push_back(std::move(*file));
        break;
    }
  }
  return manifest;
}
}  // namespace redis_simple::aof
