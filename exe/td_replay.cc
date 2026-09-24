#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "boost/program_options.hpp"

#include "fmt/format.h"

#include "utl/enumerate.h"

#include "osr/types.h"

#include "nigiri/routing/query.h"
#include "nigiri/td_footpath.h"

#include "motis/endpoints/routing.h"
#include "motis/td_offsets.h"
#include "motis/td_trace.h"

#include "./flags.h"

// Replays the time-dependent offset lookups recorded with MOTIS_TD_TRACE on
// every design, from the same producer windows:
//
//   D0  one {from, l, m} / {to, inf, m} pair per producer window, no merging
//   D   add_td_window's output (touching same-mode windows merged)
//   A   D normalized: envelope + FIFO repair + closure, first-hit lookup
//   B   D normalized without FIFO repair, exhaustive lookup
//
//   C   compact: per stop and offer one relative window + day mask + the
//       routed duration, as the timetable stores GTFS-Flex; no per-day
//       expansion, no normalization. Rebuilt from the windows (copies of one
//       window spaced by whole days); windows that do not fit that shape get
//       their own entry (counted as c_fallback).
//
// and two schedules for A's normalization, from the same measurements:
//
//   lazy  normalize a stop on its first lookup (never-looked-up stops free)
//   ski   raw lookups on D until a stop's k-th lookup, then normalize it and
//         use A (ski rental: never much worse than the better of A and D)
//
// All four are pruned with the real remove_slower_than_fastest_direct. The
// search is identical for every design (same fronts, same lookup counts), so
// the lookups recorded on A are exactly the lookups each design performs.

namespace fs = std::filesystem;
namespace po = boost::program_options;
namespace n = nigiri;

