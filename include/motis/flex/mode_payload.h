#pragma once

#include "osr/types.h"

#include "nigiri/routing/query.h"
#include "nigiri/types.h"

namespace motis::flex {

// One flex offer: transport `t`, boarding at stop `from_stop` and alighting at
// stop `to_stop` of its stop sequence (both in travel order, so from < to), as
// seen from the search direction `dir` (forward: the query position is at
// from_stop, backward: at to_stop).
struct mode_payload {
  static constexpr auto kTransportBits = 21U;
  static constexpr auto kStopBits = 5U;

  // Whether transport and stop indices fit into the payload bits.
  static bool fits(nigiri::flex_transport_idx_t const t,
                   nigiri::stop_idx_t const from_stop,
                   nigiri::stop_idx_t const to_stop) {
    return to_idx(t) < (1U << kTransportBits) &&
           from_stop < (1U << kStopBits) && to_stop < (1U << kStopBits);
  }

  mode_payload(nigiri::flex_transport_idx_t const t,
               nigiri::stop_idx_t const from_stop,
               nigiri::stop_idx_t const to_stop,
               osr::direction const dir)
      : transport_{t},
        dir_{dir != osr::direction::kForward},
        from_stop_{from_stop},
        to_stop_{to_stop} {}

  explicit mode_payload(nigiri::routing::transport_mode_t::payload_t const x) {
    std::memcpy(this, &x, sizeof(mode_payload));
  }

  osr::direction get_dir() const {
    return dir_ == 0 ? osr::direction::kForward : osr::direction::kBackward;
  }

  // boarding stop index (travel order)
  nigiri::stop_idx_t get_from_stop() const {
    return static_cast<nigiri::stop_idx_t>(from_stop_);
  }

  // alighting stop index (travel order)
  nigiri::stop_idx_t get_to_stop() const {
    return static_cast<nigiri::stop_idx_t>(to_stop_);
  }

  nigiri::flex_transport_idx_t get_flex_transport() const {
    return nigiri::flex_transport_idx_t{transport_};
  }

  nigiri::routing::transport_mode_t::payload_t to_payload() const {
    static_assert(sizeof(mode_payload) ==
                  sizeof(nigiri::routing::transport_mode_t::payload_t));
    auto id = nigiri::routing::transport_mode_t::payload_t{};
    std::memcpy(&id, this, sizeof(id));
    return id;
  }

  nigiri::flex_transport_idx_t::value_t transport_ : kTransportBits;
  nigiri::flex_transport_idx_t::value_t dir_ : 1;
  nigiri::flex_transport_idx_t::value_t from_stop_ : kStopBits;
  nigiri::flex_transport_idx_t::value_t to_stop_ : kStopBits;
};

}  // namespace motis::flex