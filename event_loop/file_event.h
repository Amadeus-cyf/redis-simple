#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace redis_simple::event_loop {
class Loop;
enum class Status;
// File event callbacks report success or failure with the loop's Status.
using CallbackStatus = Status;

class FileEvent {
 public:
  using Callback = std::function<CallbackStatus(Loop*, int, int)>;

  static std::unique_ptr<FileEvent> Create(Callback read_callback,
                                           Callback write_callback, int mask) {
    const bool has_separate_callbacks =
        read_callback != nullptr && write_callback != nullptr;
    return Create(std::move(read_callback), std::move(write_callback), mask,
                  has_separate_callbacks);
  }

  template <typename T>
  using TypedCallback = CallbackStatus (*)(Loop* event_loop, int fd,
                                           T* client_data, int mask);

  template <typename T>
  static std::unique_ptr<FileEvent> Create(TypedCallback<T> read_callback,
                                           TypedCallback<T> write_callback,
                                           T* client_data, int mask) {
    return Create(Wrap(read_callback, client_data),
                  Wrap(write_callback, client_data), mask,
                  read_callback != write_callback);
  }

  template <typename T>
  static std::unique_ptr<FileEvent> Create(std::nullptr_t read_callback,
                                           TypedCallback<T> write_callback,
                                           T* client_data, int mask) {
    return Create<T>(static_cast<TypedCallback<T>>(read_callback),
                     write_callback, client_data, mask);
  }

  template <typename T>
  static std::unique_ptr<FileEvent> Create(TypedCallback<T> read_callback,
                                           std::nullptr_t write_callback,
                                           T* client_data, int mask) {
    return Create<T>(read_callback,
                     static_cast<TypedCallback<T>>(write_callback), client_data,
                     mask);
  }

  FileEvent(int mask) : mask_(mask) {}
  int Mask() const { return mask_; }
  void SetMask(int mask) { mask_ = mask; }
  void AddMask(int mask) { mask_ |= mask; }
  CallbackStatus CallReadCallback(Loop* loop, int fd, int mask) const {
    return read_callback_(loop, fd, mask);
  }
  CallbackStatus CallWriteCallback(Loop* loop, int fd, int mask) const {
    return write_callback_(loop, fd, mask);
  }
  bool HasReadCallback() const { return static_cast<bool>(read_callback_); }
  bool HasWriteCallback() const { return static_cast<bool>(write_callback_); }
  bool HasSeparateCallbacks() const { return has_separate_callbacks_; }
  void Merge(const FileEvent* file_event);

 private:
  static std::unique_ptr<FileEvent> Create(Callback read_callback,
                                           Callback write_callback, int mask,
                                           bool has_separate_callbacks) {
    return std::unique_ptr<FileEvent>(
        new FileEvent(std::move(read_callback), std::move(write_callback), mask,
                      has_separate_callbacks));
  }

  FileEvent(Callback read_callback, Callback write_callback, int mask,
            bool has_separate_callbacks)
      : mask_(mask),
        read_callback_(std::move(read_callback)),
        write_callback_(std::move(write_callback)),
        has_separate_callbacks_(has_separate_callbacks) {}

  template <typename T>
  static Callback Wrap(TypedCallback<T> callback, T* client_data) {
    if (callback == nullptr) {
      return nullptr;
    }
    return [callback, client_data](Loop* event_loop, int fd, int mask) {
      return callback(event_loop, fd, client_data, mask);
    };
  }

  int mask_;
  Callback read_callback_;
  Callback write_callback_;
  bool has_separate_callbacks_;
};
}  // namespace redis_simple::event_loop
