#include <cstdlib>
#include <iostream>

#include "server/server.h"
#include "server/server_options.h"

#ifdef REDIS_SIMPLE_USE_MIMALLOC
// Replaces the global operator new and delete, which every container and
// object allocation in the server goes through, with mimalloc. Include it
// in exactly one translation unit.
#include <mimalloc-new-delete.h>
#include <mimalloc.h>
#endif

namespace {
#ifdef REDIS_SIMPLE_USE_MIMALLOC
// Collects the calling thread's heap, returning its free pages to the
// system. mimalloc keeps freed pages for reuse for its purge delay, and
// returns them only while the thread keeps allocating; the server calls this
// once a thread has been idle for a second (IdleMemoryReleaser).
void ReleaseFreeMemory() { mi_collect(/*force=*/true); }
constexpr redis_simple::IdleMemoryReleaser::ReleaseFunction kReleaseMemory =
    &ReleaseFreeMemory;
#else
constexpr redis_simple::IdleMemoryReleaser::ReleaseFunction kReleaseMemory =
    nullptr;
#endif
}  // namespace

int main(int argc, char* argv[]) {
  const redis_simple::OptionsResult result =
      redis_simple::ParseServerOptions(argc, argv);
  if (result.status == redis_simple::OptionsStatus::kHelp) {
    std::cout << redis_simple::ServerUsage();
    return EXIT_SUCCESS;
  }
  if (result.status == redis_simple::OptionsStatus::kError) {
    std::cerr << "redis_simple: " << result.error << '\n'
              << redis_simple::ServerUsage();
    return EXIT_FAILURE;
  }
  const auto server = redis_simple::Server::Create(kReleaseMemory);
  return server->Run(result.options) ? EXIT_SUCCESS : EXIT_FAILURE;
}
