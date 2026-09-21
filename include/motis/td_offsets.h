#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "nigiri/common/interval.h"
#include "nigiri/routing/query.h"

namespace motis {

// Instrumentation for the evaluation: records what the three normalization
// steps actually do. One instance is accumulated over all locations of a
// query and written into the response's debugOutput, so the numbers can be
// read back per query from responses.txt.
struct td_norm_stats {
  // Accumulates one normalize_td_offsets() call.
  void add(std::size_t n_raw,
           std::size_t n_envelope,
           std::size_t n_fifo,
           std::size_t n_out,
           std::size_t q,
           std::size_t n_cut,
           std::size_t n_blocked,
           bool leading_added,
           std::uint64_t ns,
           std::uint64_t ns_envelope,
           std::uint64_t ns_fifo,
           std::uint64_t ns_closure);

  // Writes the accumulated values as `<prefix>_<name>` into `m`. Nothing is
  // written if no location was normalized, so queries without time-dependent
  // offsets do not pollute the response.
  void write(std::map<std::string, std::uint64_t>& m,
             std::string_view prefix) const;

  std::uint64_t n_locations_{};  // locations normalized
  std::uint64_t n_raw_{};  // entries before step 1
  std::uint64_t n_envelope_{};  // entries after step 1 (lower envelope)
  std::uint64_t n_fifo_{};  // entries after step 2 (FIFO repair)
  std::uint64_t n_out_{};  // entries after step 3 (closure)
  std::uint64_t q_max_{};  // max. concurrently available offers at a location
  std::uint64_t n_fifo_cut_{};  // windows whose tail step 2 cut off
  std::uint64_t n_fifo_blocked_{};  // windows step 2 blocked entirely
  std::uint64_t n_leading_{};  // locations that needed a leading inf entry
  std::uint64_t ns_{};  // time spent in normalize_td_offsets
  // ... split over the three steps, so the complexity claim in the paper
  // (step 1 sorts and builds the envelope, steps 2 and 3 are linear) can be
  // checked against measurements.
  std::uint64_t ns_envelope_{};  // step 1: lower envelope
  std::uint64_t ns_fifo_{};  // step 2: FIFO repair
  std::uint64_t ns_closure_{};  // step 3: closure / leading entry
};

// Whether step 2 (FIFO repair) runs. Initialized once on first use from the
// environment variable MOTIS_TD_NO_FIFO_REPAIR (=1 disables it), so the same
// binary can produce both sides of the A/B comparison. Returned by reference
// so tests can flip it; nothing in production writes it.
bool& fifo_repair_enabled();

void normalize_td_offsets(std::vector<nigiri::routing::td_offset>&,
                          td_norm_stats* = nullptr);

// Appends the window [from, to) of `mode` as the entries {from, duration} and
// {to, kMaxDuration}. A window that touches or overlaps the previous window of
// the same mode extends that window instead, so the entries of each mode stay
// a step function for normalize_td_offsets.
// Nothing is checked: the caller adds non-empty windows, sorted by mode and by
// `from`, and gives overlapping windows of one mode the same duration.
void add_td_window(std::vector<nigiri::routing::td_offset>&,
                   nigiri::interval<nigiri::unixtime_t> window,
                   nigiri::duration_t,
                   nigiri::routing::transport_mode_t);

}  // namespace motis
