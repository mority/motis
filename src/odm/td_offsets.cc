#include "motis/odm/td_offsets.h"

#include <ranges>

#include "motis/td_offsets.h"

using namespace std::chrono_literals;
namespace n = nigiri;
namespace nr = nigiri::routing;

namespace motis::odm {

std::pair<nr::td_offsets_t, nr::td_offsets_t> get_td_offsets_split(
    std::vector<nr::offset> const& offsets,
    std::vector<service_times_t> const& times,
    transport_mode_t const mode) {
  auto const split =
      offsets.empty()
          ? 0
          : std::distance(begin(offsets),
                          std::upper_bound(begin(offsets), end(offsets),
                                           offsets[offsets.size() / 2],
                                           [](auto const& a, auto const& b) {
                                             return a.duration_ < b.duration_;
                                           }));

  auto const offsets_lo = offsets | std::views::take(split);
  auto const times_lo = times | std::views::take(split);
  auto const offsets_hi = offsets | std::views::drop(split);
  auto const times_hi = times | std::views::drop(split);

  auto const derive_td_offsets = [&](auto const& offsets_split,
                                     auto const& times_split) {
    auto td_offsets = nr::td_offsets_t{};
    for (auto const [o, t] : std::views::zip(offsets_split, times_split)) {
      auto& tdos = td_offsets[o.target_];
      // prima merges the service times, so they arrive sorted, as
      // add_td_window requires
      for (auto const& i : t) {
        // the whole ride has to fit into the service time (inclusive end)
        auto const departures =
            n::interval<n::unixtime_t>{i.from_, i.to_ - o.duration_ + 1min};
        if (departures.from_ < departures.to_) {
          add_td_window(tdos, departures, o.duration_, mode);
        }
      }
      normalize_td_offsets(tdos);
    }
    return td_offsets;
  };

  return std::pair{derive_td_offsets(offsets_lo, times_lo),
                   derive_td_offsets(offsets_hi, times_hi)};
}

}  // namespace motis::odm