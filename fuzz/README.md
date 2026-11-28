# Fuzzing

Build the opt-in Clang libFuzzer targets and run their bounded CTest smoke
checks with:

```sh
cmake --preset fuzz
cmake --build --preset fuzz --target redis_simple_fuzzers
ctest --preset fuzz -L fuzz --output-on-failure
```

Every fuzzer runs under AddressSanitizer and UndefinedBehaviorSanitizer. On
macOS, put Homebrew LLVM on `PATH` before
configuring because Apple Clang does not ship a libFuzzer runtime:

```sh
brew install llvm
PATH="$(brew --prefix llvm)/bin:${PATH}" cmake --preset fuzz
```

## Names

Each fuzzer is one file, `fuzz/<name>_fuzzer.cpp`, named after the class or
module it drives. It builds the executable `<name>_fuzzer` and registers the
CTest smoke check `redis_simple_fuzz_<name>`. `redis_simple_fuzzers` builds
them all. `redis_simple_core_instrumented` is not a fuzzer: it is the server's
core sources compiled with libFuzzer's coverage instrumentation, which every
fuzzer links.

Fuzzers for a command-level data type and for the container beneath it have
similar names, so the table gives the layer.

| Fuzzer | Layer | What it drives |
| --- | --- | --- |
| `request_parser_fuzzer` | server | RESP and inline request parsing of raw bytes, whole and in pieces |
| `db_fuzzer` | server | The keyspace: set, delete, unlink, rename, persist, expire, and flush, with TTLs |
| `aof_fuzzer` | server | Manifest parsing, loading an AOF of raw bytes, then append and rewrite |
| `rdb_fuzzer` | server | Loading raw bytes, a write and load round trip, and corrupted bytes against the checksum |
| `list_fuzzer` | data type | `List`, including conversion between listpack and quicklist |
| `set_fuzzer` | data type | `Set` across its encodings |
| `hash_fuzzer` | data type | `Hash`, including scanning |
| `zset_fuzzer` | data type | `ZSet` scores, ranks, and ranges |
| `listpack_fuzzer` | container | `Listpack` edits against a vector, and widening the short backlens Redis 8.8 to 8.10 wrote |
| `quicklist_fuzzer` | container | `Quicklist` with a fuzzed node size against a vector |
| `dict_fuzzer` | container | `Dict` scan, stepwise clear, resize policy, and rehash |
| `skiplist_fuzzer` | container | `Skiplist` ranks, ranges, and updates |
| `intset_fuzzer` | container | `Intset` against `std::set`, starting from a blob in Redis' layout that may be invalid, and a blob round trip after each operation |
| `handle_set_fuzzer` | container | `HandleSet`, the table behind the TTL index, under heavy hash collisions |
| `dynamic_buffer_fuzzer` | container | `DynamicBuffer` append, consume, compact, and read line |
| `reply_buffer_fuzzer` | container | `ReplyBuffer` append, consume, and block filling |
| `event_loop_fuzzer` | event loop | File event merging, deferred callbacks, and time events |

`fuzz_input.h` reads values out of the fuzz input, and `sequence_model.h` holds
the vector models that the list, listpack, and quicklist fuzzers compare
against.
