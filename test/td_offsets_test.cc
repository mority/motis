#include "gtest/gtest.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <random>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "nigiri/footpath.h"
#include "nigiri/td_footpath.h"

#include "motis/endpoints/routing.h"
#include "motis/flex/mode_payload.h"
#include "motis/td_offsets.h"

namespace n = nigiri;

namespace {

n::unixtime_t t(int const minutes) {
  return n::unixtime_t{n::i32_minutes{minutes}};
}

n::routing::transport_mode_t::payload_t flex_payload(
    n::flex_transport_idx_t::value_t const transport,
    n::stop_idx_t const stop,
    osr::direction const dir) {
  return motis::flex::mode_payload{n::flex_transport_idx_t{transport}, stop,
                                   dir}
      .to_payload();
}

struct offer {
  int from_, to_;
  n::duration_t duration_;
  n::routing::transport_mode_t::payload_t mode_;
};

// Builds the raw input as the producers do: every offer is added as a window
// via add_td_window.
std::vector<n::routing::td_offset> raw(std::initializer_list<offer> offers) {
  auto v = std::vector<n::routing::td_offset>{};
  for (auto const& o : offers) {
    motis::add_td_window(v, n::interval{t(o.from_), t(o.to_)}, o.duration_,
                         {.payload_ = o.mode_});
  }
  return v;
}

n::routing::td_offset active(
    int const from,
    int const duration,
    n::routing::transport_mode_t::payload_t const mode) {
  return {.valid_from_ = t(from),
          .duration_ = n::duration_t{duration},
          .transport_mode_payload_ = mode};
}

n::routing::td_offset inactive(int const from) {
  return {.valid_from_ = t(from),
          .duration_ = n::footpath::kMaxDuration,
          .transport_mode_payload_ = 0U};
}

// Raw end of a window of `mode`, as added by add_td_window.
n::routing::td_offset closer(
    int const from, n::routing::transport_mode_t::payload_t const mode) {
  return {.valid_from_ = t(from),
          .duration_ = n::footpath::kMaxDuration,
          .transport_mode_payload_ = mode};
}

// Arrival time the routing core (nigiri's get_td_duration) yields for a
// departure at minute `dep`; std::nullopt if no offer is reachable.
std::optional<n::unixtime_t> arrival(
    std::vector<n::routing::td_offset> const& offsets, int const dep) {
  auto const r = n::get_td_duration<n::direction::kForward>(
      std::span<n::routing::td_offset const>{offsets}, t(dep));
  return r.has_value() ? std::optional{t(dep) + r->first} : std::nullopt;
}


// alpha_tilde of ONE well-formed sequence, straight from the definition:
//   alpha(tau) = min_{t >= tau} ( t + l_{ehat(t)} ),  ehat(t) = max{e : tau_e <= t}
// Valid only for a sequence with strictly increasing tau_e, which a single
// provider's disjoint windows satisfy by construction.
std::optional<n::unixtime_t> alpha_brute(
    std::vector<n::routing::td_offset> const& seq,
    int const dep,
    int const horizon) {
  auto best = std::optional<n::unixtime_t>{};
  for (auto m = dep; m <= horizon; ++m) {
    auto const now = t(m);
    auto e = seq.end();
    for (auto i = seq.begin(); i != seq.end() && i->valid_from_ <= now; ++i) {
      e = i;
    }
    if (e == seq.end() || e->duration_ == n::footpath::kMaxDuration) {
      continue;
    }
    auto const arr = now + e->duration_;
    if (!best.has_value() || arr < *best) {
      best = arr;
    }
  }
  return best;
}

std::optional<n::unixtime_t> scan_arrival(
    std::vector<n::routing::td_offset> const& offsets, int const dep) {
  auto const r = n::get_td_duration_scan<n::direction::kForward>(
      std::span<n::routing::td_offset const>{offsets}, t(dep));
  return r.has_value() ? std::optional{t(dep) + r->first} : std::nullopt;
}


// Backward alpha_tilde: to a given arrival deadline, the LATEST departure that
// still meets it (Primitive-Formeln.md: "Sie liefert zu einer gegebenen
// Ankunftszeit die spaeteste Abfahrt, mit der diese noch eingehalten wird").
// Returned as a duration, i.e. arrival - departure.
std::optional<n::duration_t> alpha_brute_bwd(
    std::vector<n::routing::td_offset> const& seq,
    int const arr,
    int const horizon) {
  auto best = std::optional<n::duration_t>{};
  for (auto m = 0; m <= horizon; ++m) {
    auto const dep = t(m);
    auto e = seq.end();
    for (auto i = seq.begin(); i != seq.end() && i->valid_from_ <= dep; ++i) {
      e = i;
    }
    if (e == seq.end() || e->duration_ == n::footpath::kMaxDuration) {
      continue;
    }
    if (dep + e->duration_ > t(arr)) {
      continue;  // misses the deadline
    }
    auto const d = t(arr) - dep;          // waiting at the destination counts
    if (!best.has_value() || d < *best) {  // latest departure == smallest span
      best = d;
    }
  }
  return best;
}

std::optional<n::duration_t> fast_bwd(
    std::vector<n::routing::td_offset> const& seq, int const arr) {
  auto const r = n::get_td_duration<n::direction::kBackward>(
      std::span<n::routing::td_offset const>{seq}, t(arr));
  return r.has_value() ? std::optional{r->first} : std::nullopt;
}

std::optional<n::duration_t> scan_bwd(
    std::vector<n::routing::td_offset> const& seq, int const arr) {
  auto const r = n::get_td_duration_scan<n::direction::kBackward>(
      std::span<n::routing::td_offset const>{seq}, t(arr));
  return r.has_value() ? std::optional{r->first} : std::nullopt;
}


// Design D evaluated on the RAW vector (no envelope, no repair, no sort).
std::optional<n::unixtime_t> raw_fwd(
    std::vector<n::routing::td_offset> const& raw, int const dep) {
  auto const r = n::get_td_duration_raw_windows<n::direction::kForward>(
      std::span<n::routing::td_offset const>{raw}, t(dep));
  return r.has_value() ? std::optional{t(dep) + r->first} : std::nullopt;
}

std::optional<n::duration_t> raw_bwd(
    std::vector<n::routing::td_offset> const& raw, int const arr) {
  auto const r = n::get_td_duration_raw_windows<n::direction::kBackward>(
      std::span<n::routing::td_offset const>{raw}, t(arr));
  return r.has_value() ? std::optional{r->first} : std::nullopt;
}

}  // namespace

