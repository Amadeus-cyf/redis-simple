#include "server/idle_client_sweeper.h"

#include <algorithm>
#include <cstddef>

#include "server/client.h"

namespace redis_simple {
void IdleClientSweeper::Add(Client* const client) {
  client->SetSweepIndex(clients_.size());
  clients_.push_back(client);
}

void IdleClientSweeper::Remove(Client* const client) {
  const size_t index = client->SweepIndex();
  if (index == Client::kNotSwept) {
    return;
  }
  // Fill the gap with the last client instead of shifting the rest.
  clients_[index] = clients_.back();
  clients_[index]->SetSweepIndex(index);
  clients_.pop_back();
  client->SetSweepIndex(Client::kNotSwept);
}

void IdleClientSweeper::Clear() {
  clients_.clear();
  next_ = 0;
  sweeps_left_ = 0;
}

bool IdleClientSweeper::Sweep() {
  if (sweeps_left_ == 0) {
    next_ = 0;
    sweeps_left_ = kSweepsPerRound;
  }
  // Removals may have left fewer clients than this round has visited.
  const size_t unvisited = clients_.size() - std::min(next_, clients_.size());
  // Rounded up, so the round's last call reaches the last client.
  const size_t visits = (unvisited + sweeps_left_ - 1) / sweeps_left_;
  --sweeps_left_;
  bool released = false;
  for (size_t visit = 0; visit < visits; ++visit) {
    Client* const client = clients_[next_++];
    if (!client->HandedToMain() && client->ReleaseIdleBuffers()) {
      released = true;
    }
  }
  return released;
}
}  // namespace redis_simple
