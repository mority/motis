#pragma once

#include <vector>

#include "nigiri/routing/query.h"

namespace motis {

// Input: for each mode, a step function. An entry sets the duration of its
// mode from its valid_from on, until the next entry of the same mode
// overwrites it; kMaxDuration marks the mode as unavailable.
// Precondition: entries of the same mode have distinct valid_from.
void normalize_td_offsets(std::vector<nigiri::routing::td_offset>&);

// Appends the window [from, to) of `mode` as the entries {from, duration} and
// {to, kMaxDuration}. A window that touches or overlaps the previous window of
// the same mode extends that window instead, so the entries of each mode stay a
// step function for normalize_td_offsets.
// Precondition: the windows of a mode are added consecutively, in ascending
// order of `from`, and with the same duration.
void add_td_window(std::vector<nigiri::routing::td_offset>&,
                   nigiri::unixtime_t from,
                   nigiri::unixtime_t to,
                   nigiri::duration_t,
                   nigiri::routing::transport_mode_t);

}  // namespace motis
