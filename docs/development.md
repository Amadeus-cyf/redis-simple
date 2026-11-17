# Development

Tooling, style, and CI. `CLAUDE.md` holds the full conventions.

## Tooling

Project management files are checked in:

- `CMakePresets.json` for reproducible configure/build/test commands.
- `.clang-format` for Google-style formatting with clang-format 22.
- `.clang-tidy` for static-analysis defaults, including the `google-*` checks
  and Google naming rules (`kConstant` names for constants and `const`
  statics, `member_` suffixes for private members).
- `.editorconfig` for editor consistency.
- `.github/workflows/ci.yml` for CI on push and pull request.
- `CLAUDE.md` for AI coding-agent project guidance; `AGENTS.md` points to it
  for agents that look for that file.
- `.claude/skills/` for agent skills, such as `pre-pr-check`, which runs the
  checks a change needs before a pull request. `.agents/skills` links to the
  same folder for agents that look there.

The root `CMakeLists.txt` is target-based. Production, test, and benchmark
sources are discovered from scoped project directories with
`GLOB_RECURSE CONFIGURE_DEPENDS`, while test files and executable entry points
are excluded from library targets explicitly. Platform-specific event-loop
pollers are selected at configure time: `kqueue` on macOS and `epoll` on Linux.

## C++ Style

The code follows Google C++ style with project-local conventions: owning factory
functions are named `Create()` and return `std::unique_ptr`, while concise
accessors such as `Type()`, `Encoding()`, and `TotalBytes()` are preferred over
Java-style `Get...` names.

As in Google style, headers use `#define` guards, includes are spelled from the
repository root, and the code does not use exceptions. Recoverable failures are
returned as values (`bool`, `std::optional`, status enums, or `nullptr`), while
programming errors are caught by `RS_CHECK` in `utils/check.h`, which aborts
and stays enabled in release builds. Objects with static storage duration are
trivially destructible or deliberately leaked.

Hot paths prefer `std::string_view` for read-only inputs and visitor-style
traversal when callers can encode or rebuild results directly. Vector-returning
helpers remain useful for convenience APIs and tests, but command handlers
should avoid materializing extra copies when they can stream replies in one
pass.

Command handlers borrow their argument views directly from the client query
buffer. Completed string replies are moved into the output queue, and the queue
reuses scatter/gather metadata across partial writes. `GET` of a value of at
least 16 KiB queues a reference to the stored value instead of a copy; the
reply keeps those bytes alive if the key is overwritten or deleted before the
reply is sent.

## CI

CI runs for pull requests targeting `main` and for pushes to `main`. A new run
for the same pull request or branch cancels any older run still in progress,
avoiding duplicate branch-push runs and work on superseded commits.

GitHub Actions runs debug, release, and ASan/UBSan matrices on macOS and Ubuntu
26.04, so both event-loop pollers are built and tested. macOS also runs Apple
`leaks`; Ubuntu runs LeakSanitizer, ThreadSanitizer, clang-format 22,
clang-tidy 22, and bounded libFuzzer smoke tests. A separate Ubuntu job builds the latest
stable Redis release (`scripts/build_latest_redis.sh`) and compares
supported behavior with it: replies, change counts and `WATCH`, and AOF and
RDB files in both directions. Locally, the first of those checks is:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug -L unit --output-on-failure
ctest --preset debug -L integration --output-on-failure
```
