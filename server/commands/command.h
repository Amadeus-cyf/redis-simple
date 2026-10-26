#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace redis_simple {
class Client;
}  // namespace redis_simple

namespace redis_simple::command {
using CommandArgs = std::vector<std::string_view>;
using CommandCallback = void (*)(Client* client);

enum class CommandAccess : std::uint8_t {
  kReadOnly,
  kWrite,
  kAdmin,
  kConnection,
};

struct CommandArity {
  size_t min;
  size_t max;

  constexpr bool Accepts(size_t count) const {
    return count >= min && count <= max;
  }
};

struct KeySpec {
  static constexpr size_t kAllRemaining = std::numeric_limits<size_t>::max();

  size_t first{};
  size_t last{};
  size_t step{};

  constexpr bool HasKeys() const { return step != 0; }
};

struct Command {
  std::string_view name;
  CommandCallback callback;
  CommandArity arity;
  CommandAccess access;
  KeySpec keys;
  // Commands that unauthenticated clients may run when a password is set.
  bool allowed_before_auth{false};
};

const Command* Find(std::string_view name);
}  // namespace redis_simple::command