TEST(motis, td_offsets_keep_shortest_same_window) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 200, n::duration_t{30}, slow},
      {100, 200, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(offsets);

  EXPECT_EQ((std::vector{inactive(0), active(100, 10, fast), inactive(200)}),
            offsets);
}

TEST(motis, td_offsets_deterministic_on_equal_duration) {
  auto const first = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const second = flex_payload(2U, 0U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 200, n::duration_t{10}, first},
      {100, 200, n::duration_t{10}, second},
  });
  motis::normalize_td_offsets(offsets);

  EXPECT_EQ((std::vector{inactive(0), active(100, 10, std::min(first, second)),
                         inactive(200)}),
            offsets);
}

TEST(motis, td_offsets_split_overlapping_windows) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 220, n::duration_t{30}, slow},
      {150, 180, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(offsets);

  // The slow offer is ended early at 150 + 10 - 30 = 130: departing later than
  // that, it is faster to wait for the fast offer (FIFO repair). Without it,
  // departing at 149 (arrival 179) would beat departing at 150 (arrival 160).
  EXPECT_EQ((std::vector{inactive(0), active(100, 30, slow), inactive(130),
                         active(150, 10, fast), active(180, 30, slow),
                         inactive(220)}),
            offsets);

  // get_td_duration must yield FIFO arrivals. [130, 150) is the cut region
  // where the slow offer would otherwise let an earlier departure overtake:
  EXPECT_EQ(t(130), arrival(offsets, 100));
  EXPECT_EQ(t(159), arrival(offsets, 129));
  for (auto dep = 130; dep < 150; ++dep) {
    EXPECT_EQ(t(160), arrival(offsets, dep)) << "at minute " << dep;
  }
  EXPECT_EQ(t(160), arrival(offsets, 150));
  EXPECT_EQ(t(189), arrival(offsets, 179));
  EXPECT_EQ(t(210), arrival(offsets, 180));
  EXPECT_EQ(t(249), arrival(offsets, 219));
}

