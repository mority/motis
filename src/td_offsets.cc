#include "motis/td_offsets.h"

#include "motis/transport_mode.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <optional>

#include "nigiri/footpath.h"

#include "motis/types.h"

namespace n = nigiri;

namespace motis {

void td_norm_stats::add(std::size_t const n_raw,
                        std::size_t const n_envelope,
                        std::size_t const n_fifo,
                        std::size_t const n_out,
                        std::size_t const q,
                        std::size_t const n_cut,
                        std::size_t const n_blocked,
                        bool const leading_added,
                        std::uint64_t const ns,
                        std::uint64_t const ns_envelope,
                        std::uint64_t const ns_fifo,
                        std::uint64_t const ns_closure) {
  ++n_locations_;
  n_raw_ += n_raw;
  n_envelope_ += n_envelope;
  n_fifo_ += n_fifo;
  n_out_ += n_out;
  q_max_ = std::max(q_max_, static_cast<std::uint64_t>(q));
  n_fifo_cut_ += n_cut;
  n_fifo_blocked_ += n_blocked;
  n_leading_ += leading_added ? 1U : 0U;
  ns_ += ns;
  ns_envelope_ += ns_envelope;
  ns_fifo_ += ns_fifo;
  ns_closure_ += ns_closure;
}

void td_norm_stats::write(std::map<std::string, std::uint64_t>& m,
                          std::string_view const prefix) const {
  if (n_locations_ == 0U) {
    return;
  }
  auto const key = [&](std::string_view const name) {
    return std::string{prefix} + "_" + std::string{name};
  };
  m[key("locations")] = n_locations_;
  m[key("n_raw")] = n_raw_;
  m[key("n_envelope")] = n_envelope_;
  m[key("n_fifo")] = n_fifo_;
  m[key("n_out")] = n_out_;
  m[key("q_max")] = q_max_;
  m[key("fifo_cut")] = n_fifo_cut_;
  m[key("fifo_blocked")] = n_fifo_blocked_;
  m[key("leading")] = n_leading_;
  m[key("ns")] = ns_;
  m[key("ns_envelope")] = ns_envelope_;
  m[key("ns_fifo")] = ns_fifo_;
  m[key("ns_closure")] = ns_closure_;
}

bool& fifo_repair_enabled() {
  static auto enabled = [] {
    auto const* const v = std::getenv("MOTIS_TD_NO_FIFO_REPAIR");
    return v == nullptr || std::string_view{v} == "0";
  }();
  return enabled;
}

bool is_same_td_state(n::routing::td_offset const& a,
                      n::routing::td_offset const& b) {
  return a.duration_ == b.duration_ && a.mode() == b.mode();
}

void normalize_td_offsets(std::vector<n::routing::td_offset>& offsets,
                          td_norm_stats* const stats) {
  if (offsets.empty()) {
    return;
  }

  auto const now = [] { return std::chrono::steady_clock::now(); };
  auto const t0 = now();
  auto t_envelope = t0, t_fifo = t0;
  auto const n_raw = offsets.size();
  auto q_max = std::size_t{0U};
  auto n_cut = std::size_t{0U};
  auto n_blocked = std::size_t{0U};

  // (1) lower envelope: sweep all breakpoints, keeping the fastest active
  // offer per point in time.
  std::sort(begin(offsets), end(offsets),
            [](n::routing::td_offset const& a, n::routing::td_offset const& b) {
              return a.valid_from_ < b.valid_from_;
            });

  auto active = hash_map<transport_mode_t, n::duration_t>{};
  auto envelope = std::vector<n::routing::td_offset>{};
  for (auto it = begin(offsets); it != end(offsets);) {
    auto const t = it->valid_from_;
    for (; it != end(offsets) && it->valid_from_ == t; ++it) {
      if (it->duration_ >= n::footpath::kMaxDuration) {
        active.erase(it->mode());
      } else {
        active[it->mode()] = it->duration_;
      }
    }

    q_max = std::max(q_max, active.size());

    auto best = n::routing::td_offset{t, n::footpath::kMaxDuration};
    for (auto const& [mode, duration] : active) {
      if (duration < best.duration_ ||
          (duration == best.duration_ && mode < best.mode())) {
        best = n::routing::td_offset::make(t, duration, mode);
      }
    }

    if (envelope.empty() || !is_same_td_state(envelope.back(), best)) {
      envelope.push_back(best);
    }
  }

  t_envelope = now();

  // (2) FIFO repair: walking backwards, `best_arr` is the earliest arrival
  // reachable by departing at or after the current position. An active
  // window [vf, duration] is cut at `best_arr - duration`: departing later
  // than that should wait for the faster offer instead.
  auto fixed = std::vector<n::routing::td_offset>{};
  if (fifo_repair_enabled()) {
    fixed.reserve(envelope.size());
    auto best_arr = std::optional<n::unixtime_t>{};
    for (auto i = envelope.size(); i-- != 0U;) {
      auto const& e = envelope[i];
      if (e.duration_ >= n::footpath::kMaxDuration) {
        fixed.push_back(e);
        continue;
      }

      auto const next_from = i + 1U < envelope.size()
                                 ? envelope[i + 1U].valid_from_
                                 : n::unixtime_t::max();
      if (best_arr.has_value() && *best_arr - e.duration_ <= e.valid_from_) {
        // whole window dominated by waiting for a later, faster offer
        fixed.push_back({e.valid_from_, n::footpath::kMaxDuration});
        ++n_blocked;
        continue;
      }

      if (best_arr.has_value() && *best_arr - e.duration_ < next_from) {
        // tail of the window dominated -> end the offer early
        fixed.push_back({*best_arr - e.duration_, n::footpath::kMaxDuration});
        ++n_cut;
      }
      fixed.push_back(e);
      best_arr = e.valid_from_ + e.duration_;
    }
    std::reverse(begin(fixed), end(fixed));
  } else {
    // evaluation only: step 2 disabled via MOTIS_TD_NO_FIFO_REPAIR
    fixed = envelope;
  }

  t_fifo = now();

  auto const n_envelope = envelope.size();
  auto const n_fifo = fixed.size();

  // Re-emit, starting with a dead window from the beginning of time so the
  // result is a complete step function, and dropping adjacent duplicates
  // introduced by the repair.
  offsets.clear();
  auto const zero = n::unixtime_t{n::i32_minutes{0}};
  auto const leading_added = fixed.front().valid_from_ > zero;
  if (leading_added) {
    offsets.push_back({zero, n::footpath::kMaxDuration});
  }
  for (auto const& e : fixed) {
    if (offsets.empty() || !is_same_td_state(offsets.back(), e)) {
      offsets.push_back(e);
    }
  }

  if (stats != nullptr) {
    auto const t_end = now();
    auto const ns = [](auto const a, auto const b) {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    };
    stats->add(n_raw, n_envelope, n_fifo, offsets.size(), q_max, n_cut,
               n_blocked, leading_added, ns(t0, t_end), ns(t0, t_envelope),
               ns(t_envelope, t_fifo), ns(t_fifo, t_end));
  }
}

void add_td_window(std::vector<n::routing::td_offset>& offsets,
                   n::interval<n::unixtime_t> const window,
                   n::duration_t const duration,
                   n::routing::transport_mode_t const mode) {
  if (!offsets.empty() && offsets.back().mode() == mode &&
      offsets.back().valid_from_ >= window.from_) {
    // touches or overlaps the previous window of this mode -> extend it
    offsets.back().valid_from_ =
        std::max(offsets.back().valid_from_, window.to_);
    return;
  }
  offsets.push_back(n::routing::td_offset::make(window.from_, duration, mode));
  offsets.push_back(
      n::routing::td_offset::make(window.to_, n::footpath::kMaxDuration, mode));
}

}  // namespace motis
