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

int main(int argc, char* argv[]) {
#ifdef REDIS_SIMPLE_USE_MIMALLOC
  // Return freed memory to the OS as soon as it is freed, as Redis'
  // allocator does. With mimalloc's default delay, freed pages are only
  // returned during later allocations on the same thread, so a server that
  // deletes a large value and then idles kept the memory indefinitely. The
  // environment variable still overrides this.
  if (std::getenv("MIMALLOC_PURGE_DELAY") == nullptr) {
    mi_option_set(mi_option_purge_delay, 0);
  }
#endif
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
  const auto server = redis_simple::Server::Create();
  return server->Run(result.options) ? EXIT_SUCCESS : EXIT_FAILURE;
}
