#include "motis/direct_filter.h"

#include "utl/erase_if.h"
#include "utl/visit.h"

#include "nigiri/types.h"

#include "motis/journey_to_response.h"

namespace motis {

using namespace std::chrono_literals;
namespace n = nigiri;

namespace {

n::duration_t get_direct_duration(std::vector<api::Itinerary> const& direct,
                                  transport_mode_t const mode) {
  auto const m = to_mode(mode);
  auto const i = utl::find_if(direct, [&](auto const& d) {
    auto const leg_with_actual_mode =
        d.legs_.size() > 1 && d.legs_.front().mode_ == api::ModeEnum::WALK
            ? 1U
            : 0U;
    return d.legs_.at(leg_with_actual_mode).mode_ == m;
  });
  return i != end(direct)
             ? n::duration_t{std::chrono::round<std::chrono::minutes>(
                   std::chrono::seconds{i->duration_})}
             : n::duration_t::max();
}

}  // namespace

void direct_filter(std::vector<api::Itinerary> const& direct,
                   std::vector<n::routing::journey>& journeys) {
  auto const not_better_than_direct = [&](n::routing::journey const& j) {
    auto const first_leg_offset = utl::visit(
        j.legs_.front().uses_,
        [&](n::routing::offset const& o) { return std::optional{o}; });

    auto const last_leg_offset = utl::visit(
        j.legs_.back().uses_,
        [&](n::routing::offset const& o) { return std::optional{o}; });

    auto const longer_than_direct = [&](n::routing::offset const& o) {
      return std::optional{o.duration_ >=
                           get_direct_duration(direct, o.mode())};
    };

    return first_leg_offset.and_then(longer_than_direct).value_or(false) ||
           last_leg_offset.and_then(longer_than_direct).value_or(false) ||
           (first_leg_offset && last_leg_offset &&
            first_leg_offset->mode() == last_leg_offset->mode() &&
            first_leg_offset->duration_ + last_leg_offset->duration_ >=
                get_direct_duration(direct, first_leg_offset->mode()));
  };

  utl::erase_if(journeys, not_better_than_direct);
}

std::vector<std::pair<n::routing::transport_mode_t, n::duration_t>>
get_direct_durations(std::vector<api::Itinerary> const& direct,
                     n::routing::query const& q) {
  auto durations =
      std::vector<std::pair<n::routing::transport_mode_t, n::duration_t>>{};
  if (direct.empty()) {
    return durations;
  }
  auto const add = [&](transport_mode_t const m) {
    if (utl::find_if(durations, [&](auto const& x) { return x.first == m; }) ==
        end(durations)) {
      durations.emplace_back(m, get_direct_duration(direct, m));
    }
  };
  for (auto const* offsets : {&q.start_, &q.destination_}) {
    for (auto const& o : *offsets) {
      add(o.mode());
    }
  }
  for (auto const* td_offsets : {&q.td_start_, &q.td_dest_}) {
    for (auto const& [_, v] : *td_offsets) {
      for (auto const& o : v) {
        add(o.mode());
      }
    }
  }
  return durations;
}

}  // namespace motis
