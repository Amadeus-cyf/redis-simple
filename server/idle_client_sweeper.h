#ifndef REDIS_SIMPLE_SERVER_IDLE_CLIENT_SWEEPER_H_
#define REDIS_SIMPLE_SERVER_IDLE_CLIENT_SWEEPER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace redis_simple {
class Client;

// Releases the buffers of idle clients, a few clients at a time, as Redis'
// clientsCron does. Each side that owns clients has its own sweeper and
// calls it from its own thread: the main thread without threaded I/O,
// otherwise each I/O thread for the clients assigned to it.
class IdleClientSweeper {
 public:
  // How often the owner calls Sweep().
  static constexpr int64_t kIntervalMs = 100;

  // `client` must stay valid until Remove(client).
  void Add(Client* client);
  // Does nothing for a client that was not added.
  void Remove(Client* client);
  void Clear();
  size_t ClientCount() const { return clients_.size(); }
  // Visits a share of the clients, so that a round of kSweepsPerRound calls
  // visits each client once and one call's work stays bounded. A client
  // that read nothing since its previous visit, a second earlier, releases
  // its buffers (Client::ReleaseIdleBuffers). Clients handed to the main
  // thread are skipped: their buffers are not this side's to touch.
  void Sweep();
  static constexpr size_t kSweepsPerRound = 1000 / kIntervalMs;

 private:
  std::vector<Client*> clients_;
  // The next client of this round, and the calls left in it.
  size_t next_{};
  size_t sweeps_left_{};
};
}  // namespace redis_simple

#endif  // REDIS_SIMPLE_SERVER_IDLE_CLIENT_SWEEPER_H_
