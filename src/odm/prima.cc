#include "motis/odm/prima.h"

#include <variant>

#include "boost/asio/io_context.hpp"
#include "boost/json.hpp"

#include "utl/erase_if.h"
#include "utl/pipes.h"
#include "utl/visit.h"
#include "utl/zip.h"

#include "nigiri/common/parse_time.h"
#include "nigiri/logging.h"
#include "nigiri/timetable.h"

#include "motis/elevators/elevators.h"
#include "motis/endpoints/routing.h"
#include "motis/http_req.h"
#include "motis/odm/bounds.h"
#include "motis/odm/odm.h"
#include "motis/transport_mode_ids.h"

namespace n = nigiri;
namespace nr = nigiri::routing;
namespace json = boost::json;

namespace motis::odm {

prima::prima(std::string const& prima_url,
             osr::location const& from,
             osr::location const& to,
             api::plan_params const& query)
    : query_{query},
      taxi_blacklist_{prima_url + kBlacklistPath},
      ride_sharing_whitelist_{prima_url + kRidesharingPath},
      from_{from},
      to_{to},
      fixed_{query.arriveBy_ ? n::event_type::kArr : n::event_type::kDep},
      cap_{
          .wheelchairs_ = static_cast<std::uint8_t>(
              query.pedestrianProfile_ == api::PedestrianProfileEnum::WHEELCHAIR
                  ? 1U
                  : 0U),
          .bikes_ =
              static_cast<std::uint8_t>(query.requireBikeTransport_ ? 1 : 0),
          .passengers_ = query.passengers_.value_or(1U),
          .luggage_ = query.luggage_.value_or(0U)} {}

void init_pt(std::vector<n::routing::offset>& offsets,
             std::vector<n::routing::start>& rides,
             ep::routing const& r,
             osr::location const& l,
             osr::direction dir,
             api::plan_params const& query,
             gbfs::gbfs_routing_data& gbfs_rd,
             n::timetable const& tt,
             n::rt_timetable const* rtt,
             n::interval<n::unixtime_t> const& intvl,
             n::routing::query const& start_time,
             n::routing::location_match_mode location_match_mode,
             std::chrono::seconds const max) {
  auto stats = std::map<std::string, std::uint64_t>{};
  offsets = r.get_offsets(rtt, l, dir, {api::ModeEnum::CAR}, std::nullopt,
                          std::nullopt, std::nullopt, std::nullopt, false,
                          get_osr_parameters(query), query.pedestrianProfile_,
                          query.elevationCosts_, max,
                          query.maxMatchingDistance_, gbfs_rd, stats);

  std::erase_if(offsets, [&](n::routing::offset const& o) {
    return r.ride_sharing_bounds_ != nullptr &&
           !r.ride_sharing_bounds_->contains(
               r.tt_->locations_.coordinates_[o.target_]);
  });

  for (auto& o : offsets) {
    o.duration_ += kODMTransferBuffer;
  }

  rides.reserve(offsets.size() * 2);

  n::routing::get_starts(
      dir == osr::direction::kForward ? n::direction::kForward
                                      : n::direction::kBackward,
      tt, rtt, intvl, offsets, {}, {}, n::routing::kMaxTravelTime,
      location_match_mode, false, rides, true, start_time.prf_idx_,
      start_time.transfer_time_settings_);
}

void prima::init(n::interval<n::unixtime_t> const& search_intvl,
                 n::interval<n::unixtime_t> const& taxi_intvl,
                 bool use_first_mile_taxi,
                 bool use_last_mile_taxi,
                 bool use_first_mile_ride_sharing,
                 bool use_last_mile_ride_sharing,
                 n::timetable const& tt,
                 n::rt_timetable const* rtt,
                 ep::routing const& r,
                 elevators const* e,
                 gbfs::gbfs_routing_data& gbfs,
                 api::Place const& from,
                 api::Place const& to,
                 api::plan_params const& query,
                 n::routing::query const& n_query,
                 unsigned api_version,
                 std::vector<api::Itinerary>& direct) {
  auto const [car_direct, direct_duration] = r.route_direct(
      e, gbfs, {}, from, to, {api::ModeEnum::CAR}, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, false, search_intvl.from_, false,
      get_osr_parameters(query), query.pedestrianProfile_,
      query.elevationCosts_, kODMMaxDuration, query.maxMatchingDistance_,
      kODMDirectFactor, api_version);
  direct.insert(end(direct), begin(car_direct), end(car_direct));

  auto const max_offset_duration =
      std::min(direct_duration,
               std::chrono::duration_cast<n::duration_t>(kODMMaxDuration));

  if (use_first_mile_ride_sharing || use_first_mile_taxi) {
    init_pt(
        first_mile_taxi_, first_mile_ride_sharing_, r, from_,
        osr::direction::kForward, query, gbfs, tt, rtt, taxi_intvl, n_query,
        query.arriveBy_ ? n_query.dest_match_mode_ : n_query.start_match_mode_,
        max_offset_duration);

    if (!use_first_mile_taxi || r.odm_bounds_ == nullptr ||
        !r.odm_bounds_->contains(from_.pos_)) {
      first_mile_taxi_.clear();
    } else {
      std::erase_if(first_mile_taxi_, [&](n::routing::offset const& o) {
        return !r.odm_bounds_->contains(
            r.tt_->locations_.coordinates_[o.target_]);
      });
    }

    if (!use_first_mile_ride_sharing) {
      first_mile_ride_sharing_.clear();
    }
  }

  if (use_last_mile_ride_sharing || use_last_mile_taxi) {
    init_pt(
        last_mile_taxi_, last_mile_ride_sharing_, r, to_,
        osr::direction::kBackward, query, gbfs, tt, rtt, taxi_intvl, n_query,
        query.arriveBy_ ? n_query.start_match_mode_ : n_query.dest_match_mode_,
        max_offset_duration);

    if (!use_last_mile_taxi || r.odm_bounds_ == nullptr ||
        !r.odm_bounds_->contains(to_.pos_)) {
      last_mile_taxi_.clear();
    } else {
      std::erase_if(last_mile_taxi_, [&](n::routing::offset const& o) {
        return !r.odm_bounds_->contains(
            r.tt_->locations_.coordinates_[o.target_]);
      });
    }

    if (!use_last_mile_ride_sharing) {
      last_mile_ride_sharing_.clear();
    }
  }

  auto const by_duration = [](auto const& a, auto const& b) {
    return a.duration_ < b.duration_;
  };
  utl::sort(first_mile_taxi_, by_duration);
  utl::sort(last_mile_taxi_, by_duration);
}

std::int64_t to_millis(n::unixtime_t const t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             t.time_since_epoch())
      .count();
}

