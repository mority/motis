#pragma once

#include <chrono>
#include <vector>

#include "geo/latlng.h"

#include "nigiri/routing/journey.h"
#include "nigiri/routing/start_times.h"

#include "motis-api/motis-api.h"

namespace motis::ep {
struct routing;
}  // namespace motis::ep

namespace motis::odm {

nigiri::unixtime_t to_unix(std::chrono::milliseconds);
std::chrono::milliseconds to_millis(nigiri::unixtime_t);

struct ride {
  friend bool operator==(ride const& a, ride const& b) = default;
  friend bool operator<(ride const& a, ride const& b) {
    return std::tie(a.stop_, a.dep_, a.arr_) <
           std::tie(b.stop_, b.dep_, b.arr_);
  }

  nigiri::unixtime_t unix_dep() const { return to_unix(dep_); }

  nigiri::unixtime_t unix_arr() const { return to_unix(arr_); }

  nigiri::duration_t duration() const { return to_unix(arr_) - to_unix(dep_); }

  std::chrono::milliseconds dep_;
  std::chrono::milliseconds arr_;
  nigiri::location_idx_t stop_{nigiri::location_idx_t::invalid()};
};

struct capacities {
  std::uint8_t wheelchairs_;
  std::uint8_t bikes_;
  std::uint8_t passengers_;
  std::uint8_t luggage_;
};

struct prima {
  void init(api::Place const& from,
            api::Place const& to,
            api::plan_params const& query);
  std::string get_prima_request(nigiri::timetable const&) const;
  std::size_t n_events() const;
  bool blacklist_update(std::string_view json);
  bool whitelist_update(std::string_view json);
  void adjust_to_whitelisting();

  geo::latlng from_;
  geo::latlng to_;
  nigiri::event_type fixed_;
  capacities cap_;

  std::vector<ride> from_rides_{};
  std::vector<ride> to_rides_{};
  std::vector<ride> direct_rides_{};

  std::vector<ride> prev_from_rides_{};
  std::vector<ride> prev_to_rides_{};
  std::vector<ride> prev_direct_rides_{};

  std::vector<nigiri::routing::journey> odm_journeys_{};
};

}  // namespace motis::odm