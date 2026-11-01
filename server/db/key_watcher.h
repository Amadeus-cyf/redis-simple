#ifndef REDIS_SIMPLE_SERVER_DB_KEY_WATCHER_H_
#define REDIS_SIMPLE_SERVER_DB_KEY_WATCHER_H_

#include <string>
#include <vector>

namespace redis_simple::db {
class RedisDb;

// One connection's WATCH state. The database marks the watcher dirty when any
// watched key is modified, deleted, or expires, which makes the next EXEC
// fail. Register and release keys through RedisDb::WatchKey and
// RedisDb::UnwatchKeys; a watcher must be unwatched before it is destroyed.
class KeyWatcher {
 public:
  KeyWatcher() = default;
  KeyWatcher(const KeyWatcher&) = delete;
  KeyWatcher& operator=(const KeyWatcher&) = delete;
  bool Dirty() const { return dirty_; }
  bool Watching() const { return !keys_.empty(); }
  const std::vector<std::string>& Keys() const { return keys_; }

 private:
  friend class RedisDb;
  std::vector<std::string> keys_;
  bool dirty_{};
};
}  // namespace redis_simple::db

#endif  // REDIS_SIMPLE_SERVER_DB_KEY_WATCHER_H_
