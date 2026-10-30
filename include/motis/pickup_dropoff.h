#pragma once

#include "osr/lookup.h"
#include "osr/platforms.h"
#include "osr/types.h"
#include "osr/ways.h"

#include "nigiri/timetable.h"
#include "nigiri/types.h"

#include "motis/types.h"

namespace motis {

struct pickup_dropoff_point {
  friend bool operator==(pickup_dropoff_point const&,
                         pickup_dropoff_point const&) = default;

  osr::node_idx_t node_;
  osr::duration_t walk_;
};

// Per timetable location: car-accessible nodes where a ride can pick up / drop
// off, with the walk from the location (pickup) or to it (drop-off).
struct pickup_dropoff_points {
  nigiri::vecvec<nigiri::location_idx_t, pickup_dropoff_point> pickup_,
      drop_off_;
};

pickup_dropoff_points get_pickup_dropoff_points(
    nigiri::timetable const&,
    osr::ways const&,
    osr::lookup const&,
    osr::platforms const&,
    vector_map<nigiri::location_idx_t, osr::platform_idx_t> const&,
    bool all_locations,
    double max_matching_distance);

}  // namespace motis
