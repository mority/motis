#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nigiri/routing/query.h"
#include "nigiri/types.h"

namespace motis {

// Records, per query, everything `motis td-replay` needs to rebuild the
// time-dependent offsets of every design and repeat the search's lookups on
// them. Enabled by MOTIS_TD_TRACE=<directory>; the lookups are only seen in a
// build with -DNIGIRI_TD_TRACE, so never time such a build.

// One window as a producer hands it to add_td_window, before any merging.
struct td_trace_window {
  nigiri::location_idx_t location_;
  std::uint8_t dir_;  // osr::direction of the offsets it belongs to
  nigiri::unixtime_t from_, to_;
  nigiri::duration_t duration_;
  nigiri::routing::transport_mode_t mode_;
};

// A sequence as the search received it (normalized, pruned).
struct td_trace_seq {
  std::uint8_t side_;  // 0 = td_start_, 1 = td_dest_
  nigiri::location_idx_t location_;
  std::vector<nigiri::routing::td_offset> offsets_;
};

struct td_trace_lookup {
  std::uint32_t seq_;
  std::uint8_t dir_;  // nigiri::direction the lookup was evaluated in
  nigiri::unixtime_t t_;
};

struct td_trace {
  // $MOTIS_TD_TRACE, or nothing if tracing is off.
  static std::optional<std::filesystem::path> const& dir();

  // Minimum of the plain (not time-dependent) offsets, taken before
  // remove_slower_than_fastest_direct so td-replay can repeat the pruning.
  void capture_before_pruning(nigiri::routing::query const&);

  // The sequences the search will evaluate. Lookups are matched to them by
  // address, so the query must be MOVED into the search afterwards.
  void capture_query(nigiri::routing::query const&);

  void write(std::filesystem::path const& dir) const;
  static td_trace read(std::filesystem::path const&);

  std::string url_;  // "fromPlace|toPlace", also names the file
  bool arrive_by_{};
  std::optional<nigiri::duration_t> fastest_direct_;
  nigiri::duration_t min_start_plain_{}, min_dest_plain_{};
  std::vector<td_trace_window> windows_;
  std::vector<td_trace_seq> seqs_;
  std::vector<td_trace_lookup> lookups_;
  std::uint64_t n_unmatched_{};  // lookups on sequences not registered

  std::unordered_map<void const*, std::uint32_t> by_address_;  // not written
};

#ifdef NIGIRI_TD_TRACE
// Routes nigiri's lookup hook into `trace` while in scope (same thread; the
// search does not yield).
struct td_trace_scope {
  explicit td_trace_scope(td_trace&);
  ~td_trace_scope();
  td_trace_scope(td_trace_scope const&) = delete;
  td_trace_scope& operator=(td_trace_scope const&) = delete;
};
#endif

}  // namespace motis
