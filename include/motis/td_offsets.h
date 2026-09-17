#pragma once

#include <vector>

#include "nigiri/common/interval.h"
#include "nigiri/routing/query.h"

namespace motis {

void normalize_td_offsets(std::vector<nigiri::routing::td_offset>&);

// Appends the window [from, to) of `mode` as the entries {from, duration} and
// {to, kMaxDuration}. A window that touches or overlaps the previous window of
// the same mode extends that window instead, so the entries of each mode stay
// a step function for normalize_td_offsets.
// Nothing is checked: the caller adds non-empty windows, sorted by mode and by
// `from`, and gives overlapping windows of one mode the same duration.
void add_td_window(std::vector<nigiri::routing::td_offset>&,
                   nigiri::interval<nigiri::unixtime_t> window,
                   nigiri::duration_t,
                   nigiri::routing::transport_mode_t);

}  // namespace motis