n::unixtime_t to_unix(std::int64_t const t) {
  return n::unixtime_t{
      std::chrono::duration_cast<n::i32_minutes>(std::chrono::milliseconds{t})};
}

json::array to_json(std::vector<n::routing::start> const& rides,
                    n::timetable const& tt,
                    which_mile const wm) {
  auto a = json::array{};
  utl::equal_ranges_linear(
      rides,
      [](n::routing::start const& a, n::routing::start const& b) {
        return a.stop_ == b.stop_;
      },
      [&](auto&& from_it, auto&& to_it) {
        auto const& pos = tt.locations_.coordinates_[from_it->stop_];
        a.emplace_back(json::value{
            {"lat", pos.lat_},
            {"lng", pos.lng_},
            {"times",
             utl::all(from_it, to_it) |
                 utl::transform([&](n::routing::start const& s) {
                   return wm == kFirstMile
                              ? to_millis(s.time_at_stop_ - kODMTransferBuffer)
                              : to_millis(s.time_at_stop_ + kODMTransferBuffer);
                 }) |
                 utl::emplace_back_to<json::array>()}});
      });
  return a;
}

void tag_invoke(json::value_from_tag const&,
                json::value& jv,
                capacities const& c) {
  jv = {{"wheelchairs", c.wheelchairs_},
        {"bikes", c.bikes_},
        {"passengers", c.passengers_},
        {"luggage", c.luggage_}};
}

std::string make_whitelist_request(
    osr::location const& from,
    osr::location const& to,
    std::vector<n::routing::start> const& first_mile,
    std::vector<n::routing::start> const& last_mile,
    n::event_type const fixed,
    capacities const& cap,
    n::timetable const& tt) {
  return json::serialize(
      json::value{{"start", {{"lat", from.pos_.lat_}, {"lng", from.pos_.lng_}}},
                  {"target", {{"lat", to.pos_.lat_}, {"lng", to.pos_.lng_}}},
                  {"startBusStops", to_json(first_mile, tt, kFirstMile)},
                  {"targetBusStops", to_json(last_mile, tt, kLastMile)},
                  {"directTimes", json::array{}},
                  {"startFixed", fixed == n::event_type::kDep},
                  {"capacities", json::value_from(cap)}});
}

std::size_t prima::n_ride_sharing_events() const {
  return first_mile_ride_sharing_.size() + last_mile_ride_sharing_.size();
}

std::size_t n_rides_in_response(json::array const& ja) {
  return std::accumulate(
      ja.begin(), ja.end(), std::size_t{0U},
      [](auto const& a, auto const& b) { return a + b.as_array().size(); });
}

void fix_odm_durations(nr::journey& j) {
  if (j.legs_.size() < 2) {
    return;
  }

  utl::visit(j.legs_.front().uses_, [&](nr::offset& o) {
    if (o.transport_mode_id_ != kOdmTransportModeId &&
        o.transport_mode_id_ != kRideSharingTransportModeId) {
      return;
    }
    auto const l = begin(j.legs_);
    l->arr_time_ -= kODMTransferBuffer;
    o.duration_ -= kODMTransferBuffer;
    j.legs_.emplace(
        std::next(l), n::direction::kForward, l->to_, l->to_, l->arr_time_,
        std::next(l)->dep_time_,
        n::footpath{l->to_, std::next(l)->dep_time_ - l->arr_time_});
  });

  utl::visit(j.legs_.back().uses_, [&](nr::offset& o) {
    if (o.transport_mode_id_ != kOdmTransportModeId &&
        o.transport_mode_id_ != kRideSharingTransportModeId) {
      return;
    }
    auto const l = std::prev(end(j.legs_));
    l->dep_time_ += kODMTransferBuffer;
    o.duration_ -= kODMTransferBuffer;
    j.legs_.emplace(
        l, n::direction::kForward, l->from_, l->from_, std::prev(l)->arr_time_,
        l->dep_time_,
        n::footpath{l->from_, l->dep_time_ - std::prev(l)->arr_time_});
  });
}

}  // namespace motis::odm