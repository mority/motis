#pragma once

#include <chrono>
#include <vector>

#include "geo/latlng.h"

#include "nigiri/routing/start_times.h"

#include "motis-api/motis-api.h"

namespace motis::ep {
struct routing;
}  // namespace motis::ep

namespace motis::odm {

namespace n = nigiri;

enum fixed { kArr, kDep };

struct direct_ride {
  n::unixtime_t dep_;
  n::unixtime_t arr_;
};

struct capacities {
  std::uint8_t wheelchairs_;
  std::uint8_t bikes_;
  std::uint8_t passengers_;
  std::uint8_t luggage_;
};

struct prima_state {
  geo::latlng from_;
  geo::latlng to_;
  std::vector<n::routing::start> from_rides_;
  std::vector<n::routing::start> to_rides_;
  std::vector<direct_ride> direct_rides_;
  fixed fixed_;
  capacities cap_;

  std::vector<n::routing::start> prev_from_rides_;
  std::vector<n::routing::start> prev_to_rides_;
  std::vector<direct_ride> prev_direct_rides_;
};

}  // namespace motis::odm