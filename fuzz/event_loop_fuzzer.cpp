#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "event_loop/file_event.h"
#include "event_loop/loop.h"
#include "event_loop/time_event.h"
#include "fuzz/fuzz_input.h"

namespace redis_simple::fuzz {
namespace {
struct CallbackCounts {
  size_t deferred{};
  size_t timer{};
  size_t finalized{};
};

void VerifyFileEventMerge(FuzzInput* input) {
  size_t reads = 0;
  size_t writes = 0;
  auto read_event = event_loop::FileEvent::Create(
      [&reads](event_loop::Loop*, int, int) {
        ++reads;
        return event_loop::CallbackStatus::kOk;
      },
      nullptr, event_loop::ToInt(event_loop::EventFlag::kReadable));
  const int write_mask =
      event_loop::ToInt(event_loop::EventFlag::kWritable) |
      ((input->ReadByte() & 1U) != 0
           ? event_loop::ToInt(event_loop::EventFlag::kBarrier)
           : 0);
  auto write_event = event_loop::FileEvent::Create(
      nullptr,
      [&writes](event_loop::Loop*, int, int) {
        ++writes;
        return event_loop::CallbackStatus::kOk;
      },
      write_mask);

  read_event->Merge(write_event.get());
  Require(read_event->HasReadCallback());
  Require(read_event->HasWriteCallback());
  Require(read_event->HasSeparateCallbacks());
  Require((read_event->Mask() & event_loop::EventFlag::kReadable) != 0);
  Require((read_event->Mask() & event_loop::EventFlag::kWritable) != 0);
  Require(read_event->CallReadCallback(nullptr, 0, read_event->Mask()) ==
          event_loop::CallbackStatus::kOk);
  Require(read_event->CallWriteCallback(nullptr, 0, read_event->Mask()) ==
          event_loop::CallbackStatus::kOk);
  Require(reads == 1 && writes == 1);
}

void AddDeferredCallbacks(event_loop::Loop* loop, FuzzInput* input,
                          CallbackCounts* counts, size_t* expected_after_first,
                          size_t* expected_after_second) {
  const size_t callback_count = input->ReadIndex(16);
  *expected_after_first = callback_count + 1;
  *expected_after_second = *expected_after_first + 1;
  for (size_t i = 0; i < callback_count; ++i) {
    const bool nest_callback = (input->ReadByte() & 1U) != 0;
    loop->Defer([loop, counts, nest_callback] {
      ++counts->deferred;
      if (nest_callback) {
        loop->Defer([counts] { ++counts->deferred; });
      }
    });
    if (nest_callback) {
      ++*expected_after_second;
    }
  }
  // These callbacks guarantee that both event-loop polls are non-blocking.
  loop->Defer([loop, counts] {
    ++counts->deferred;
    loop->Defer([counts] { ++counts->deferred; });
  });
}

size_t AddTimeEvents(event_loop::Loop* loop, FuzzInput* input,
                     CallbackCounts* counts,
                     size_t* expected_deferred_after_second) {
  const size_t event_count = input->ReadIndex(16);
  for (size_t i = 0; i < event_count; ++i) {
    const bool defer_from_timer = (input->ReadByte() & 1U) != 0;
    auto event = event_loop::TimeEvent::Create(
        [loop, counts, defer_from_timer](int64_t) {
          ++counts->timer;
          if (defer_from_timer) {
            loop->Defer([counts] { ++counts->deferred; });
          }
          return -1;
        },
        [counts] {
          ++counts->finalized;
          return 0;
        });
    Require(event != nullptr);
    event->SetWhen(0);
    loop->CreateTimeEvent(std::move(event));
    if (defer_from_timer) {
      ++*expected_deferred_after_second;
    }
  }
  return event_count;
}

void RunOperations(FuzzInput* input) {
  VerifyFileEventMerge(input);

  auto loop = event_loop::Loop::Create();
  Require(loop != nullptr);
  CallbackCounts counts;
  size_t deferred_after_first = 0;
  size_t deferred_after_second = 0;
  AddDeferredCallbacks(loop.get(), input, &counts, &deferred_after_first,
                       &deferred_after_second);
  const size_t timer_count =
      AddTimeEvents(loop.get(), input, &counts, &deferred_after_second);

  loop->ProcessEvents();
  Require(counts.deferred == deferred_after_first);
  Require(counts.timer == timer_count);
  Require(counts.finalized == 0);

  loop->ProcessEvents();
  Require(counts.deferred == deferred_after_second);
  Require(counts.timer == timer_count);
  Require(counts.finalized == timer_count);
}
}  // namespace
}  // namespace redis_simple::fuzz

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  redis_simple::fuzz::FuzzInput input(data, size);
  redis_simple::fuzz::RunOperations(&input);
  return 0;
}