TEST(motis, td_offsets_fifo_cascade) {
  auto const a = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const b = flex_payload(2U, 0U, osr::direction::kBackward);
  auto const c = flex_payload(3U, 0U, osr::direction::kBackward);

  // Three overlapping offers, each faster than the previous: the FIFO repair
  // cuts `a` relative to `b` (at 200 + 60 - 90 = 170) and `b` relative to `c`
  // (at 300 + 10 - 60 = 250).
  auto offsets = raw({
      {100, 400, n::duration_t{90}, a},
      {200, 400, n::duration_t{60}, b},
      {300, 400, n::duration_t{10}, c},
  });
  motis::normalize_td_offsets(offsets);

  EXPECT_EQ((std::vector{inactive(0), active(100, 90, a), inactive(170),
                         active(200, 60, b), inactive(250), active(300, 10, c),
                         inactive(400)}),
            offsets);

  // get_td_duration must yield FIFO arrivals across both cut regions:
  EXPECT_EQ(t(259), arrival(offsets, 169));
  for (auto dep = 170; dep < 200; ++dep) {
    EXPECT_EQ(t(260), arrival(offsets, dep)) << "at minute " << dep;
  }
  EXPECT_EQ(t(309), arrival(offsets, 249));
  for (auto dep = 250; dep < 300; ++dep) {
    EXPECT_EQ(t(310), arrival(offsets, dep)) << "at minute " << dep;
  }
  EXPECT_EQ(t(409), arrival(offsets, 399));
}

TEST(motis, td_offsets_keep_inactive_gaps) {
  auto const first = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const second = flex_payload(2U, 0U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 120, n::duration_t{10}, first},
      {150, 170, n::duration_t{8}, second},
  });
  motis::normalize_td_offsets(offsets);

  // No FIFO violation: departing at 119 arrives at 129, departing at 150
  // arrives at 158 -> the slow offer is kept as-is.
  EXPECT_EQ((std::vector{inactive(0), active(100, 10, first), inactive(120),
                         active(150, 8, second), inactive(170)}),
            offsets);
}

TEST(motis, td_offsets_drop_fully_dominated_window) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 200, n::duration_t{100}, slow},
      {150, 250, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(offsets);

  // Waiting from 100 for the fast offer (arrival 160) always beats the slow
  // offer (arrival >= 200) -> the slow window is fully replaced by an
  // inactive gap.
  EXPECT_EQ((std::vector{inactive(0), active(150, 10, fast), inactive(250)}),
            offsets);

  // The whole slow window is dominated: every departure in [100, 150) arrives
  // no earlier than waiting for the fast offer (arrival 160) would.
  for (auto dep = 100; dep < 150; ++dep) {
    EXPECT_EQ(t(160), arrival(offsets, dep)) << "at minute " << dep;
  }
  EXPECT_EQ(t(160), arrival(offsets, 150));
  EXPECT_EQ(t(259), arrival(offsets, 249));
}

