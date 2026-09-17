#include "gtest/gtest.h"

#include "nigiri/td_footpath.h"

#include "motis/odm/td_offsets.h"
#include "motis/td_offsets.h"
#include "motis/transport_mode.h"

using namespace nigiri;
using namespace nigiri::routing;
using namespace std::chrono_literals;

namespace motis::odm {

void print(td_offsets_t const& tdos) {
  for (auto const& [l, tdo] : tdos) {
    std::cout << "l: " << l << ":\n";
    for (auto const& t : tdo) {
      std::cout << "[valid_from_: " << t.valid_from_
                << ", duration_: " << t.duration_
                << ", transport_mode_: " << to_mode(t.mode())
                << ", transport_mode_payload_: " << t.transport_mode_payload_
                << "]\n";
    }
  }
}

TEST(odm, get_td_offsets_basic) {
  auto const rides = std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                                         .time_at_stop_ = unixtime_t{11h},
                                         .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_extension) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{11h + 1min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_extension_reverse) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{11h + 1min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_extension_fill_gap) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{11h + 1min},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_intermittent) {
  auto const rides = std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                                         .time_at_stop_ = unixtime_t{11h},
                                         .stop_ = location_idx_t{1U}},
                                        {.time_at_start_ = unixtime_t{11h},
                                         .time_at_stop_ = unixtime_t{12h},
                                         .stop_ = location_idx_t{1U}},
                                        {.time_at_start_ = unixtime_t{12h},
                                         .time_at_stop_ = unixtime_t{13h},
                                         .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 6U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_, unixtime_t{11h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(),
            kRideSharingTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{11h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].valid_from_, unixtime_t{12h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].mode(),
            kRideSharingTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].valid_from_,
            unixtime_t{12h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_long_short_long) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{10h + 31min},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 4U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_, 30min);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_,
            unixtime_t{10h + 2min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_late_improvement) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{11h + 1min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 3min},
                          .time_at_stop_ = unixtime_t{11h + 3min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{10h + 32min},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 4U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 2min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_, 30min);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{10h + 4min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(),
            kRideSharingTransportMode);
}

TEST(odm, get_td_offsets_late_worse) {
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{10h},
                          .time_at_stop_ = unixtime_t{11h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 1min},
                          .time_at_stop_ = unixtime_t{11h + 1min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{11h + 2min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 3min},
                          .time_at_stop_ = unixtime_t{11h + 3min},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 2min},
                          .time_at_stop_ = unixtime_t{12h + 2min},
                          .stop_ = location_idx_t{1U}}};

  auto const td_offsets =
      motis::odm::get_td_offsets(rides, kRideSharingTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(),
            kRideSharingTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 4min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(),
            kRideSharingTransportMode);
}

// A range query looks up the td offsets backwards: for a departure event at
// the stop it takes the latest ride that still arrives in time. This is what
// decides which rides end up in a journey.
std::optional<unixtime_t> latest_pickup(std::vector<td_offset> const& tdos,
                                        unixtime_t const at_stop) {
  auto const r = get_td_duration<direction::kBackward>(
      std::span<td_offset const>{tdos}, at_stop);
  return r.has_value() ? std::optional{at_stop - r->first} : std::nullopt;
}

TEST(odm, get_td_offsets_two_rides_pareto_window) {
  // A: leaves at 8:00, 2 h ride, at the stop at 10:00.
  // B: leaves at 10:20, 10 min ride, at the stop at 10:30.
  auto const rides =
      std::vector<start>{{.time_at_start_ = unixtime_t{8h},
                          .time_at_stop_ = unixtime_t{10h},
                          .stop_ = location_idx_t{1U}},
                         {.time_at_start_ = unixtime_t{10h + 20min},
                          .time_at_stop_ = unixtime_t{10h + 30min},
                          .stop_ = location_idx_t{1U}}};

  auto const tdos = motis::odm::get_td_offsets(rides, kRideSharingTransportMode)
                        .at(location_idx_t{1U});

  // Both rides are kept, the slower one included.
  EXPECT_EQ((std::vector{
                td_offset::make(unixtime_t{8h}, 2h, kRideSharingTransportMode),
                td_offset::make(unixtime_t{8h + 1min}, footpath::kMaxDuration,
                                kRideSharingTransportMode),
                td_offset::make(unixtime_t{10h + 20min}, 10min,
                                kRideSharingTransportMode),
                td_offset::make(unixtime_t{10h + 21min}, footpath::kMaxDuration,
                                kRideSharingTransportMode)}),
            tdos);

  // Before A arrives, no ride reaches the stop in time.
  EXPECT_EQ(std::nullopt, latest_pickup(tdos, unixtime_t{9h + 59min}));

  // Between the two arrivals only A works, so it is needed for the journeys
  // departing in that window.
  EXPECT_EQ(unixtime_t{8h}, latest_pickup(tdos, unixtime_t{10h}));
  EXPECT_EQ(unixtime_t{8h}, latest_pickup(tdos, unixtime_t{10h + 29min}));

  // From B's arrival on, B is the later and therefore better pickup.
  EXPECT_EQ(unixtime_t{10h + 20min},
            latest_pickup(tdos, unixtime_t{10h + 30min}));
  EXPECT_EQ(unixtime_t{10h + 20min}, latest_pickup(tdos, unixtime_t{11h}));
}

