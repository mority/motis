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

  // Base day of the real-time timetable. Default: the first day of the
  // timetable, which is where `motis generate` starts its default query
  // window. Real-time data can only be generated for days close to the base
  // day (~3 weeks, limited by `delta_t`), so the base day has to match the
  // days the queries are asking for.
  std::optional<date::sys_days> base_day_{};

  // Days to generate real-time data for, relative to the base day of the
  // real-time timetable: [base_day + first_day_, base_day + first_day_ +
  // n_days_). The default covers the 15 days `motis generate` distributes its
  // queries over by default (first day of the timetable + 14 days) plus the
  // day before to also give transports running over midnight real-time data.
  int first_day_{-1};
  unsigned n_days_{16U};
};

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
