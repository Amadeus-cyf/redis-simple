#include <cstdlib>
#include <iostream>

#include "server/server.h"
#include "server/server_options.h"

#ifdef REDIS_SIMPLE_USE_MIMALLOC
// Replaces the global operator new and delete, which every container and
// object allocation in the server goes through, with mimalloc. Include it
// in exactly one translation unit.
#include <mimalloc-new-delete.h>
#endif

int main(int argc, char* argv[]) {
  const auto result = redis_simple::ParseServerOptions(argc, argv);
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
