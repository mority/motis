#pragma once

#include <vector>

#include "osr/types.h"

#include "nigiri/types.h"

#include "motis/fwd.h"

namespace motis {

// Links between stops longer than this don't make them the same place.
// Same limit as nigiri's same-name equivalences.
constexpr auto const kMaxEquivalentDistance = 500.0;  // [m]

// `stops` followed by their equivalents with traffic that aren't in `stops`.
std::vector<nigiri::location_idx_t> add_equivalents(
    nigiri::timetable const&,
    nigiri::rt_timetable const*,
    std::vector<nigiri::location_idx_t> stops);

// One color per stop: the connected components of the equivalence links
// between `stops`.
std::vector<osr::color_idx_t> equivalence_colors(
    nigiri::timetable const&, std::vector<nigiri::location_idx_t> const& stops);

}  // namespace motis
