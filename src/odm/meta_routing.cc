#include "motis/odm/meta_routing.h"

#include "boost/asio/co_spawn.hpp"
#include "boost/asio/detached.hpp"
#include "boost/asio/io_context.hpp"
#include "boost/thread/tss.hpp"

#include "nigiri/routing/journey.h"
#include "nigiri/routing/limits.h"
#include "nigiri/routing/query.h"
#include "nigiri/routing/raptor/raptor_state.h"
#include "nigiri/routing/raptor_search.h"
#include "nigiri/routing/start_times.h"
#include "nigiri/types.h"

#include "osr/routing/route.h"

#include "motis-api/motis-api.h"
#include "motis/endpoints/routing.h"
#include "motis/gbfs/routing_data.h"
#include "motis/http_req.h"
#include "motis/odm/json.h"
#include "motis/odm/prima_state.h"
#include "motis/place.h"

namespace motis::odm {

namespace n = nigiri;
using namespace std::chrono_literals;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static boost::thread_specific_ptr<prima_state> p_state;

static auto const kPrimaUrl = boost::urls::url{""};
static auto const kPrimaHeaders = std::map<std::string, std::string>{
    {"Content-Type", "application/json"}, {"Accept", "application/json"}};

static auto const kTransportModeIdODM = n::transport_mode_id_t{23};

n::interval<n::unixtime_t> get_dest_intvl(
    n::direction dir, n::interval<n::unixtime_t> const& start_intvl) {
  return dir == n::direction::kForward
             ? n::interval<n::unixtime_t>{start_intvl.from_,
                                          start_intvl.to_ + 24h}
             : n::interval<n::unixtime_t>{start_intvl.from_ - 24h,
                                          start_intvl.to_};
}

std::vector<n::routing::offset> get_offsets(
    ep::routing const& r,
    osr::location const& pos,
    osr::direction const dir,
    std::vector<api::ModeEnum> const& modes,
    bool const wheelchair,
    std::chrono::seconds const max,
    unsigned const max_matching_distance,
    gbfs::gbfs_routing_data& gbfs,
    ep::stats_map_t& stats) {
  return r.get_offsets(pos, dir, modes, std::nullopt, std::nullopt,
                       std::nullopt, wheelchair, max, max_matching_distance,
                       gbfs, stats);
}

void init(prima_state& ps,
          api::Place const& from,
          api::Place const& to,
          api::plan_params const& query) {
  ps.from_ = geo::latlng{from.lat_, from.lon_};
  ps.to_ = geo::latlng{to.lat_, to.lon_};
  ps.fixed_ = query.arriveBy_ ? kArr : kDep;
  ps.cap_ = {
      .wheelchairs_ = static_cast<std::uint8_t>(
          query.pedestrianProfile_ == api::PedestrianProfileEnum::WHEELCHAIR
              ? 1U
              : 0U),
      .bikes_ =
          static_cast<std::uint8_t>(query.requireBikeTransport_ ? 1U : 0U),
      .passengers_ = 1U,
      .luggage_ = 0U};
}

void init_pt(std::vector<n::routing::start>& rides,
             ep::routing const& r,
             osr::location const& l,
             osr::direction dir,
             api::plan_params const& query,
             gbfs::gbfs_routing_data& gbfs,
             motis::ep::stats_map_t& odm_stats,
             n::timetable const& tt,
             n::rt_timetable const* rtt,
             n::interval<n::unixtime_t> const& intvl,
             n::routing::query const& start_time,
             n::routing::location_match_mode location_match_mode) {
  static auto const kNoTdOffsets =
      hash_map<n::location_idx_t, std::vector<n::routing::td_offset>>{};

  auto const offsets = get_offsets(
      r, l, dir, {api::ModeEnum::CAR},
      query.pedestrianProfile_ == api::PedestrianProfileEnum::WHEELCHAIR,
      std::chrono::seconds{query.maxPreTransitTime_},
      query.maxMatchingDistance_, gbfs, odm_stats);

  rides.clear();
  rides.reserve(offsets.size() * 2);

  n::routing::get_starts(
      dir == osr::direction::kForward ? n::direction::kForward
                                      : n::direction::kBackward,
      tt, rtt, intvl, offsets, kNoTdOffsets, n::routing::kMaxTravelTime,
      location_match_mode, false, rides, true, start_time.prf_idx_,
      start_time.transfer_time_settings_);
  utl::sort(rides, [](auto&& a, auto&& b) {
    return std::tie(a.stop_, a.time_at_start_, a.time_at_stop_) <
           std::tie(b.stop_, b.time_at_start_, b.time_at_stop_);
  });
}

void init_direct(std::vector<direct_ride>& direct_rides,
                 ep::routing const& r,
                 elevators const* e,
                 gbfs::gbfs_routing_data& gbfs,
                 api::Place const& from_p,
                 api::Place const& to_p,
                 n::interval<n::unixtime_t> const intvl,
                 api::plan_params const& query) {
  auto [direct, duration] = r.route_direct(
      e, gbfs, from_p, to_p, {api::ModeEnum::CAR}, std::nullopt, std::nullopt,
      std::nullopt, intvl.from_,
      query.pedestrianProfile_ == api::PedestrianProfileEnum::WHEELCHAIR,
      std::chrono::seconds{query.maxDirectTime_});
  direct_rides.clear();
  if (query.arriveBy_) {
    for (auto arr =
             std::chrono::floor<std::chrono::hours>(intvl.to_ - duration) +
             duration;
         intvl.contains(arr); arr -= 1h) {
      direct_rides.emplace_back(arr - duration, arr);
    }
  } else {
    for (auto dep = std::chrono::ceil<std::chrono::hours>(intvl.from_);
         intvl.contains(dep); dep += 1h) {
      direct_rides.emplace_back(dep, dep + duration);
    }
  }
}

auto ride_time_halves(std::vector<n::routing::start>& rides) {
  utl::sort(rides, [](auto const& a, auto const& b) {
    auto const ride_time = [](auto const& ride) {
      return std::chrono::abs(ride.time_at_stop_ - ride.time_at_start_);
    };
    return ride_time(a) < ride_time(b);
  });
  auto const half = rides.size() / 2;
  auto lo = rides | std::views::take(half);
  auto hi = rides | std::views::drop(half);
  auto const stop_comp = [](auto const& a, auto const& b) {
    return a.stop_ < b.stop_;
  };
  std::ranges::sort(lo, stop_comp);
  std::ranges::sort(hi, stop_comp);
  return std::pair{lo, hi};
}

enum offset_event_type { kTimeAtStart, kTimeAtStop };
auto get_td_offsets(auto const& rides, offset_event_type const oet) {
  auto td_offsets =
      hash_map<n::location_idx_t, std::vector<n::routing::td_offset>>{};
  utl::equal_ranges_linear(
      rides, [](auto const& a, auto const& b) { return a.stop_ == b.stop_; },
      [&](auto&& from_it, auto&& to_it) {
        td_offsets.emplace(from_it->stop_,
                           std::vector<n::routing::td_offset>{});
        for (auto const& r : n::it_range{from_it, to_it}) {
          td_offsets.at(from_it->stop_)
              .emplace_back(
                  oet == kTimeAtStart ? r.time_at_start_ : r.time_at_stop_,
                  std::chrono::abs(r.time_at_start_ - r.time_at_stop_),
                  kTransportModeIdODM);
          td_offsets.at(from_it->stop_)
              .emplace_back(oet == kTimeAtStart ? r.time_at_start_ + 1min
                                                : r.time_at_stop_ + 1min,
                            n::kInfeasible, kTransportModeIdODM);
        }
      });
  return td_offsets;
}

bool is_intermodal(place_t const& p) {
  return std::holds_alternative<osr::location>(p);
}

n::routing::location_match_mode get_match_mode(place_t const& p) {
  return is_intermodal(p) ? n::routing::location_match_mode::kIntermodal
                          : n::routing::location_match_mode::kEquivalent;
}

std::vector<n::routing::offset> station_start(n::location_idx_t const l) {
  return {{l, n::duration_t{0U}, 0U}};
}

api::plan_response meta_routing(
    ep::routing const& r,
    api::plan_params const& query,
    std::vector<api::ModeEnum> const& pre_transit_modes,
    std::vector<api::ModeEnum> const& post_transit_modes,
    std::vector<api::ModeEnum> const& direct_modes,
    std::variant<osr::location, tt_location> const& from,
    std::variant<osr::location, tt_location> const& to,
    api::Place const& from_p,
    api::Place const& to_p,
    const nigiri::routing::query& start_time,
    bool odm_pre_transit,
    bool odm_post_transit,
    bool odm_direct) {
  utl::verify(r.tt_ != nullptr && r.tags_ != nullptr,
              "mode=TRANSIT requires timetable to be loaded");

  auto const tt = r.tt_;
  auto const rt = r.rt_;
  auto const rtt = rt->rtt_.get();
  auto const e = r.rt_->e_.get();
  auto gbfs_rd = gbfs::gbfs_routing_data{r.w_, r.l_, r.gbfs_};
  if (ep::blocked.get() == nullptr && r.w_ != nullptr) {
    ep::blocked.reset(new osr::bitvec<osr::node_idx_t>{r.w_->n_nodes()});
  }

  auto const& start = query.arriveBy_ ? to : from;
  auto const& dest = query.arriveBy_ ? from : to;
  auto const& start_modes =
      query.arriveBy_ ? post_transit_modes : pre_transit_modes;
  auto const& dest_modes =
      query.arriveBy_ ? pre_transit_modes : post_transit_modes;
  auto const& start_form_factors = query.arriveBy_
                                       ? query.postTransitRentalFormFactors_
                                       : query.preTransitRentalFormFactors_;
  auto const& dest_form_factors = query.arriveBy_
                                      ? query.preTransitRentalFormFactors_
                                      : query.postTransitRentalFormFactors_;
  auto const& start_propulsion_types =
      query.arriveBy_ ? query.postTransitRentalPropulsionTypes_
                      : query.preTransitRentalPropulsionTypes_;
  auto const& dest_propulsion_types =
      query.arriveBy_ ? query.postTransitRentalPropulsionTypes_
                      : query.preTransitRentalPropulsionTypes_;
  auto const& start_rental_providers = query.arriveBy_
                                           ? query.postTransitRentalProviders_
                                           : query.preTransitRentalProviders_;
  auto const& dest_rental_providers = query.arriveBy_
                                          ? query.preTransitRentalProviders_
                                          : query.postTransitRentalProviders_;

  auto stats = motis::ep::stats_map_t{};

  auto const start_intvl = std::visit(
      utl::overloaded{[](n::interval<n::unixtime_t> const i) { return i; },
                      [](n::unixtime_t const t) {
                        return n::interval<n::unixtime_t>{t, t};
                      }},
      start_time.start_time_);
  auto const dest_intvl = get_dest_intvl(
      query.arriveBy_ ? n::direction::kBackward : n::direction::kForward,
      start_intvl);
  auto const& from_intvl = query.arriveBy_ ? dest_intvl : start_intvl;
  auto const& to_intvl = query.arriveBy_ ? start_intvl : dest_intvl;

  if (p_state.get() == nullptr) {
    p_state.reset(new prima_state{});
  }

  init(*p_state, from_p, to_p, query);

  if (odm_pre_transit && holds_alternative<osr::location>(from)) {
    init_pt(p_state->from_rides_, r, std::get<osr::location>(from),
            osr::direction::kForward, query, gbfs_rd, stats, *tt, rtt,
            from_intvl, start_time,
            query.arriveBy_ ? start_time.dest_match_mode_
                            : start_time.start_match_mode_);
  }

  if (odm_post_transit && holds_alternative<osr::location>(to)) {
    init_pt(p_state->to_rides_, r, std::get<osr::location>(to),
            osr::direction::kBackward, query, gbfs_rd, stats, *tt, rtt,
            to_intvl, start_time,
            query.arriveBy_ ? start_time.start_match_mode_
                            : start_time.dest_match_mode_);
  }

  if (odm_direct && r.w_ && r.l_) {
    init_direct(p_state->direct_rides_, r, e, gbfs_rd, from_p, to_p, to_intvl,
                query);
  }

  auto odm_networking = true;
  try {
    // TODO the fiber/thread should yield until network response arrives?
    auto ioc = boost::asio::io_context{};
    boost::asio::co_spawn(
        ioc,
        [&]() -> boost::asio::awaitable<void> {
          auto const bl_response = co_await http_POST(
              kPrimaUrl, kPrimaHeaders, serialize(*p_state, *tt), 10s);
          update(*p_state, get_http_body(bl_response));
        },
        boost::asio::detached);
    ioc.run();
  } catch (std::exception const& e) {
    std::cout << "blacklisting failed: " << e.what();
    odm_networking = false;
  }

  auto const walk_start = std::visit(
      utl::overloaded{
          [&](tt_location const l) { return station_start(l.l_); },
          [&](osr::location const& pos) {
            auto const dir = query.arriveBy_ ? osr::direction::kBackward
                                             : osr::direction::kForward;
            return r.get_offsets(pos, dir, start_modes, start_form_factors,
                                 start_propulsion_types, start_rental_providers,
                                 query.pedestrianProfile_ ==
                                     api::PedestrianProfileEnum::WHEELCHAIR,
                                 std::chrono::seconds{query.maxPreTransitTime_},
                                 query.maxMatchingDistance_, gbfs_rd, stats);
          }},
      start);

  auto const walk_dest = std::visit(
      utl::overloaded{[&](tt_location const l) { return station_start(l.l_); },
                      [&](osr::location const& pos) {
                        auto const dir = query.arriveBy_
                                             ? osr::direction::kForward
                                             : osr::direction::kBackward;
                        return r.get_offsets(
                            pos, dir, dest_modes, dest_form_factors,
                            dest_propulsion_types, dest_rental_providers,
                            query.pedestrianProfile_ ==
                                api::PedestrianProfileEnum::WHEELCHAIR,
                            std::chrono::seconds{query.maxPostTransitTime_},
                            query.maxMatchingDistance_, gbfs_rd, stats);
                      }},
      dest);

  auto const [from_rides_short, from_rides_long] =
      ride_time_halves(p_state->from_rides_);
  auto const [to_rides_short, to_rides_long] =
      ride_time_halves(p_state->to_rides_);

  auto const td_offsets_from_short = get_td_offsets(
      from_rides_short, query.arriveBy_ ? offset_event_type::kTimeAtStop
                                        : offset_event_type::kTimeAtStart);
  auto const td_offsets_from_long = get_td_offsets(
      from_rides_long, query.arriveBy_ ? offset_event_type::kTimeAtStop
                                       : offset_event_type::kTimeAtStart);
  auto const td_offsets_to_short = get_td_offsets(
      to_rides_short, query.arriveBy_ ? offset_event_type::kTimeAtStart
                                      : offset_event_type::kTimeAtStop);
  auto const td_offsets_to_long = get_td_offsets(
      to_rides_long, query.arriveBy_ ? offset_event_type::kTimeAtStart
                                     : offset_event_type::kTimeAtStop);

  // TODO start fibers to do the ODM routing

  // TODO whitelist request for ODM rides used in journeys

  // TODO remove journeys with non-whitelisted ODM rides

  return std::vector<nigiri::routing::journey>{};
}

}  // namespace motis::odm