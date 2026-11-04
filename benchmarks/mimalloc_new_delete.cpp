// Built into memory_benchmark only with REDIS_SIMPLE_BENCHMARK_WITH_MIMALLOC,
// so the same benchmarks can run on mimalloc, as the server does, or on the
// system allocator. Like server/main.cpp, it replaces the global operator new
// and delete, and must be the only file in the executable that does.
#include <mimalloc-new-delete.h>