namespace motis {

namespace {

using offsets_t = std::vector<n::routing::td_offset>;

enum arm : unsigned { kA, kB, kD, kD0, kNArms };
constexpr auto const kArmName = std::array{"A", "B", "D", "D0"};

struct counter {
  void operator()() noexcept { ++n_; }
  std::uint64_t n_{};
};

template <arm Arm, n::direction Dir, typename... Count>
std::optional<n::duration_t> lookup(offsets_t const& v,
                                    n::unixtime_t const t,
                                    Count&... count) {
  auto const s = std::span<n::routing::td_offset const>{v};
  auto const r = [&] {
    if constexpr (Arm == kA) {
      return n::get_td_duration_first<Dir>(s, t, count...);
    } else if constexpr (Arm == kB) {
      return n::get_td_duration_scan<Dir>(s, t, count...);
    } else {
      return n::get_td_duration_raw_windows<Dir>(s, t, count...);
    }
  }();
  return r.has_value() ? std::optional{r->first} : std::nullopt;
}

template <arm Arm, typename... Count>
std::optional<n::duration_t> lookup(offsets_t const& v,
                                    td_trace_lookup const& l,
                                    Count&... count) {
  return static_cast<n::direction>(l.dir_) == n::direction::kForward
             ? lookup<Arm, n::direction::kForward>(v, l.t_, count...)
             : lookup<Arm, n::direction::kBackward>(v, l.t_, count...);
}

// Design C: one entry per (stop, offer). Bit d of days_ means the window
// [from_ + d days, to_ + d days) runs, with ride duration duration_.
struct compact_offer {
  n::unixtime_t from_, to_;
  n::duration_t duration_;
  std::uint64_t days_;
};
using compact_t = std::vector<compact_offer>;

constexpr auto const kDay = n::i32_minutes{1440};

template <n::direction Dir, typename... Count>
std::optional<n::duration_t> lookup_c(compact_t const& offers,
                                      n::unixtime_t const t,
                                      Count&... count) {
  auto best = std::optional<n::duration_t>{};
  for (auto const& o : offers) {
    (count(), ...);
    if constexpr (Dir == n::direction::kForward) {
      // first running day whose window has not closed at t
      auto const d_min =
          t < o.to_ ? 0 : static_cast<int>((t - o.to_) / kDay) + 1;
      if (d_min >= 64) {
        continue;
      }
      auto const later = o.days_ >> d_min;
      if (later == 0U) {
        continue;
      }
      auto const d = d_min + std::countr_zero(later);
      auto const dep = std::max(o.from_ + d * kDay, t);
      auto const dur = dep + o.duration_ - t;
      if (dur > n::routing::kMaxTravelTime) {
        continue;
      }
      if (!best.has_value() || dur < *best) {
        best = n::duration_t{dur.count()};
      }
    } else {
      // latest running day that opens before t - duration; within it the
      // latest departure is min(t - duration, window end - 1 min)
      auto const x = t - o.duration_;
      if (x < o.from_) {
        continue;
      }
      auto const d_max = std::min(static_cast<int>((x - o.from_) / kDay), 63);
      auto const mask =
          d_max == 63 ? o.days_ : o.days_ & ((std::uint64_t{2} << d_max) - 1U);
      if (mask == 0U) {
        continue;
      }
      auto const d = 63 - std::countl_zero(mask);
      auto const latest =
          std::min(x, o.to_ + d * kDay - n::i32_minutes{1});
      auto const dur = t - latest;
      if (dur > n::routing::kMaxTravelTime) {
        continue;
      }
      if (!best.has_value() || dur < *best) {
        best = n::duration_t{dur.count()};
      }
    }
  }
  return best;
}

template <typename... Count>
std::optional<n::duration_t> lookup_c(compact_t const& offers,
                                      td_trace_lookup const& l,
                                      Count&... count) {
  return static_cast<n::direction>(l.dir_) == n::direction::kForward
             ? lookup_c<n::direction::kForward>(offers, l.t_, count...)
             : lookup_c<n::direction::kBackward>(offers, l.t_, count...);
}

using steady = std::chrono::steady_clock;

std::uint64_t ns_since(steady::time_point const start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(steady::now() -
                                                           start)
          .count());
}

// Minimum over `reps` runs of all of a query's lookups, back to back.
template <arm Arm>
std::uint64_t time_lookups(std::vector<offsets_t> const& seqs,
                           std::vector<td_trace_lookup> const& lookups,
                           unsigned const reps,
                           std::int64_t& sink) {
  auto best = std::numeric_limits<std::uint64_t>::max();
  for (auto rep = 0U; rep != reps; ++rep) {
    auto const start = steady::now();
    auto sum = std::int64_t{0};
    for (auto const& l : lookups) {
      auto const r = lookup<Arm>(seqs[l.seq_], l);
      sum += r.has_value() ? r->count() : -1;
    }
    best = std::min(best, ns_since(start));
    sink += sum;
  }
  return best;
}

// Ski rental: a stop's first k lookups scan D's raw windows, later ones use A.
// Normalization is not in here; it is charged per switched stop by the caller.
template <typename... Count>
std::optional<n::duration_t> ski_lookup(std::vector<offsets_t> const& d,
                                        std::vector<offsets_t> const& a,
                                        std::vector<std::uint32_t>& seen,
                                        unsigned const k,
                                        td_trace_lookup const& l,
                                        Count&... count) {
  return seen[l.seq_]++ < k ? lookup<kD>(d[l.seq_], l, count...)
                            : lookup<kA>(a[l.seq_], l, count...);
}

std::uint64_t time_c(std::vector<compact_t> const& c,
                     std::vector<td_trace_lookup> const& lookups,
                     unsigned const reps,
                     std::int64_t& sink) {
  auto best = std::numeric_limits<std::uint64_t>::max();
  for (auto rep = 0U; rep != reps; ++rep) {
    auto const start = steady::now();
    auto sum = std::int64_t{0};
    for (auto const& l : lookups) {
      auto const r = lookup_c(c[l.seq_], l);
      sum += r.has_value() ? r->count() : -1;
    }
    best = std::min(best, ns_since(start));
    sink += sum;
  }
  return best;
}

std::uint64_t time_ski(std::vector<offsets_t> const& d,
                       std::vector<offsets_t> const& a,
                       std::vector<td_trace_lookup> const& lookups,
                       unsigned const k,
                       unsigned const reps,
                       std::int64_t& sink) {
  auto best = std::numeric_limits<std::uint64_t>::max();
  auto seen = std::vector<std::uint32_t>(d.size());
  for (auto rep = 0U; rep != reps; ++rep) {
    std::fill(begin(seen), end(seen), 0U);
    auto const start = steady::now();
    auto sum = std::int64_t{0};
    for (auto const& l : lookups) {
      auto const r = ski_lookup(d, a, seen, k, l);
      sum += r.has_value() ? r->count() : -1;
    }
    best = std::min(best, ns_since(start));
    sink += sum;
  }
  return best;
}

template <typename Fn>
std::uint64_t time_min(unsigned const reps, Fn&& fn) {
  auto best = std::numeric_limits<std::uint64_t>::max();
  for (auto rep = 0U; rep != reps; ++rep) {
    auto const start = steady::now();
    fn();
    best = std::min(best, ns_since(start));
  }
  return best;
}

struct query_result {
  std::size_t n_windows_{}, n_lookups_{};
  std::size_t n_missing_windows_{}, n_a_check_fail_{}, n_unchecked_{};
  std::size_t n_seqs_looked_up_{}, n_seqs_switched_{};
  std::array<std::size_t, kNArms> entries_{};
  std::uint64_t build_ns_d0_{}, build_ns_d_{}, norm_ns_a_{}, norm_ns_b_{};
  std::array<std::uint64_t, kNArms> lookup_ns_{}, visits_{};
  std::array<std::uint64_t, kNArms> wrong_{};  // lookups disagreeing with A
  std::array<std::uint64_t, kNArms> beyond_max_{};  // ... only past the limit
  std::uint64_t norm_ns_lazy_{}, norm_ns_ski_{}, lookup_ns_ski_{};
  std::uint64_t visits_ski_{}, wrong_ski_{};
  std::size_t entries_c_{}, c_fallback_{};
  std::uint64_t build_ns_c_{}, lookup_ns_c_{}, visits_c_{}, wrong_c_{},
      beyond_max_c_{};
};

void print_seq(char const* name, offsets_t const& v) {
  fmt::print("    {:>2}:", name);
  for (auto const& o : v) {
    if (o.duration_ == n::footpath::kMaxDuration) {
      fmt::print(" [{} inf]", o.valid_from_);
    } else {
      fmt::print(" [{} {} m{}]", o.valid_from_, o.duration_,
                 o.transport_mode_payload_);
    }
  }
  fmt::print("\n");
}

query_result replay(td_trace const& tr,
                    unsigned const reps,
                    std::size_t const rotation,
                    std::int64_t& sink,
                    unsigned& n_dump,
                    unsigned const ski_k) {
  auto res = query_result{};
  res.n_windows_ = tr.windows_.size();
  res.n_lookups_ = tr.lookups_.size();

  // Producer windows per (osr direction, stop), in production order.
  auto windows = std::map<std::pair<std::uint8_t, n::location_idx_t>,
                          std::vector<td_trace_window const*>>{};
  for (auto const& w : tr.windows_) {
    windows[{w.dir_, w.location_}].push_back(&w);
  }
  auto const osr_dir = [&](std::uint8_t const side) {
    auto const fwd = (side == 0U) != tr.arrive_by_;
    return static_cast<std::uint8_t>(fwd ? osr::direction::kForward
                                         : osr::direction::kBackward);
  };
  auto const producer_windows = [&](td_trace_seq const& s)
      -> std::vector<td_trace_window const*> const* {
    auto const it = windows.find({osr_dir(s.side_), s.location_});
    return it == end(windows) ? nullptr : &it->second;
  };

  auto const n_seqs = tr.seqs_.size();
  auto arms = std::array<std::vector<offsets_t>, kNArms>{};
  for (auto& a : arms) {
    a.resize(n_seqs);
  }

  auto const build_d0 = [&] {
    for (auto i = 0U; i != n_seqs; ++i) {
      auto& v = arms[kD0][i];
      v.clear();
      if (auto const* const ws = producer_windows(tr.seqs_[i])) {
        for (auto const* w : *ws) {
          v.push_back(n::routing::td_offset::make(w->from_, w->duration_,
                                                  w->mode_));
          v.push_back(n::routing::td_offset::make(
              w->to_, n::footpath::kMaxDuration, w->mode_));
        }
      }
    }
  };
  auto const build_d = [&] {
    for (auto i = 0U; i != n_seqs; ++i) {
      auto& v = arms[kD][i];
      v.clear();
      if (auto const* const ws = producer_windows(tr.seqs_[i])) {
        for (auto const* w : *ws) {
          add_td_window(v, n::interval{w->from_, w->to_}, w->duration_,
                        w->mode_);
        }
      }
    }
  };
  auto const normalize = [&](arm const target, bool const fifo) {
    fifo_repair_enabled() = fifo;
    for (auto i = 0U; i != n_seqs; ++i) {
      arms[target][i] = arms[kD][i];
    }
    auto const start = steady::now();
    for (auto& v : arms[target]) {
      normalize_td_offsets(v, nullptr);
    }
    auto const ns = ns_since(start);
    fifo_repair_enabled() = true;
    return ns;
  };

  res.build_ns_d0_ = time_min(reps, build_d0);
  res.build_ns_d_ = time_min(reps, build_d);
  res.norm_ns_a_ = std::numeric_limits<std::uint64_t>::max();
  res.norm_ns_b_ = std::numeric_limits<std::uint64_t>::max();
  for (auto rep = 0U; rep != reps; ++rep) {
    res.norm_ns_a_ = std::min(res.norm_ns_a_, normalize(kA, true));
    res.norm_ns_b_ = std::min(res.norm_ns_b_, normalize(kB, false));
  }

  for (auto const& s : tr.seqs_) {
    if (producer_windows(s) == nullptr) {
      ++res.n_missing_windows_;
    }
  }

  // Design C, rebuilt from the windows. Only the rebuilt template is kept;
  // what a query would really do per stop -- copy the offers and set the
  // routed duration -- is what build_ns_c_ times.
  auto c_template = std::vector<compact_t>(n_seqs);
  for (auto i = 0U; i != n_seqs; ++i) {
    auto const* const ws = producer_windows(tr.seqs_[i]);
    if (ws == nullptr) {
      continue;
    }
    struct key {
      auto operator<=>(key const&) const = default;
      n::routing::transport_mode_t::mode_t mode_;
      n::routing::transport_mode_t::payload_t payload_;
      n::duration_t::rep duration_;
      std::int32_t length_, phase_;
    };
    auto by_key = std::map<key, std::size_t>{};
    auto& offers = c_template[i];
    for (auto const* w : *ws) {
      auto const from = w->from_.time_since_epoch().count();
      auto const k = key{w->mode_.mode_, w->mode_.payload_,
                         w->duration_.count(),
                         (w->to_ - w->from_).count(),
                         ((from % 1440) + 1440) % 1440};
      if (auto const it = by_key.find(k); it != end(by_key)) {
        auto& o = offers[it->second];
        auto const d = (w->from_ - o.from_) / kDay;
        if (w->from_ >= o.from_ && d < 64) {
          o.days_ |= std::uint64_t{1} << d;
          continue;
        }
      }
      if (!by_key.contains(k)) {
        by_key.emplace(k, offers.size());
      } else {
        ++res.c_fallback_;  // not a day-shifted copy of its group's first
      }
      offers.push_back(compact_offer{.from_ = w->from_,
                                     .to_ = w->to_,
                                     .duration_ = w->duration_,
                                     .days_ = 1U});
    }
  }
  auto arm_c = std::vector<compact_t>(n_seqs);
  res.build_ns_c_ = time_min(reps, [&] {
    for (auto i = 0U; i != n_seqs; ++i) {
      arm_c[i].clear();
      for (auto const& o : c_template[i]) {
        arm_c[i].push_back(o);  // + the routed duration, already in o
      }
    }
  });

  // Pruning, as remove_slower_than_fastest_direct would do it on C: the
  // duration does not depend on time, so a losing offer goes as a whole.
  if (tr.fastest_direct_.has_value()) {
    auto const kNone =
        n::duration_t{std::numeric_limits<n::duration_t::rep>::max()};
    auto min_side = std::array{tr.min_start_plain_, tr.min_dest_plain_};
    for (auto i = 0U; i != n_seqs; ++i) {
      for (auto const& o : arm_c[i]) {
        auto& m = min_side[tr.seqs_[i].side_];
        m = std::min(m, o.duration_);
      }
    }
    for (auto i = 0U; i != n_seqs; ++i) {
      auto const other = min_side[1U - tr.seqs_[i].side_];
      if (other == kNone) {
        continue;
      }
      std::erase_if(arm_c[i], [&](compact_offer const& o) {
        return o.duration_ + other >= *tr.fastest_direct_;
      });
    }
  }
  for (auto const& c : arm_c) {
    res.entries_c_ += c.size();
  }

  // Each stop's share of A's normalization, timed one stop at a time and
  // scaled to the batch total above (removes the per-call timer overhead).
  auto seq_norm_ns = std::vector<std::uint64_t>(n_seqs,
                                                std::numeric_limits<std::uint64_t>::max());
  {
    auto tmp = offsets_t{};
    for (auto rep = 0U; rep != reps; ++rep) {
      for (auto i = 0U; i != n_seqs; ++i) {
        tmp = arms[kD][i];
        auto const start = steady::now();
        normalize_td_offsets(tmp, nullptr);
        seq_norm_ns[i] = std::min(seq_norm_ns[i], ns_since(start));
      }
    }
  }
  auto n_lookups_of = std::vector<std::uint32_t>(n_seqs);
  for (auto const& l : tr.lookups_) {
    ++n_lookups_of[l.seq_];
  }
  auto seq_norm_total = 0.0, lazy = 0.0, switched = 0.0;
  for (auto i = 0U; i != n_seqs; ++i) {
    auto const ns = static_cast<double>(seq_norm_ns[i]);
    seq_norm_total += ns;
    if (n_lookups_of[i] != 0U) {
      lazy += ns;
      ++res.n_seqs_looked_up_;
    }
    if (n_lookups_of[i] > ski_k) {  // the (k+1)-th lookup triggers the switch
      switched += ns;
      ++res.n_seqs_switched_;
    }
  }
  if (seq_norm_total > 0.0) {
    auto const scale = static_cast<double>(res.norm_ns_a_) / seq_norm_total;
    res.norm_ns_lazy_ = static_cast<std::uint64_t>(lazy * scale);
    res.norm_ns_ski_ = static_cast<std::uint64_t>(switched * scale);
  }

  // Same pruning as the search: the plain offsets only enter via their minimum.
  auto const kNoOffset =
      n::duration_t{std::numeric_limits<n::duration_t::rep>::max()};
  for (auto& a : arms) {
    auto q = n::routing::query{};
    q.fastest_direct_ = tr.fastest_direct_;
    if (tr.min_start_plain_ != kNoOffset) {
      q.start_.emplace_back(n::location_idx_t{0U}, tr.min_start_plain_, 0U);
    }
    if (tr.min_dest_plain_ != kNoOffset) {
      q.destination_.emplace_back(n::location_idx_t{0U}, tr.min_dest_plain_,
                                  0U);
    }
    for (auto i = 0U; i != n_seqs; ++i) {
      auto& side = tr.seqs_[i].side_ == 0U ? q.td_start_ : q.td_dest_;
      side[tr.seqs_[i].location_] = std::move(a[i]);
    }
    ep::remove_slower_than_fastest_direct(q);
    for (auto i = 0U; i != n_seqs; ++i) {
      auto& side = tr.seqs_[i].side_ == 0U ? q.td_start_ : q.td_dest_;
      a[i] = std::move(side[tr.seqs_[i].location_]);
    }
  }

  for (auto i = 0U; i != n_seqs; ++i) {
    if (tr.seqs_[i].offsets_.empty()) {
      ++res.n_unchecked_;  // synthetic trace: nothing recorded to compare to
    } else if (arms[kA][i] != tr.seqs_[i].offsets_) {
      ++res.n_a_check_fail_;
    }
    for (auto a = 0U; a != kNArms; ++a) {
      res.entries_[a] += arms[a][i].size();
    }
  }

  // Entries visited, and agreement with A, in one untimed pass per arm.
  auto const count_arm = [&]<arm Arm>() {
    auto c = counter{};
    for (auto const& l : tr.lookups_) {
      auto const r = lookup<Arm>(arms[Arm][l.seq_], l, c);
      if constexpr (Arm != kA) {
        if (auto const a = lookup<kA>(arms[kA][l.seq_], l); r != a) {
          // A cuts off on the arrival slack, B/D on the departure slack; they
          // only differ for legs longer than kMaxTravelTime, which no search
          // can use.
          auto const beyond = [](std::optional<n::duration_t> const& x) {
            return !x.has_value() || *x > n::routing::kMaxTravelTime;
          };
          if (beyond(a) && beyond(r)) {
            ++res.beyond_max_[Arm];
            continue;
          }
          ++res.wrong_[Arm];
          if (n_dump != 0U) {
            --n_dump;
            auto const str = [](std::optional<n::duration_t> const& x) {
              return x.has_value() ? fmt::format("{}", *x)
                                   : std::string{"none"};
            };
            fmt::println("  {} vs A: side={} dir={} t={}  A={} {}={}",
                         kArmName[Arm], tr.seqs_[l.seq_].side_,
                         l.dir_ == 0U ? "fwd" : "bwd", l.t_, str(a),
                         kArmName[Arm], str(r));
            print_seq("A", arms[kA][l.seq_]);
            print_seq(kArmName[Arm], arms[Arm][l.seq_]);
          }
        }
      }
    }
    res.visits_[Arm] = c.n_;
  };
  count_arm.operator()<kA>();
  count_arm.operator()<kB>();
  count_arm.operator()<kD>();
  count_arm.operator()<kD0>();
  {
    auto c = counter{};
    auto seen = std::vector<std::uint32_t>(n_seqs);
    for (auto const& l : tr.lookups_) {
      auto const r = ski_lookup(arms[kD], arms[kA], seen, ski_k, l, c);
      auto const a = lookup<kA>(arms[kA][l.seq_], l);
      auto const beyond = [](std::optional<n::duration_t> const& x) {
        return !x.has_value() || *x > n::routing::kMaxTravelTime;
      };
      if (r != a && !(beyond(a) && beyond(r))) {
        ++res.wrong_ski_;
      }
    }
    res.visits_ski_ = c.n_;
  }
  {
    auto c = counter{};
    for (auto const& l : tr.lookups_) {
      auto const r = lookup_c(arm_c[l.seq_], l, c);
      auto const a = lookup<kA>(arms[kA][l.seq_], l);
      if (r == a) {
        continue;
      }
      auto const beyond = [](std::optional<n::duration_t> const& x) {
        return !x.has_value() || *x > n::routing::kMaxTravelTime;
      };
      if (beyond(a) && beyond(r)) {
        ++res.beyond_max_c_;
        continue;
      }
      ++res.wrong_c_;
      if (n_dump != 0U) {
        --n_dump;
        auto const str = [](std::optional<n::duration_t> const& x) {
          return x.has_value() ? fmt::format("{}", *x) : std::string{"none"};
        };
        fmt::println("  C vs A: side={} dir={} t={}  A={} C={}",
                     tr.seqs_[l.seq_].side_, l.dir_ == 0U ? "fwd" : "bwd",
                     l.t_, str(a), str(r));
        print_seq("A", arms[kA][l.seq_]);
        for (auto const& o : arm_c[l.seq_]) {
          fmt::println("     C: [{}, {}) {} days={:b}", o.from_, o.to_,
                       o.duration_, o.days_);
        }
      }
    }
    res.visits_c_ = c.n_;
  }

  // Rotate the order across queries so no arm always runs first (cold) or
  // last.
  for (auto k = 0U; k != kNArms + 1U; ++k) {
    switch ((k + rotation) % (kNArms + 1U)) {
      case kA:
        res.lookup_ns_[kA] =
            time_lookups<kA>(arms[kA], tr.lookups_, reps, sink);
        break;
      case kB:
        res.lookup_ns_[kB] =
            time_lookups<kB>(arms[kB], tr.lookups_, reps, sink);
        break;
      case kD:
        res.lookup_ns_[kD] =
            time_lookups<kD>(arms[kD], tr.lookups_, reps, sink);
        break;
      case kD0:
        res.lookup_ns_[kD0] =
            time_lookups<kD0>(arms[kD0], tr.lookups_, reps, sink);
        break;
      default:
        res.lookup_ns_c_ = time_c(arm_c, tr.lookups_, reps, sink);
        break;
    }
  }
  res.lookup_ns_ski_ =
      time_ski(arms[kD], arms[kA], tr.lookups_, ski_k, reps, sink);
  return res;
}

}  // namespace

