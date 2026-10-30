#include "motis/pickup_dropoff.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <tuple>
#include <vector>

#include "utl/erase_duplicates.h"
#include "utl/helpers/algorithm.h"
#include "utl/parallel_for.h"
#include "utl/progress_tracker.h"

#include "geo/box.h"
#include "geo/latlng.h"

#include "nigiri/special_stations.h"

#include "osr/routing/dijkstra.h"
#include "osr/routing/profiles/car.h"
#include "osr/routing/profiles/foot.h"

#include "motis/get_loc.h"
#include "motis/osr/max_distance.h"
#include "motis/osr/parameters.h"

namespace n = nigiri;

namespace motis {

namespace {

constexpr auto const kMaxWalk = std::chrono::minutes{5};
constexpr auto const kMaxWalkCost = static_cast<osr::cost_t>(
    std::chrono::duration_cast<std::chrono::seconds>(kMaxWalk).count());
constexpr auto const kMaxSelectCost = kMaxWalkCost + 900U;
constexpr auto const kMaxStopSpeedKmh = 60U;

struct candidate {
  osr::node_idx_t node_;
  osr::cost_t cost_;
  osr::duration_t duration_;
};

struct state {
  osr::match_result match_;
  osr::dijkstra<osr::foot<false>> foot_;
  osr::dijkstra<osr::car> car_;
  std::vector<candidate> candidates_;
  std::vector<bool> keep_;
};

struct result {
  std::vector<pickup_dropoff_point> pickup_, drop_off_;
};

bool is_near_flex_area(n::timetable const& tt,
                       geo::latlng const& pos,
                       double const dist) {
  auto const box = geo::box{pos, dist};
  auto found = false;
  tt.flex_area_rtree_.search(box.min_.lnglat_float(), box.max_.lnglat_float(),
                             [&](auto&&, auto&&, n::flex_area_idx_t) {
                               found = true;
                               return false;
                             });
  return found;
}

bool is_ground_level(osr::level_t const l) {
  return l == osr::kNoLevel || l == osr::level_t{0.F};
}

bool can_stop(osr::ways::routing const& r, osr::node_idx_t const x) {
  return r.node_properties_[x].is_car_accessible() &&
         utl::any_of(r.node_ways_[x], [&](osr::way_idx_t const way) {
           auto const p = r.way_properties_[way];
           return p.is_car_accessible() &&
                  p.max_speed_km_per_h() <= kMaxStopSpeedKmh &&
                  is_ground_level(p.from_level()) &&
                  is_ground_level(p.to_level());
         });
}

// Car-accessible nodes within walking range of `pos`. Forward: walk from
// `pos` (pickup), backward: walk to `pos` (drop-off).
void get_candidates(osr::ways const& w,
                    osr::lookup const& lookup,
                    osr::location const& pos,
                    osr::direction const dir,
                    double const max_matching_distance,
                    state& s) {
  auto const params = osr::foot<false>::parameters{};

  s.match_.clear();
  lookup.match<osr::foot<false>>(params, pos, false, dir, max_matching_distance,
                                 nullptr, s.match_);
  auto const m = s.match_[osr::match_idx_t{0U}];

  s.foot_.reset(
      {.profile_ = params, .w_ = &w, .max_ = kMaxWalkCost, .dir_ = dir});
  for (auto j = 0U; j != m.size(); ++j) {
    for (auto const& nc : {m.left(j), m.right(j)}) {
      if (!nc.valid() || nc.cost_ >= kMaxWalkCost) {
        continue;
      }
      osr::foot<false>::resolve_endpoint(*w.r_, m.way_[j], nc.node_, m.lvl_,
                                         osr::route_end_of(dir),
                                         osr::endpoint_role::kRoot, m.state_[j],
                                         [&](osr::foot<false>::node const x) {
                                           s.foot_.add_start({x, nc.cost_});
                                         });
    }
  }
  s.foot_.run();

  s.candidates_.clear();
  for (auto const& [x, e] : s.foot_.cost_) {
    auto const cost = e.cost(x);
    if (cost <= kMaxWalkCost && can_stop(*w.r_, x.n_)) {
      s.candidates_.push_back({x.n_, cost, e.duration(x)});
    }
  }
  utl::erase_duplicates(
      s.candidates_,
      [](candidate const& a, candidate const& b) {
        return std::tie(a.node_, a.cost_) < std::tie(b.node_, b.cost_);
      },
      [](candidate const& a, candidate const& b) {
        return a.node_ == b.node_;
      });
}

// Keeps the candidates that are the best point for some car
// origin (drop-off, backward search) or destination (pickup, forward search)
// beyond walking range: the car search is seeded with the walks, so every
// state is labelled by its best candidate. States inside walking range don't
// count, there every candidate is best for a car already standing on it.
void keep_best(osr::ways const& w,
               geo::latlng const& pos,
               double const walk_dist,
               osr::direction const dir,
               state& s) {
  s.car_.reset({.profile_ = osr::car::parameters{},
                .w_ = &w,
                .max_ = kMaxSelectCost,
                .dir_ = dir});
  for (auto const& c : s.candidates_) {
    osr::car::resolve_all(*w.r_, c.node_, [&](osr::car::node const x) {
      s.car_.add_start({x, c.cost_}, c.duration_);
    });
  }
  s.car_.run();

  auto const in_walk_range = [&](osr::node_idx_t const x) {
    return geo::distance(w.get_node_pos(x).as_latlng(), pos) < walk_dist;
  };
  auto const keep_seed = [&](osr::car::node x) {
    for (auto pred = std::optional{x}; pred.has_value();
         pred = s.car_.cost_.at(x.get_key()).pred(x)) {
      x = *pred;
    }
    auto const it =
        std::lower_bound(begin(s.candidates_), end(s.candidates_), x.n_,
                         [](candidate const& c, osr::node_idx_t const n) {
                           return c.node_ < n;
                         });
    s.keep_[static_cast<std::size_t>(std::distance(begin(s.candidates_), it))] =
        true;
  };
  for (auto const& [n, e] : s.car_.cost_) {
    if (in_walk_range(n)) {
      continue;
    }
    osr::car::resolve_all(*w.r_, n, [&](osr::car::node const x) {
      auto const pred = e.pred(x);
      if (e.cost(x) != osr::kInfeasible && pred.has_value() &&
          in_walk_range(pred->n_)) {
        keep_seed(x);
      }
    });
  }
}

std::vector<pickup_dropoff_point> get_points(osr::ways const& w,
                                             osr::lookup const& lookup,
                                             osr::location const& pos,
                                             double const walk_dist,
                                             osr::direction const dir,
                                             double const max_matching_distance,
                                             state& s) {
  get_candidates(w, lookup, pos, dir, max_matching_distance, s);
  s.keep_.assign(s.candidates_.size(), false);
  keep_best(w, pos.pos_, walk_dist, dir, s);

  auto points = std::vector<pickup_dropoff_point>{};
  for (auto i = 0U; i != s.candidates_.size(); ++i) {
    if (s.keep_[i]) {
      points.push_back({s.candidates_[i].node_, s.candidates_[i].duration_});
    }
  }
  return points;
}

}  // namespace

pickup_dropoff_points get_pickup_dropoff_points(
    n::timetable const& tt,
    osr::ways const& w,
    osr::lookup const& lookup,
    osr::platforms const& pl,
    vector_map<n::location_idx_t, osr::platform_idx_t> const& matches,
    bool const all_locations,
    double const max_matching_distance) {
  auto const max_dist =
      get_max_distance(osr::search_profile::kFoot, osr_parameters{}, kMaxWalk);

  auto const pt = utl::get_active_progress_tracker();
  pt->in_high(tt.n_locations());

  auto ret = pickup_dropoff_points{};
  utl::parallel_ordered_collect_threadlocal<state>(
      tt.n_locations(),
      [&](state& s, std::size_t const i) {
        auto const l =
            n::location_idx_t{static_cast<n::location_idx_t::value_t>(i)};
        if (n::is_special(l)) {
          return result{};
        }

        auto const pos = get_loc(tt, w, pl, matches, l);
        if (!all_locations && tt.location_location_groups_[l].empty() &&
            !is_near_flex_area(tt, pos.pos_, max_dist)) {
          return result{};
        }

        return result{.pickup_ = get_points(w, lookup, pos, max_dist,
                                            osr::direction::kForward,
                                            max_matching_distance, s),
                      .drop_off_ = get_points(w, lookup, pos, max_dist,
                                              osr::direction::kBackward,
                                              max_matching_distance, s)};
      },
      [&](std::size_t, result&& r) {
        ret.pickup_.emplace_back(r.pickup_);
        ret.drop_off_.emplace_back(r.drop_off_);
      },
      pt->update_fn());
  return ret;
}

}  // namespace motis