TEST(motis, td_offsets_merge_inactive_after_cut) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  // The slow offer is cut at 250 + 10 - 100 = 160; the envelope already has an
  // inactive gap at 200 (slow closes, fast not yet open). The cut's inactive
  // entry and that gap are adjacent kMaxDuration entries -> they must collapse
  // into a single one.
  auto offsets = raw({
      {100, 200, n::duration_t{100}, slow},
      {250, 400, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(offsets);

  EXPECT_EQ((std::vector{inactive(0), active(100, 100, slow), inactive(160),
                         active(250, 10, fast), inactive(400)}),
            offsets);

  // Verify via nigiri's get_td_duration that the result is FIFO. Before the
  // cut the slow offer is used directly:
  EXPECT_EQ(t(200), arrival(offsets, 100));
  EXPECT_EQ(t(259), arrival(offsets, 159));
  // During the cut/overlap region departing later must never arrive earlier:
  // the routing core waits for the fast offer (arrival 260) instead of taking
  // the slow one, which would arrive at dep + 100 > 260 (the non-FIFO case).
  for (auto dep = 160; dep < 250; ++dep) {
    EXPECT_EQ(t(260), arrival(offsets, dep)) << "at minute " << dep;
  }
  // Inside the fast window the arrival grows monotonically again:
  EXPECT_EQ(t(260), arrival(offsets, 250));
  EXPECT_EQ(t(310), arrival(offsets, 300));
}

TEST(motis, td_offsets_same_mode_touching_windows) {
  auto const mode = flex_payload(1U, 0U, osr::direction::kBackward);

  // [100, 200) and [200, 300) of the same mode (e.g. the same flex transport on
  // consecutive days): add_td_window merges them, so the step function of the
  // mode has no two entries at 200.
  auto offsets = raw({
      {100, 200, n::duration_t{10}, mode},
      {200, 300, n::duration_t{10}, mode},
  });
  EXPECT_EQ((std::vector{active(100, 10, mode), closer(300, mode)}), offsets);

  motis::normalize_td_offsets(offsets);
  EXPECT_EQ((std::vector{inactive(0), active(100, 10, mode), inactive(300)}),
            offsets);
  EXPECT_EQ(t(260), arrival(offsets, 250));
}

TEST(motis, td_offsets_same_mode_nested_windows) {
  auto const mode = flex_payload(1U, 0U, osr::direction::kBackward);

  // [200, 250) lies inside [100, 300), both of the same mode. add_td_window
  // merges them, so the end of the inner window does not end the outer one.
  auto offsets = raw({
      {100, 300, n::duration_t{10}, mode},
      {200, 250, n::duration_t{10}, mode},
  });
  EXPECT_EQ((std::vector{active(100, 10, mode), closer(300, mode)}), offsets);

  motis::normalize_td_offsets(offsets);
  EXPECT_EQ((std::vector{inactive(0), active(100, 10, mode), inactive(300)}),
            offsets);
  EXPECT_EQ(t(270), arrival(offsets, 260));
}

// add_td_window checks nothing, so a caller that breaks the contract gets a
// wrong result. This documents what goes wrong; it is not desired behaviour.
TEST(motis, td_offsets_ill_formed_windows) {
  auto const mode = flex_payload(1U, 0U, osr::direction::kBackward);

  // Overlapping windows of one mode with different durations: extending the
  // first window keeps its duration, so the 30 min of [150, 250) are lost.
  auto overlapping = raw({
      {100, 200, n::duration_t{10}, mode},
      {150, 250, n::duration_t{30}, mode},
  });
  EXPECT_EQ((std::vector{active(100, 10, mode), closer(250, mode)}),
            overlapping);
  motis::normalize_td_offsets(overlapping);
  EXPECT_EQ(t(210), arrival(overlapping, 200));  // 230 with the real duration

  // Windows added out of order: the earlier one is swallowed by the later one
  // instead of being added.
  auto unsorted = raw({
      {200, 300, n::duration_t{10}, mode},
      {100, 150, n::duration_t{10}, mode},
  });
  EXPECT_EQ((std::vector{active(200, 10, mode), closer(300, mode)}), unsorted);
  motis::normalize_td_offsets(unsorted);
  EXPECT_EQ(t(210), arrival(unsorted, 100));  // 110 if [100, 150) were there
}

TEST(motis, td_offsets_preserve_mode_payload) {
  auto const backward = flex_payload(42U, 3U, osr::direction::kBackward);

  auto offsets = raw({
      {100, 200, n::duration_t{10}, backward},
  });
  motis::normalize_td_offsets(offsets);

  ASSERT_EQ(3U, offsets.size());
  auto const restored =
      motis::flex::mode_payload{offsets[1].transport_mode_payload_};
  EXPECT_EQ(osr::direction::kBackward, restored.get_dir());
  EXPECT_EQ(n::flex_transport_idx_t{42U}, restored.get_flex_transport());
  EXPECT_EQ(static_cast<n::stop_idx_t>(3U), restored.get_stop());
}

// --- Instrumentation for the evaluation ------------------------------------

TEST(motis, td_offsets_stats_blocked_window) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  // Same input as td_offsets_drop_fully_dominated_window: the slow window is
  // replaced entirely, so step 2 reports one blocked and no cut window.
  auto offsets = raw({
      {100, 200, n::duration_t{100}, slow},
      {150, 250, n::duration_t{10}, fast},
  });
  auto stats = motis::td_norm_stats{};
  motis::normalize_td_offsets(offsets, &stats);

  EXPECT_EQ(1U, stats.n_locations_);
  EXPECT_EQ(4U, stats.n_raw_);  // two windows, two raw entries each
  EXPECT_EQ(2U, stats.q_max_);  // both offers available at the same time
  EXPECT_EQ(1U, stats.n_fifo_blocked_);
  EXPECT_EQ(0U, stats.n_fifo_cut_);
  EXPECT_EQ(1U, stats.n_leading_);  // sequence starts after t=0
  EXPECT_EQ(offsets.size(), stats.n_out_);
}

TEST(motis, td_offsets_stats_cut_window) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  // Same input as td_offsets_merge_inactive_after_cut: only the tail of the
  // slow window is dominated, so step 2 reports one cut and nothing blocked.
  auto offsets = raw({
      {100, 200, n::duration_t{100}, slow},
      {250, 400, n::duration_t{10}, fast},
  });
  auto stats = motis::td_norm_stats{};
  motis::normalize_td_offsets(offsets, &stats);

  EXPECT_EQ(1U, stats.n_fifo_cut_);
  EXPECT_EQ(0U, stats.n_fifo_blocked_);
  EXPECT_EQ(1U, stats.q_max_);  // the windows do not overlap
  EXPECT_EQ(offsets.size(), stats.n_out_);
}

