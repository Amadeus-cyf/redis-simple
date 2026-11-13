#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz/fuzz_input.h"
#include "server/commands/command.h"
#include "server/request_parser.h"

namespace redis_simple::fuzz {
namespace {
bool IsBorrowed(std::string_view value, std::string_view input) {
  if (value.empty()) {
    return true;
  }
  const auto input_begin = reinterpret_cast<uintptr_t>(input.data());
  const auto input_end = input_begin + input.size();
  const auto value_begin = reinterpret_cast<uintptr_t>(value.data());
  return value_begin >= input_begin && value_begin <= input_end &&
         value.size() <= input_end - value_begin;
}

void ParseOne(std::string_view input) {
  std::string_view command_name;
  commands::CommandArgs args;
  const auto result = request_parser::Parse(input, &command_name, &args);
  if (result.status != request_parser::ParseStatus::kComplete) {
    Require(command_name.empty());
    Require(args.empty());
    return;
  }

  Require(result.consumed > 0);
  Require(result.consumed <= input.size());
  Require(IsBorrowed(command_name, input));
  for (const auto argument : args) {
    Require(IsBorrowed(argument, input));
  }
}

// The client's path: a ParseState, which also takes inline requests, whose
// quoted arguments live in the state.
bool IsInState(std::string_view value,
               const request_parser::ParseState& state) {
  return std::ranges::any_of(state.unescaped, [value](const std::string& s) {
    return value.data() == s.data() && value.size() == s.size();
  });
}

void ParseWithState(std::string_view input) {
  request_parser::ParseState state;
  size_t consumed = 0;
  for (size_t command_count = 0; command_count < 64 && consumed < input.size();
       ++command_count) {
    std::string_view command_name;
    commands::CommandArgs args;
    const auto remaining = input.substr(consumed);
    const auto result =
        request_parser::Parse(remaining, &state, &command_name, &args);
    if (result.status != request_parser::ParseStatus::kComplete) {
      Require(command_name.empty());
      Require(args.empty());
      if (result.status == request_parser::ParseStatus::kInvalid) {
        Require(!request_parser::ProtocolErrorMessage(result).empty());
      }
      break;
    }
    Require(result.consumed > 0);
    Require(result.consumed <= remaining.size());
    Require(!result.empty_request || command_name.empty());
    Require(IsBorrowed(command_name, remaining) ||
            IsInState(command_name, state));
    for (const auto argument : args) {
      Require(IsBorrowed(argument, remaining) || IsInState(argument, state));
    }
    consumed += result.consumed;
  }
  state.ReleaseUnescaped();
}

void ParseSequence(std::string_view input) {
  size_t consumed = 0;
  for (size_t command_count = 0; command_count < 64 && consumed < input.size();
       ++command_count) {
    std::string_view command_name;
    commands::CommandArgs args;
    const auto remaining = input.substr(consumed);
    const auto result = request_parser::Parse(remaining, &command_name, &args);
    if (result.status != request_parser::ParseStatus::kComplete) {
      break;
    }
    Require(result.consumed > 0);
    Require(result.consumed <= remaining.size());
    consumed += result.consumed;
  }
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const char* bytes = size == 0 ? "" : reinterpret_cast<const char*>(data);
  const std::string_view input(bytes, size);
  redis_simple::fuzz::ParseOne(input);
  redis_simple::fuzz::ParseOne(input.substr(0, size / 2));
  redis_simple::fuzz::ParseSequence(input);
  redis_simple::fuzz::ParseWithState(input);
  return 0;
}
