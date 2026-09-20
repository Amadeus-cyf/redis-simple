#pragma once

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#ifdef REDIS_SIMPLE_USE_SPDLOG
#include <spdlog/spdlog.h>
#endif

namespace redis_simple::logging {

enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3, kOff = 4 };

inline const char* LevelName(Level level) {
  switch (level) {
    case Level::kDebug:
      return "DEBUG";
    case Level::kInfo:
      return "INFO";
    case Level::kWarn:
      return "WARN";
    case Level::kError:
      return "ERROR";
    case Level::kOff:
      return "OFF";
  }
  return "UNKNOWN";
}

inline Level ParseLevel(const char* value) {
  if (value == nullptr) {
    return Level::kInfo;
  }
  std::string level(value);
  for (char& ch : level) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  if (level == "debug") {
    return Level::kDebug;
  }
  if (level == "info") {
    return Level::kInfo;
  }
  if (level == "warn" || level == "warning") {
    return Level::kWarn;
  }
  if (level == "error") {
    return Level::kError;
  }
  if (level == "off") {
    return Level::kOff;
  }
  return Level::kInfo;
}

inline Level& MinLevel() {
  static Level level = ParseLevel(std::getenv("REDIS_SIMPLE_LOG_LEVEL"));
  return level;
}

inline void SetLevel(Level level) { MinLevel() = level; }

inline bool Enabled(Level level) {
  return static_cast<int>(level) >= static_cast<int>(MinLevel()) &&
         MinLevel() != Level::kOff;
}

inline std::mutex& LogMutex() {
  static std::mutex mutex;
  return mutex;
}

#ifdef REDIS_SIMPLE_USE_SPDLOG
inline spdlog::level::level_enum ToSpdlogLevel(Level level) {
  switch (level) {
    case Level::kDebug:
      return spdlog::level::debug;
    case Level::kInfo:
      return spdlog::level::info;
    case Level::kWarn:
      return spdlog::level::warn;
    case Level::kError:
      return spdlog::level::err;
    case Level::kOff:
      return spdlog::level::off;
  }
  return spdlog::level::info;
}
#endif

inline void VLogf(Level level, const char* file, int line, const char* fmt,
                  va_list args) {
  if (!Enabled(level)) {
    return;
  }

  std::array<char, 1024> message{};
  va_list args_copy;
  va_copy(args_copy, args);
  const int n = std::vsnprintf(message.data(), message.size(), fmt, args_copy);
  va_end(args_copy);

  std::string rendered;
  if (n < 0) {
    rendered = fmt ? fmt : "";
  } else if (static_cast<size_t>(n) < message.size()) {
    rendered.assign(message.data(), static_cast<size_t>(n));
  } else {
    std::vector<char> large(static_cast<size_t>(n) + 1);
    std::vsnprintf(large.data(), large.size(), fmt, args);
    rendered.assign(large.data(), static_cast<size_t>(n));
  }

#ifdef REDIS_SIMPLE_USE_SPDLOG
  spdlog::set_level(ToSpdlogLevel(MinLevel()));
  spdlog::log(spdlog::source_loc{file, line, ""}, ToSpdlogLevel(level), "{}",
              rendered);
#else
  const auto now = std::chrono::system_clock::now();
  const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now_time);
#else
  localtime_r(&now_time, &tm);
#endif

  std::array<char, 32> timestamp{};
  std::strftime(timestamp.data(), timestamp.size(), "%Y-%m-%d %H:%M:%S", &tm);

  std::lock_guard<std::mutex> lock(LogMutex());
  std::fprintf(stderr, "[%s] [%s] [%s:%d] %s", timestamp.data(),
               LevelName(level), file, line, rendered.c_str());
  if (rendered.empty() || rendered.back() != '\n') {
    std::fprintf(stderr, "\n");
  }
#endif
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
inline void Logf(Level level, const char* file, int line, const char* fmt,
                 ...) {
  va_list args;
  va_start(args, fmt);
  VLogf(level, file, line, fmt, args);
  va_end(args);
}

}  // namespace redis_simple::logging

#define RS_LOG_DEBUG(...)                                               \
  ::redis_simple::logging::Logf(::redis_simple::logging::Level::kDebug, \
                                __FILE__, __LINE__, __VA_ARGS__)
#define RS_LOG_INFO(...)                                               \
  ::redis_simple::logging::Logf(::redis_simple::logging::Level::kInfo, \
                                __FILE__, __LINE__, __VA_ARGS__)
#define RS_LOG_WARN(...)                                               \
  ::redis_simple::logging::Logf(::redis_simple::logging::Level::kWarn, \
                                __FILE__, __LINE__, __VA_ARGS__)
#define RS_LOG_ERROR(...)                                               \
  ::redis_simple::logging::Logf(::redis_simple::logging::Level::kError, \
                                __FILE__, __LINE__, __VA_ARGS__)
