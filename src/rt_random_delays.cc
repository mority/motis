#include "motis/rt_random_delays.h"

#include <algorithm>
#include <cmath>

#include "cista/strong.h"

#include "date/date.h"

#include "utl/timer.h"

#include "nigiri/logging.h"
#include "nigiri/common/interval.h"
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

  // The feed's "now" and how far ahead of it the feed reaches. A transport is
  // in the feed iff it is running (or about to run) inside this interval -
  // that is what a real feed delivers, and it is what bounds
  // `rt_timetable::coverage_`.
  auto const now = n::unixtime_t{
      std::chrono::time_point_cast<n::unixtime_t::duration>(rtt.base_day_) +
      opt.time_of_day_};
  auto const feed = n::interval<n::unixtime_t>{now, now + opt.window_};

  // The days swept for transports falling into `feed`. One day of slack on
  // either side catches transports that departed yesterday and are still
  // running.
  //
  // Real-time event times are stored as `delta_t` (int16 minutes relative to
  // the base day), so days too far away from the base day can not be
  // represented -- not a concern at one day, but kept as a guard.
  auto const first_day =
      std::clamp(base_day - kRandomDelayDayRadius,
                 std::max(0, base_day - kMaxDayOffset), std::max(0, n_tt_days));
  auto const last_day = std::max(
      first_day, std::min({base_day + kRandomDelayDayRadius + 1, n_tt_days,
                           static_cast<int>(n::kMaxDays),
                           base_day + kMaxDayOffset + 1}));
  auto const to_date = [&](int const d) {
    return date::format("%F", tt_days.from_ + date::days{d});
  };

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

      auto const tr = n::transport{t, day};

      // Not in the feed's window: a real feed does not report a transport
      // that has already arrived or that does not run until much later.
      // Checked before anything is drawn from `gen` so that the window does
      // not change which random numbers a transport gets.
      auto const first_dep =
          tt.event_time(tr, n::stop_idx_t{0U}, n::event_type::kDep);
      auto const last_arr = tt.event_time(
          tr, static_cast<n::stop_idx_t>(n_stops - 1U), n::event_type::kArr);
      if (last_arr < feed.from_ || feed.to_ <= first_dep) {
        continue;
      }

      auto r = n::rt::run{.t_ = tr,
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
         "random delays: seed={}, now={}, window=[{}, {}) ({}min), swept days "
         "[{}, {}), {} covered ({} of them delayed), {} cancelled transports",
         opt.seed_, now, feed.from_, feed.to_, opt.window_.count(),
         to_date(first_day), to_date(last_day), n_covered, n_delayed,
         n_cancelled);
}

}  // namespace motis