TEST(odm, get_td_offsets_dominated_ride) {
  // Same as above, but A is so slow that it arrives AFTER B: it leaves at
  // 8:00, takes 3 h and is at the stop at 11:00, while B leaves at 10:20 and
  // is there at 10:30.
  auto tdos = motis::odm::get_td_offsets(
                  std::vector<start>{{.time_at_start_ = unixtime_t{8h},
                                      .time_at_stop_ = unixtime_t{11h},
                                      .stop_ = location_idx_t{1U}},
                                     {.time_at_start_ = unixtime_t{10h + 20min},
                                      .time_at_stop_ = unixtime_t{10h + 30min},
                                      .stop_ = location_idx_t{1U}}},
                  kRideSharingTransportMode)
                  .at(location_idx_t{1U});

  // A is kept in the offsets, ...
  EXPECT_EQ(td_offset::make(unixtime_t{8h}, 3h, kRideSharingTransportMode),
            tdos.front());

  // ... but a range query never picks it: for every departure event A reaches,
  // B leaves later and is there earlier.
  EXPECT_EQ(unixtime_t{10h + 20min}, latest_pickup(tdos, unixtime_t{11h}));
  EXPECT_EQ(unixtime_t{10h + 20min},
            latest_pickup(tdos, unixtime_t{11h + 5min}));

  // Normalizing would drop exactly that unusable entry (ride sharing does not
  // normalize; this only shows what the FIFO repair does).
  motis::normalize_td_offsets(tdos);
  EXPECT_EQ(
      (std::vector{td_offset{unixtime_t{0h}, footpath::kMaxDuration},
                   td_offset::make(unixtime_t{10h + 20min}, 10min,
                                   kRideSharingTransportMode),
                   td_offset{unixtime_t{10h + 21min}, footpath::kMaxDuration}}),
      tdos);
}

// Taxi td offsets for a single stop with a 20 min ride and the given service
// times. With one offset, everything ends up in the "short" half of the split.
std::vector<td_offset> taxi_td_offsets(service_times_t const& service_times) {
  auto const [lo, hi] = get_td_offsets_split(
      {offset{location_idx_t{1U}, 20min, kOdmTransportMode}}, {service_times},
      kOdmTransportMode);
  EXPECT_TRUE(hi.empty());
  EXPECT_TRUE(lo.contains(location_idx_t{1U}));
  return lo.at(location_idx_t{1U});
}

td_offset taxi_ride(unixtime_t const from) {
  return td_offset::make(from, 20min, kOdmTransportMode);
}

// Normalization leaves the gaps without a mode.
td_offset no_ride(unixtime_t const from) {
  return td_offset{from, footpath::kMaxDuration};
}

TEST(odm, get_td_offsets_split_basic) {
  // Service 10:00-11:00: the ride has to end by 11:00, so the last departure
  // is 10:40 and the offset ends at 10:41. Normalization prepends the gap from
  // the beginning of time.
  EXPECT_EQ((std::vector{no_ride(unixtime_t{0h}), taxi_ride(unixtime_t{10h}),
                         no_ride(unixtime_t{10h + 41min})}),
            taxi_td_offsets({{unixtime_t{10h}, unixtime_t{11h}}}));
}

TEST(odm, get_td_offsets_split_too_short) {
  // A 15 min service time can't fit a 20 min ride.
  EXPECT_TRUE(
      taxi_td_offsets({{unixtime_t{10h}, unixtime_t{10h + 15min}}}).empty());
}

TEST(odm, get_td_offsets_split_overlapping) {
  // Departure windows [10:00, 10:41) and [10:30, 11:41) overlap and are
  // merged, so the offset does not end at 10:41.
  EXPECT_EQ((std::vector{no_ride(unixtime_t{0h}), taxi_ride(unixtime_t{10h}),
                         no_ride(unixtime_t{11h + 41min})}),
            taxi_td_offsets({{unixtime_t{10h}, unixtime_t{11h}},
                             {unixtime_t{10h + 30min}, unixtime_t{12h}}}));
}

TEST(odm, get_td_offsets_split_gap) {
  // Two service times far apart stay two separate windows.
  EXPECT_EQ(
      (std::vector{no_ride(unixtime_t{0h}), taxi_ride(unixtime_t{10h}),
                   no_ride(unixtime_t{10h + 41min}), taxi_ride(unixtime_t{12h}),
                   no_ride(unixtime_t{12h + 41min})}),
      taxi_td_offsets({{unixtime_t{10h}, unixtime_t{11h}},
                       {unixtime_t{12h}, unixtime_t{13h}}}));
}

}  // namespace motis::odm