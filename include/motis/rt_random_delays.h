#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "date/date.h"

#include "motis/fwd.h"

namespace motis {

// Parameters for `generate_random_delays()`.
struct random_delay_options {
  // Seed of the pseudo random number generator. Two runs with the same seed,
  // the same timetable and the same options produce the same real-time
  // timetable.
  std::uint64_t seed_{0U};

  // Probability that a transport appears in the real-time feed at all [0, 1].
  // A real feed normally covers (close to) all of the day's service, and says
  // about most of it that it is on time - so a covered transport still gets an
  // `rt_transport`, just without any deviation from its schedule. That matters
  // beyond statistics: materialising an `rt_transport` clears the day in
  // `rtt.transport_traffic_days_`, moving the transport off the static scan
  // onto the real-time one.
  double coverage_{1.0};

  // Probability that a *covered* transport is actually late [0, 1].
  // coverage_ * lateness_ is the share of all transports that are delayed.
  double lateness_{0.2};

  // Probability that a transport is cancelled [0, 1]. Checked before
  // `delay_probability_`, i.e. cancelled transports are never delayed.
  double cancel_probability_{0.0};

  // Maximum delay in minutes.
  unsigned max_delay_{30U};

  // The day of the feed's "now" (see `time_of_day_`), and the base day of the
  // real-time timetable. Default: the first day of the timetable, which is
  // where `motis generate` starts its default query window.
  std::optional<date::sys_days> date_{};

  // Time of day (UTC) of the feed's "now", as hours after midnight on
  // `date_`. Real-time is only relevant for here-and-now queries, so all
  // queries should ask for exactly this instant - otherwise they route on the
  // scheduled timetable in all but name. Matches `motis generate
  // --time_of_day`.
  std::chrono::hours time_of_day_{8};

  // How far ahead of "now" the feed reaches. A transport is in the feed iff
  // it is running (or about to run) somewhere in `[now, now + window_)`, i.e.
  // iff `[first departure, last arrival]` intersects that interval.
  //
  // This is what bounds `rt_timetable::coverage_`, so it is the single most
  // important knob for anything that keys off the coverage. Real feeds carry
  // the currently running service plus the next couple of hours; generating
  // whole days instead materialises an `rt_transport` for essentially the
  // entire timetable and makes every query look real-time relevant.
  std::chrono::minutes window_{180};
};

// How many days around the base day are swept for transports that fall into
// the feed window. One day of slack on either side catches transports that
// departed yesterday and are still running, and the timetable's own
// midnight-crossing offsets.
constexpr auto const kRandomDelayDayRadius = 1;

// Deterministically generates random delays and cancellations in `rtt`.
//
// The random numbers for each transport are derived from the seed and the
// (timetable global) transport index and day index. Therefore, the result only
// depends on the timetable, the options and this binary - not on the order in
// which transports are processed, the number of threads or the base day of the
// real-time timetable.
//
// Does not update the lower bound graph, call `rtt.update_lbs(tt)` afterwards.
void generate_random_delays(nigiri::timetable const&,
                            nigiri::rt_timetable&,
                            random_delay_options const&);

}  // namespace motis
