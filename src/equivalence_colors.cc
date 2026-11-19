#include "motis/equivalence_colors.h"

#include <numeric>

#include "utl/enumerate.h"

#include "geo/latlng.h"

#include "nigiri/for_each_meta.h"
#include "nigiri/timetable.h"

#include "motis/get_stops_with_traffic.h"
#include "motis/types.h"

namespace n = nigiri;

namespace motis {

namespace {

template <typename Fn>
void for_each_equivalent(n::timetable const& tt,
                         n::location_idx_t const l,
                         Fn&& fn) {
  auto const pos = tt.locations_.coordinates_[l];
  n::routing::for_each_meta(
      tt, nullptr, n::routing::location_match_mode::kEquivalent, l,
      [&](n::location_idx_t const x) {
        if (x != l && to_idx(x) < tt.n_locations() &&
            geo::distance(pos, tt.locations_.coordinates_[x]) <=
                kMaxEquivalentDistance) {
          fn(x);
        }
      });
}

}  // namespace

std::vector<n::location_idx_t> add_equivalents(
    n::timetable const& tt,
    n::rt_timetable const* rtt,
    std::vector<n::location_idx_t> stops) {
  auto seen = hash_set<n::location_idx_t>{begin(stops), end(stops)};
  auto const n_stops = stops.size();
  for (auto i = 0U; i != n_stops; ++i) {
    for_each_equivalent(tt, stops[i], [&](n::location_idx_t const x) {
      if (has_traffic(tt, rtt, x) && seen.emplace(x).second) {
        stops.emplace_back(x);
      }
    });
  }
  return stops;
}

std::vector<osr::color_idx_t> equivalence_colors(
    n::timetable const& tt, std::vector<n::location_idx_t> const& stops) {
  auto idx = hash_map<n::location_idx_t, std::uint32_t>{};
  for (auto const [i, l] : utl::enumerate(stops)) {
    idx.emplace(l, static_cast<std::uint32_t>(i));
  }

  auto root = std::vector<std::uint32_t>(stops.size());
  std::iota(begin(root), end(root), 0U);
  auto const find = [&](std::uint32_t x) {
    while (root[x] != x) {
      x = root[x] = root[root[x]];
    }
    return x;
  };

  for (auto const [i, l] : utl::enumerate(stops)) {
    for_each_equivalent(tt, l, [&](n::location_idx_t const x) {
      if (auto const it = idx.find(x); it != end(idx)) {
        auto const a = find(static_cast<std::uint32_t>(i));
        auto const b = find(it->second);
        root[std::max(a, b)] = std::min(a, b);
      }
    });
  }

  auto colors = std::vector<osr::color_idx_t>(stops.size());
  for (auto i = 0U; i != stops.size(); ++i) {
    colors[i] = osr::color_idx_t{find(i)};
  }
  return colors;
}

}  // namespace motis
