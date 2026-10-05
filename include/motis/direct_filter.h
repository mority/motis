#pragma once

#include <utility>
#include <vector>

#include "nigiri/routing/journey.h"
#include "nigiri/routing/query.h"

#include "motis-api/motis-api.h"

namespace motis {

void direct_filter(std::vector<api::Itinerary> const& direct,
                   std::vector<nigiri::routing::journey>&);

// Direct connection duration for every transport mode used by the query's
// start and destination offsets, so that the routing core applies the same
// filter as direct_filter before counting journeys towards
// min_connection_count_.
std::vector<std::pair<nigiri::routing::transport_mode_t, nigiri::duration_t>>
get_direct_durations(std::vector<api::Itinerary> const& direct,
                     nigiri::routing::query const&);

}  // namespace motis
