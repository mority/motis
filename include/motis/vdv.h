#pragma once

#include "motis/config.h"
#include "net/web_server/query_router.h"

namespace motis::vdv {

template <typename Executor>
void setup_client_endpoints(net::query_router<Executor>& qr, config const& c) {
  if (!c.vdv_.has_value()) {
    return;
  }

  for (auto const& [name, addr] : c.vdv_->aus_servers_) {
  }
}

}  // namespace motis::vdv