TEST(motis, td_offsets_stats_accumulate_over_locations) {
  auto const a = flex_payload(1U, 0U, osr::direction::kBackward);
  auto stats = motis::td_norm_stats{};

  auto first = raw({{100, 200, n::duration_t{10}, a}});
  motis::normalize_td_offsets(first, &stats);
  auto second = raw({{300, 400, n::duration_t{10}, a}});
  motis::normalize_td_offsets(second, &stats);

  EXPECT_EQ(2U, stats.n_locations_);
  EXPECT_EQ(4U, stats.n_raw_);
  EXPECT_EQ(first.size() + second.size(), stats.n_out_);
}

TEST(motis, td_offsets_fifo_repair_can_be_disabled) {
  auto const slow = flex_payload(1U, 0U, osr::direction::kBackward);
  auto const fast = flex_payload(2U, 0U, osr::direction::kBackward);

  struct restore {
    ~restore() { motis::fifo_repair_enabled() = prev_; }
    bool prev_;
  } const guard{motis::fifo_repair_enabled()};

  auto with_repair = raw({
      {100, 200, n::duration_t{100}, slow},
      {150, 250, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(with_repair);

  motis::fifo_repair_enabled() = false;
  auto without_repair = raw({
      {100, 200, n::duration_t{100}, slow},
      {150, 250, n::duration_t{10}, fast},
  });
  motis::normalize_td_offsets(without_repair);

  EXPECT_NE(with_repair, without_repair);

  // Without step 2 the slow offer survives, and the sequence violates FIFO:
  // departing later arrives earlier. This is exactly what the repair prevents.
  EXPECT_EQ(t(200), arrival(without_repair, 100));
  EXPECT_EQ(t(160), arrival(without_repair, 150));
  EXPECT_LT(arrival(without_repair, 150), arrival(without_repair, 100));

  // With the repair the arrival never decreases as the departure grows.
  // The fast window [150, 250) is half-open, so 249 is the last departure
  // that can still start a ride.
  auto prev = arrival(with_repair, 100);
  for (auto dep = 101; dep < 250; ++dep) {
    auto const cur = arrival(with_repair, dep);
    ASSERT_TRUE(cur.has_value()) << "at minute " << dep;
    EXPECT_GE(*cur, *prev) << "at minute " << dep;
    prev = cur;
  }
}

// Property test: on random producer-built sequences, both evaluation paths must
// reproduce alpha_tilde from Definition 2.
//   * normalized sequence + get_td_duration  (the first-match fast path)
//   * raw sequence        + get_td_duration_scan (the ablation path)
// A failure here localises the discrepancy that end-to-end runs only show as
// differing itineraries.
TEST(motis, td_offsets_property_alpha_tilde) {
  // Losslessness, exactly as Primitive-Formeln.md states it:
  //     alpha_merged(tau) == min_j alpha_provider_j(tau)   for all tau
  //
  // Each provider gets DISJOINT windows in ascending order, because
  // add_td_window's contract requires windows sorted by mode and by `from`,
  // with equal durations where windows of one mode overlap. Competition
  // between providers -- the case the envelope exists for -- is modelled by
  // letting different modes overlap freely.
  auto rng = std::mt19937{42};
  auto n_provider = std::uniform_int_distribution<int>{1, 3};
  auto n_win = std::uniform_int_distribution<int>{1, 4};
  auto gap = std::uniform_int_distribution<int>{0, 120};
  auto len = std::uniform_int_distribution<int>{5, 180};
  auto dur = std::uniform_int_distribution<int>{1, 200};

  auto const horizon = 3000;
  auto lossy = 0, cases = 0, worse = 0, better = 0;
  auto worst_gap = 0;
  auto naive_wrong = 0, naive_gap = 0, scan_wrong = 0;

  for (auto iter = 0; iter != 3000; ++iter) {
    auto per_provider = std::vector<std::vector<n::routing::td_offset>>{};
    auto merged = std::vector<n::routing::td_offset>{};
    auto const q = n_provider(rng);
    for (auto j = 0; j != q; ++j) {
      auto const mode = flex_payload(static_cast<std::uint32_t>(j + 1), 0,
                                     osr::direction::kForward);
      auto own = std::vector<n::routing::td_offset>{};
      auto cursor = gap(rng);
      for (auto w = 0, nw = n_win(rng); w != nw; ++w) {
        auto const from = cursor;
        auto const to = from + len(rng);
        auto const d = n::duration_t{dur(rng)};
        motis::add_td_window(own, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        motis::add_td_window(merged, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        cursor = to + gap(rng);
      }
      if (!own.empty()) {
        per_provider.push_back(std::move(own));
      }
    }
    if (merged.empty() || per_provider.empty()) {
      continue;
    }
    auto normalized = merged;
    motis::normalize_td_offsets(normalized);

    for (auto dep = 0; dep <= 900; dep += 29) {
      ++cases;
      auto expect = std::optional<n::unixtime_t>{};
      for (auto const& seq : per_provider) {
        auto const a = alpha_brute(seq, dep, horizon);
        if (a.has_value() && (!expect.has_value() || *a < *expect)) {
          expect = a;
        }
      }
      auto const got = arrival(normalized, dep);
      // The ablation path must agree with the fast path on the same sequence:
      // both evaluate the identical normal form, so any difference is a bug in
      // the scan rather than a property of normalization.
      // With (N1) established (step 2 on) the first-match fast path must equal
      // the exhaustive scan. With step 2 disabled it need not -- and the rate
      // and size of the disagreement is what FIFO repair buys.
      auto const by_scan = scan_arrival(normalized, dep);
      if (by_scan != expect) {
        ++scan_wrong;  // does the exhaustive lookup alone restore correctness?
      }
      if (by_scan != got) {
        ++naive_wrong;
        if (by_scan.has_value() && got.has_value()) {
          naive_gap = std::max(naive_gap, static_cast<int>((*got - *by_scan).count()));
        }
      }
      if (got != expect) {
        ++lossy;
        if (expect.has_value() && got.has_value()) {
          auto const g = static_cast<int>((*got - *expect).count());
          if (g > 0) { ++worse; worst_gap = std::max(worst_gap, g); }
          else { ++better; }
        } else if (!got.has_value()) {
          ++worse;
        } else {
          ++better;
        }
        if (lossy <= 3) {
          ADD_FAILURE() << "dep=" << dep
                        << " expected=" << (expect ? std::to_string(expect->time_since_epoch().count()) : "none")
                        << " got=" << (got ? std::to_string(got->time_since_epoch().count()) : "none");
        }
      }
    }
  }
  std::cout << "exhaustive scan != lossless reference: " << scan_wrong << "/"
            << cases << std::endl;
  std::cout << "fast-path != exhaustive scan: " << naive_wrong << "/" << cases
            << " (worst " << naive_gap << " min too late)" << std::endl;
  std::cout << "cases=" << cases << " mismatches=" << lossy
            << " (normalized later=" << worse << ", earlier=" << better
            << ", worst_gap=" << worst_gap << " min)" << std::endl;
}

// Same property, backward direction. The forward test showed the scan is a
// valid oracle there; this checks the mirror branch, which the search also
// uses and which was never covered.
TEST(motis, td_offsets_property_alpha_tilde_backward) {
  auto rng = std::mt19937{7};
  auto n_provider = std::uniform_int_distribution<int>{1, 3};
  auto n_win = std::uniform_int_distribution<int>{1, 4};
  auto gap = std::uniform_int_distribution<int>{0, 120};
  auto len = std::uniform_int_distribution<int>{5, 180};
  auto dur = std::uniform_int_distribution<int>{1, 200};
  auto const horizon = 3000;
  auto fast_wrong = 0, scan_wrong = 0, cases = 0;

  for (auto iter = 0; iter != 2000; ++iter) {
    auto per_provider = std::vector<std::vector<n::routing::td_offset>>{};
    auto merged = std::vector<n::routing::td_offset>{};
    for (auto j = 0, q = n_provider(rng); j != q; ++j) {
      auto const mode = flex_payload(static_cast<std::uint32_t>(j + 1), 0,
                                     osr::direction::kForward);
      auto own = std::vector<n::routing::td_offset>{};
      auto cursor = gap(rng);
      for (auto w = 0, nw = n_win(rng); w != nw; ++w) {
        auto const from = cursor, to = from + len(rng);
        auto const d = n::duration_t{dur(rng)};
        motis::add_td_window(own, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        motis::add_td_window(merged, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        cursor = to + gap(rng);
      }
      if (!own.empty()) per_provider.push_back(std::move(own));
    }
    if (merged.empty() || per_provider.empty()) continue;
    auto normalized = merged;
    motis::normalize_td_offsets(normalized);

    for (auto arr = 60; arr <= 900; arr += 31) {
      ++cases;
      auto expect = std::optional<n::duration_t>{};
      for (auto const& seq : per_provider) {
        auto const a = alpha_brute_bwd(seq, arr, horizon);
        if (a.has_value() && (!expect.has_value() || *a < *expect)) expect = a;
      }
      if (fast_bwd(normalized, arr) != expect) ++fast_wrong;
      if (scan_bwd(normalized, arr) != expect) ++scan_wrong;
    }
  }
  std::cout << "BACKWARD cases=" << cases
            << " fast_path_wrong=" << fast_wrong
            << " scan_wrong=" << scan_wrong << std::endl;
  EXPECT_EQ(0, fast_wrong);
  EXPECT_EQ(0, scan_wrong);
}

// Design D: the producers' windows, evaluated directly. Must reproduce the
// same alpha_tilde as the normalized fast path -- in BOTH directions. The
// backward branch is checked explicitly because reasoning by analogy from the
// forward one produced a 78%-wrong implementation once already.
TEST(motis, td_offsets_property_raw_windows) {
  auto rng = std::mt19937{11};
  auto n_provider = std::uniform_int_distribution<int>{1, 3};
  auto n_win = std::uniform_int_distribution<int>{1, 4};
  auto gap = std::uniform_int_distribution<int>{0, 120};
  auto len = std::uniform_int_distribution<int>{5, 180};
  auto dur = std::uniform_int_distribution<int>{1, 200};
  auto const horizon = 3000;
  auto fwd_wrong = 0, bwd_wrong = 0, cases = 0;

  for (auto iter = 0; iter != 2000; ++iter) {
    auto per_provider = std::vector<std::vector<n::routing::td_offset>>{};
    auto raw = std::vector<n::routing::td_offset>{};
    for (auto j = 0, q = n_provider(rng); j != q; ++j) {
      auto const mode = flex_payload(static_cast<std::uint32_t>(j + 1), 0,
                                     osr::direction::kForward);
      auto own = std::vector<n::routing::td_offset>{};
      auto cursor = gap(rng);
      for (auto w = 0, nw = n_win(rng); w != nw; ++w) {
        auto const from = cursor, to = from + len(rng);
        auto const d = n::duration_t{dur(rng)};
        motis::add_td_window(own, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        motis::add_td_window(raw, n::interval{t(from), t(to)}, d, {.payload_ = mode});
        cursor = to + gap(rng);
      }
      if (!own.empty()) per_provider.push_back(std::move(own));
    }
    if (raw.empty() || per_provider.empty()) continue;

    for (auto x = 0; x <= 900; x += 29) {
      ++cases;
      auto fexp = std::optional<n::unixtime_t>{};
      auto bexp = std::optional<n::duration_t>{};
      for (auto const& seq : per_provider) {
        auto const f = alpha_brute(seq, x, horizon);
        if (f.has_value() && (!fexp.has_value() || *f < *fexp)) fexp = f;
        if (x >= 60) {
          auto const b = alpha_brute_bwd(seq, x, horizon);
          if (b.has_value() && (!bexp.has_value() || *b < *bexp)) bexp = b;
        }
      }
      if (raw_fwd(raw, x) != fexp) ++fwd_wrong;
      if (x >= 60 && raw_bwd(raw, x) != bexp) ++bwd_wrong;
    }
  }
  std::cout << "RAW-WINDOWS cases=" << cases << " fwd_wrong=" << fwd_wrong
            << " bwd_wrong=" << bwd_wrong << std::endl;
  EXPECT_EQ(0, fwd_wrong);
  EXPECT_EQ(0, bwd_wrong);
}

// remove_slower_than_fastest_direct erases single entries from td_start_. On
// the normal form an entry is valid until the NEXT entry, so erasing one
// stretches its predecessor over the erased span: with touching windows
//     [10: 5 min, A] [20: 100 min, B] [30: end]
// pruning the 100-min entry leaves the 5-min offer of A valid until 30, an
// offer nobody published. Pruning may only take options away; it must never
// yield an arrival earlier than the unpruned sequence does. Design D's raw
// pairs are checked alongside: there, erasing a start orphans its closer.
TEST(motis, td_offsets_pruning_does_not_stretch_windows) {
  auto const a = flex_payload(1U, 0U, osr::direction::kForward);
  auto const b = flex_payload(2U, 0U, osr::direction::kForward);
  auto const l = n::location_idx_t{7U};

  auto const prune = [&](std::vector<n::routing::td_offset> offsets) {
    auto q = n::routing::query{};
    q.fastest_direct_ = n::duration_t{50};
    // destination reachable in 0 min, so every start offset of >= 50 min loses
    // against the direct connection and is erased
    q.destination_.emplace_back(n::location_idx_t{8U}, n::duration_t{0}, 0U);
    q.td_start_[l] = std::move(offsets);
    motis::ep::remove_slower_than_fastest_direct(q);
    return q.td_start_.at(l);
  };

  auto const offers = {offer{10, 20, n::duration_t{5}, a},
                       offer{20, 30, n::duration_t{100}, b}};

  auto normalized = raw(offers);
  motis::normalize_td_offsets(normalized, nullptr);
  auto const normalized_pruned = prune(normalized);

  auto const raw_windows = raw(offers);
  auto const raw_pruned = prune(raw_windows);
  auto const raw_arrival = [](std::vector<n::routing::td_offset> const& v,
                              int const dep) {
    auto const r = n::get_td_duration_raw_windows<n::direction::kForward>(
        std::span<n::routing::td_offset const>{v}, t(dep));
    return r.has_value() ? std::optional{t(dep) + r->first} : std::nullopt;
  };

  // true if `pruned` offers an arrival the unpruned sequence cannot reach
  auto const invents = [](std::optional<n::unixtime_t> const pruned,
                          std::optional<n::unixtime_t> const unpruned) {
    return pruned.has_value() &&
           (!unpruned.has_value() || *pruned < *unpruned);
  };

  auto normalized_wrong = 0, raw_wrong = 0;
  for (auto dep = 0; dep <= 40; ++dep) {
    auto const want = arrival(normalized, dep);
    auto const got = arrival(normalized_pruned, dep);
    if (invents(got, want)) {
      ++normalized_wrong;
      std::cout << "  normal form: dep=" << dep << " unpruned="
                << (want ? std::to_string(want->time_since_epoch().count())
                         : std::string{"none"})
                << " pruned=" << got->time_since_epoch().count() << "\n";
    }
    if (invents(raw_arrival(raw_pruned, dep), raw_arrival(raw_windows, dep))) {
      ++raw_wrong;
    }
  }
  std::cout << "PRUNING normal_form_wrong=" << normalized_wrong
            << " raw_wrong=" << raw_wrong << std::endl;
  EXPECT_EQ(0, normalized_wrong);
  EXPECT_EQ(0, raw_wrong);
}
