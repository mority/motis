#pragma once

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

  // Probability that a transport is delayed [0, 1].
  double delay_probability_{0.5};

  // Probability that a transport is cancelled [0, 1]. Checked before
  // `delay_probability_`, i.e. cancelled transports are never delayed.
  double cancel_probability_{0.0};

  // Maximum delay in minutes.
  unsigned max_delay_{30U};

  // The day real-time data is generated for. Real-time feeds normally only
  // cover what is currently running, so this generates data for this day and
  // the following one - the following day so that journeys departing on
  // `date_` and travelling overnight still see real-time data, matching a feed
  // that already knows about tomorrow's early departures.
  //
  // Also the base day of the real-time timetable. Default: the first day of
  // the timetable, which is where `motis generate` starts its default query
  // window. All queries should ask for `date_`, otherwise they route on the
  // scheduled timetable in all but name.
  std::optional<date::sys_days> date_{};
};

// Number of days `generate_random_delays()` covers: `date_` and the day after.
constexpr auto const kRandomDelayDays = 2;

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
