# Fuzzing

Build the opt-in Clang libFuzzer targets and run their bounded CTest smoke
checks with:

```sh
cmake --preset fuzz
cmake --build --preset fuzz --target redis_simple_fuzzers
ctest --preset fuzz -L fuzz --output-on-failure
```

The fuzz targets exercise incremental RESP parsing; listpack, quicklist, Dict,
Skiplist, and Intset mutation; Redis list, set, hash, and sorted-set behavior;
dynamic and reply buffers; database expiration; and deterministic event-loop
callbacks. AOF replay also has a bounded malformed-input target. Every target
runs under AddressSanitizer and
UndefinedBehaviorSanitizer. On macOS, put Homebrew LLVM on `PATH` before
configuring because Apple Clang does not ship a libFuzzer runtime:

```sh
brew install llvm
PATH="$(brew --prefix llvm)/bin:${PATH}" cmake --preset fuzz
```
