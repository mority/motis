#pragma once

#include <chrono>
#include <vector>

#include "geo/latlng.h"

#include "nigiri/routing/journey.h"
#include "nigiri/routing/start_times.h"

#include "motis-api/motis-api.h"

#include "motis/fwd.h"
#include "motis/place.h"

namespace motis::ep {
struct routing;
}  // namespace motis::ep

namespace motis::odm {

using service_times_t = std::vector<nigiri::interval<nigiri::unixtime_t>>;

struct taxi_offset {
  nigiri::routing::offset o_;
  service_times_t t_;
};

struct direct_ride {
  nigiri::unixtime_t dep_;
  nigiri::unixtime_t arr_;
};

struct capacities {
  std::int64_t wheelchairs_;
  std::int64_t bikes_;
  std::int64_t passengers_;
  std::int64_t luggage_;
};

struct prima {

  prima(std::string const& prima_url,
        osr::location const& from,
        osr::location const& to,
        api::plan_params const& query);

  void init(nigiri::interval<nigiri::unixtime_t> const& search_intvl,
            nigiri::interval<nigiri::unixtime_t> const& taxi_intvl,
            bool use_first_mile_taxi,
            bool use_last_mile_taxi,
            bool use_direct_taxi,
            bool use_first_mile_ride_sharing,
            bool use_last_mile_ride_sharing,
            bool use_direct_ride_sharing,
            nigiri::timetable const& tt,
            nigiri::rt_timetable const* rtt,
            ep::routing const& r,
            elevators const* e,
            gbfs::gbfs_routing_data& gbfs,
            api::Place const& from,
            api::Place const& to,
            api::plan_params const& query,
            nigiri::routing::query const& n_query,
            unsigned api_version);

  std::size_t n_taxi_events() const;
  std::size_t n_ride_sharing_events() const;

  std::string make_blacklist_taxi_request(
      nigiri::timetable const&,
      nigiri::interval<nigiri::unixtime_t> const&) const;
  bool consume_blacklist_taxi_response(std::string_view json);
  bool blacklist_taxi(nigiri::timetable const&,
                       nigiri::interval<nigiri::unixtime_t> const&);

  std::string make_whitelist_taxi_request(std::vector<nigiri::routing::start> const& first_mile,
    std::vector<nigiri::routing::start> const& last_mile, nigiri::timetable const&) const;
  bool consume_whitelist_taxi_response(
      std::string_view json,
      std::vector<nigiri::routing::journey>&,
      std::vector<nigiri::routing::start>& first_mile_taxi_rides,
      std::vector<nigiri::routing::start>& last_mile_taxi_rides);
  bool whitelist_taxi(std::vector<nigiri::routing::journey>&,
                       nigiri::timetable const&);

  void add_direct_odm(std::vector<direct_ride> const&,
                      std::vector<nigiri::routing::journey>&,
                      place_t const& from,
                      place_t const& to,
                      bool arrive_by,
                      nigiri::transport_mode_id_t) const;

  std::string make_ride_sharing_request(nigiri::timetable const&) const;
  bool consume_ride_sharing_response(std::string_view json);
  bool whitelist_ride_sharing(nigiri::timetable const&);

  boost::urls::url taxi_blacklist_;
  boost::urls::url taxi_whitelist_;
  boost::urls::url ride_sharing_whitelist_;

  osr::location const from_;
  osr::location const to_;
  nigiri::event_type fixed_;
  capacities cap_;

  std::optional<std::chrono::seconds> direct_duration_;

  std::vector<nigiri::routing::offset> first_mile_taxi_{};
  std::vector<nigiri::routing::offset> last_mile_taxi_{};
  std::vector<service_times_t> first_mile_taxi_times_{};
  std::vector<service_times_t> last_mile_taxi_times_{};
  std::vector<direct_ride> direct_taxi_{};

  std::vector<nigiri::routing::start> first_mile_ride_sharing_{};
  std::vector<std::uint32_t> first_mile_ride_sharing_tour_ids_{};
  std::vector<nigiri::routing::start> last_mile_ride_sharing_{};
  std::vector<std::uint32_t> last_mile_ride_sharing_tour_ids_{};
  std::vector<direct_ride> direct_ride_sharing_{};
  std::vector<std::uint32_t> direct_ride_sharing_tour_ids_{};
};

void extract_taxis(std::vector<nigiri::routing::journey> const&,
                   std::vector<nigiri::routing::start>& first_mile_taxi_rides,
                   std::vector<nigiri::routing::start>& last_mile_taxi_rides);

void fix_first_mile_duration(
    std::vector<nigiri::routing::journey>& journeys,
    std::vector<nigiri::routing::start> const& first_mile,
    std::vector<nigiri::routing::start> const& prev_first_mile,
    nigiri::transport_mode_id_t mode);
void fix_last_mile_duration(
    std::vector<nigiri::routing::journey>& journeys,
    std::vector<nigiri::routing::start> const& last_mile,
    std::vector<nigiri::routing::start> const& prev_last_mile,
    nigiri::transport_mode_id_t mode);

nigiri::unixtime_t to_unix(std::int64_t);

}  // namespace motis::odm