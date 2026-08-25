#include "motis/rt_random_delays.h"

#include <algorithm>
#include <cmath>

#include "cista/strong.h"

#include "date/date.h"

#include "utl/timer.h"

#include "nigiri/logging.h"
#include "nigiri/rt/rt_timetable.h"
#include "nigiri/rt/run.h"
#include "nigiri/timetable.h"

namespace n = nigiri;

namespace motis {

// Real-time event times are `delta_t` (int16) minutes relative to the base
// day, i.e. ~22 days can be represented. Keep some slack for transports
// running over midnight and for the delays themselves.
constexpr auto const kMaxDayOffset = 20;

// splitmix64: fast, deterministic (no dependency on the standard library
// implementation) and good enough for generating test data.
struct rng {
  explicit rng(std::uint64_t const seed) : state_{seed} {}

  static std::uint64_t mix(std::uint64_t x) {
    x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
  }

  std::uint64_t next() { return mix(state_ += 0x9e3779b97f4a7c15ULL); }

  // uniform in [0, 1)
  double next_double() {
    return static_cast<double>(next() >> 11U) * 0x1.0p-53;
  }

  // uniform in [from, to]
  int next_int(int const from, int const to) {
    return from +
           static_cast<int>(next() % static_cast<std::uint64_t>(to - from + 1));
  }

  std::uint64_t state_;
};

std::uint64_t transport_seed(std::uint64_t const seed,
                             n::transport_idx_t const t,
                             n::day_idx_t const day) {
  return rng::mix(rng::mix(rng::mix(seed) + cista::to_idx(t)) +
                  cista::to_idx(day));
}

void generate_random_delays(n::timetable const& tt,
                            n::rt_timetable& rtt,
                            random_delay_options const& opt) {
  auto const timer = utl::scoped_timer{"generate random delays"};

  // Day indices are computed from `sys_days` (and not from `base_day_idx_`) to
  // stay correct if the base day is outside the timetable's date range.
  auto const tt_days = tt.internal_interval_days();
  auto const n_tt_days =
      static_cast<int>((tt_days.to_ - tt_days.from_).count());
  auto const base_day =
      static_cast<int>((rtt.base_day_ - tt_days.from_).count());

  // `date_` and the day after it: a real-time feed normally only covers what
  // is currently running, plus enough of tomorrow for journeys that travel
  // overnight. `rtt.base_day_` is `date_`, so this is [base_day, base_day + 2).
  //
  // Real-time event times are stored as `delta_t` (int16 minutes relative to
  // the base day), so days too far away from the base day can not be
  // represented -- not a concern at two days, but kept as a guard.
  auto const requested_first = base_day;
  auto const requested_last = requested_first + kRandomDelayDays;
  auto const first_day =
      std::clamp(requested_first, std::max(0, base_day - kMaxDayOffset),
                 std::max(0, base_day + kMaxDayOffset));
  auto const last_day = std::max(
      first_day,
      std::min({requested_last, n_tt_days, static_cast<int>(n::kMaxDays),
                base_day + kMaxDayOffset + 1}));
  auto const to_date = [&](int const d) {
    return date::format("%F", tt_days.from_ + date::days{d});
  };
  if (first_day != requested_first || last_day != requested_last) {
    n::log(n::log_lvl::info, "motis.rt",
           "random delays: requested days [{}, {}) truncated to [{}, {}) "
           "(timetable date range)",
           to_date(requested_first), to_date(requested_last),
           to_date(first_day), to_date(last_day));
  }

  auto n_covered = 0U, n_delayed = 0U, n_cancelled = 0U;
  auto const n_transports = n::transport_idx_t{
      static_cast<n::transport_idx_t::value_t>(tt.transport_route_.size())};
  for (auto t = n::transport_idx_t{0U}; t != n_transports; ++t) {
    auto const route = tt.transport_route_[t];
    auto const n_stops =
        static_cast<n::stop_idx_t>(tt.route_location_seq_[route].size());
    auto const providers = tt.transport_section_providers_[t];
    auto const src = providers.empty() ? n::source_idx_t{0U}
                                       : tt.providers_[providers.front()].src_;

    for (auto d = first_day; d < last_day; ++d) {
      auto const day = n::day_idx_t{static_cast<n::day_idx_t::value_t>(d)};
      if (!rtt.is_transport_active(t, day)) {
        continue;
      }

      auto r = n::rt::run{.t_ = n::transport{t, day},
                          .stop_range_ = {n::stop_idx_t{0U}, n_stops}};
      auto gen = rng{transport_seed(opt.seed_, t, day)};

      if (gen.next_double() < opt.cancel_probability_) {
        rtt.cancel_run(r);
        ++n_cancelled;
        continue;
      }

      if (gen.next_double() >= opt.coverage_) {
        continue;  // not in the feed at all -> stays a purely static transport
      }

      // Covered by the feed. Drawn before the delay magnitude so that which
      // transports are covered does not depend on `lateness_`.
      auto const is_late = gen.next_double() < opt.lateness_;

      // Skewed towards small delays: most delayed transports are only a few
      // minutes late, large delays are rare.
      auto const u = gen.next_double();
      auto delay = is_late ? static_cast<int>(std::lround(
                                 static_cast<double>(opt.max_delay_) * u * u *
                                 u))
                           : 0;

      // An on-time transport still gets an rt_transport -- that is what a real
      // feed reports, and it moves the transport onto the real-time scan.
      r.rt_ = rtt.add_rt_transport(src, tt, r.t_);

      auto pred_time = n::unixtime_t::min();
      auto const update = [&](n::stop_idx_t const stop_idx,
                              n::event_type const ev_type) {
        if (is_late) {
          delay = std::clamp(delay + gen.next_int(-3, 3), 0,
                             static_cast<int>(opt.max_delay_));
        }
        auto const new_time =
            std::max(pred_time,
                     tt.event_time(r.t_, stop_idx, ev_type) +
                         n::duration_t{static_cast<n::duration_t::rep>(delay)});
        rtt.update_time(r.rt_, stop_idx, ev_type, new_time);
        pred_time = new_time;
      };

      for (auto stop_idx = n::stop_idx_t{0U}; stop_idx != n_stops; ++stop_idx) {
        if (stop_idx != 0U) {
          update(stop_idx, n::event_type::kArr);
        }
        if (stop_idx != n_stops - 1U) {
          update(stop_idx, n::event_type::kDep);
        }
      }

      ++n_covered;
      if (is_late) {
        ++n_delayed;
      }
    }
  }

  n::log(n::log_lvl::info, "motis.rt",
         "random delays: seed={}, date={}, days=[{}, {}), {} covered ({} of "
         "them delayed), {} cancelled transports",
         opt.seed_, date::format("%F", rtt.base_day_), to_date(first_day),
         to_date(last_day), n_covered, n_delayed, n_cancelled);
}

}  // namespace motis