int td_replay(int ac, char** av) {
  auto trace_path = fs::path{"td-trace"};
  auto out_path = fs::path{"td-replay.csv"};
  auto reps = 5U;
  auto n_dump = 0U;
  auto ski_k = 32U;
  auto desc = po::options_description{"Options"};
  desc.add_options()  //
      ("help", "Prints this help message")  //
      ("trace,t", po::value(&trace_path)->default_value(trace_path),
       "directory written with MOTIS_TD_TRACE")  //
      ("out,o", po::value(&out_path)->default_value(out_path),
       "per-query CSV")  //
      ("reps", po::value(&reps)->default_value(reps),
       "repetitions per timing, the minimum is reported")  //
      ("dump", po::value(&n_dump)->default_value(n_dump),
       "print the first N lookups on which an arm disagrees with A")  //
      ("ski-k", po::value(&ski_k)->default_value(ski_k),
       "ski rental: lookups per stop answered from raw windows before the stop "
       "is normalized");
  auto vm = parse_opt(ac, av, desc);
  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }
  if (auto const* const e = std::getenv("MOTIS_TD_RAW");
      e != nullptr && std::string_view{e} == "1") {
    std::cerr << "unset MOTIS_TD_RAW: it would skip normalizing A and B\n";
    return 1;
  }

  auto files = std::vector<fs::path>{};
  for (auto const& e : fs::directory_iterator{trace_path}) {
    if (e.path().extension() == ".bin") {
      files.push_back(e.path());
    }
  }
  std::sort(begin(files), end(files));

  auto out = std::ofstream{out_path};
  out << "file,url,n_seqs,n_windows,n_lookups,n_unmatched,missing_windows,"
         "a_check_fail,entries_D0,entries_D,entries_A,entries_B,"
         "build_ns_D0,build_ns_D,norm_ns_A,norm_ns_B,"
         "lookup_ns_A,lookup_ns_B,lookup_ns_D,lookup_ns_D0,"
         "visits_A,visits_B,visits_D,visits_D0,wrong_B,wrong_D,wrong_D0,"
         "beyond_max_B,beyond_max_D,beyond_max_D0,"
         "unchecked,seqs_looked_up,seqs_switched,ski_k,"
         "norm_ns_A_lazy,norm_ns_ski,lookup_ns_ski,visits_ski,wrong_ski,"
         "entries_C,c_fallback,build_ns_C,lookup_ns_C,visits_C,wrong_C,"
         "beyond_max_C\n";

  auto sink = std::int64_t{0};
  auto total = query_result{};
  auto n_arrive_by = 0U;
  auto n_unmatched = std::uint64_t{0};
  for (auto const [i, path] : utl::enumerate(files)) {
    auto const tr = td_trace::read(path);
    if (tr.arrive_by_) {
      ++n_arrive_by;  // not recorded by the eval sets; replay assumes forward
    }
    auto const r = replay(tr, reps, i, sink, n_dump, ski_k);
    n_unmatched += tr.n_unmatched_;

    out << fmt::format(
        "{},\"{}\",{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
        "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
        path.filename().string(), tr.url_, tr.seqs_.size(), r.n_windows_,
        r.n_lookups_, tr.n_unmatched_, r.n_missing_windows_,
        r.n_a_check_fail_, r.entries_[kD0], r.entries_[kD], r.entries_[kA],
        r.entries_[kB], r.build_ns_d0_, r.build_ns_d_, r.norm_ns_a_,
        r.norm_ns_b_, r.lookup_ns_[kA], r.lookup_ns_[kB], r.lookup_ns_[kD],
        r.lookup_ns_[kD0], r.visits_[kA], r.visits_[kB], r.visits_[kD],
        r.visits_[kD0], r.wrong_[kB], r.wrong_[kD], r.wrong_[kD0],
        r.beyond_max_[kB], r.beyond_max_[kD], r.beyond_max_[kD0],
        r.n_unchecked_, r.n_seqs_looked_up_, r.n_seqs_switched_, ski_k,
        r.norm_ns_lazy_, r.norm_ns_ski_, r.lookup_ns_ski_, r.visits_ski_,
        r.wrong_ski_, r.entries_c_, r.c_fallback_, r.build_ns_c_,
        r.lookup_ns_c_, r.visits_c_, r.wrong_c_, r.beyond_max_c_);

    total.n_windows_ += r.n_windows_;
    total.n_lookups_ += r.n_lookups_;
    total.n_missing_windows_ += r.n_missing_windows_;
    total.n_a_check_fail_ += r.n_a_check_fail_;
    total.build_ns_d0_ += r.build_ns_d0_;
    total.build_ns_d_ += r.build_ns_d_;
    total.norm_ns_a_ += r.norm_ns_a_;
    total.norm_ns_b_ += r.norm_ns_b_;
    total.norm_ns_lazy_ += r.norm_ns_lazy_;
    total.norm_ns_ski_ += r.norm_ns_ski_;
    total.lookup_ns_ski_ += r.lookup_ns_ski_;
    total.visits_ski_ += r.visits_ski_;
    total.wrong_ski_ += r.wrong_ski_;
    total.n_unchecked_ += r.n_unchecked_;
    total.entries_c_ += r.entries_c_;
    total.c_fallback_ += r.c_fallback_;
    total.build_ns_c_ += r.build_ns_c_;
    total.lookup_ns_c_ += r.lookup_ns_c_;
    total.visits_c_ += r.visits_c_;
    total.wrong_c_ += r.wrong_c_;
    total.beyond_max_c_ += r.beyond_max_c_;
    for (auto a = 0U; a != kNArms; ++a) {
      total.entries_[a] += r.entries_[a];
      total.lookup_ns_[a] += r.lookup_ns_[a];
      total.visits_[a] += r.visits_[a];
      total.wrong_[a] += r.wrong_[a];
      total.beyond_max_[a] += r.beyond_max_[a];
    }
  }

  fmt::println("queries={} arrive_by={} windows={} lookups={} unmatched={}",
               files.size(), n_arrive_by, total.n_windows_, total.n_lookups_,
               n_unmatched);
  fmt::println("checks: stops_without_windows={} A_rebuilt_differs={} "
               "unchecked={}",
               total.n_missing_windows_, total.n_a_check_fail_,
               total.n_unchecked_);
  fmt::println("build: D0 pairs {:.1f} ms, D merge {:.1f} ms, "
               "normalize A {:.1f} ms, normalize B {:.1f} ms",
               total.build_ns_d0_ / 1e6, total.build_ns_d_ / 1e6,
               total.norm_ns_a_ / 1e6, total.norm_ns_b_ / 1e6);
  for (auto a = 0U; a != kNArms; ++a) {
    fmt::println("{:>2}: entries={:>12} lookups {:>9.1f} ms  visits={:>14} "
                 "({:.2f}/lookup)  wrong_vs_A={} beyond_max={}",
                 kArmName[a], total.entries_[a], total.lookup_ns_[a] / 1e6,
                 total.visits_[a],
                 total.n_lookups_ == 0U
                     ? 0.0
                     : static_cast<double>(total.visits_[a]) /
                           static_cast<double>(total.n_lookups_),
                 total.wrong_[a], total.beyond_max_[a]);
  }
  auto const ms = [](std::uint64_t const ns) { return ns / 1e6; };
  fmt::println("per design, normalization + lookups:");
  fmt::println("  A eager {:>9.1f} ms = {:.1f} + {:.1f}",
               ms(total.norm_ns_a_ + total.lookup_ns_[kA]),
               ms(total.norm_ns_a_), ms(total.lookup_ns_[kA]));
  fmt::println("  A lazy  {:>9.1f} ms = {:.1f} + {:.1f}",
               ms(total.norm_ns_lazy_ + total.lookup_ns_[kA]),
               ms(total.norm_ns_lazy_), ms(total.lookup_ns_[kA]));
  fmt::println("  ski k={} {:>7.1f} ms = {:.1f} + {:.1f}  (wrong_vs_A={})",
               ski_k, ms(total.norm_ns_ski_ + total.lookup_ns_ski_),
               ms(total.norm_ns_ski_), ms(total.lookup_ns_ski_),
               total.wrong_ski_);
  fmt::println("  B       {:>9.1f} ms = {:.1f} + {:.1f}",
               ms(total.norm_ns_b_ + total.lookup_ns_[kB]),
               ms(total.norm_ns_b_), ms(total.lookup_ns_[kB]));
  fmt::println("  D       {:>9.1f} ms = 0 + {:.1f}", ms(total.lookup_ns_[kD]),
               ms(total.lookup_ns_[kD]));
  fmt::println("  C       {:>9.1f} ms = 0 + {:.1f}  (entries={} "
               "visits={:.2f}/lookup wrong_vs_A={} beyond_max={} "
               "fallback_windows={} build {:.1f} ms vs D merge {:.1f} ms)",
               ms(total.lookup_ns_c_), ms(total.lookup_ns_c_),
               total.entries_c_,
               total.n_lookups_ == 0U
                   ? 0.0
                   : static_cast<double>(total.visits_c_) /
                         static_cast<double>(total.n_lookups_),
               total.wrong_c_, total.beyond_max_c_, total.c_fallback_,
               ms(total.build_ns_c_), ms(total.build_ns_d_));
  fmt::println("(sink {})", sink);
  return 0;
}

}  // namespace motis
