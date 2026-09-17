#include "gtest/gtest.h"

#include "motis/odm/td_offsets.h"
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 6U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_, unixtime_t{11h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(), kOdmTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{11h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].valid_from_, unixtime_t{12h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[4].mode(), kOdmTransportMode);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].valid_from_,
            unixtime_t{12h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[5].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 4U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 1min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_, 30min);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_,
            unixtime_t{10h + 2min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 4U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 2min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_, 30min);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].valid_from_,
            unixtime_t{10h + 3min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[2].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].valid_from_,
            unixtime_t{10h + 4min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[3].mode(), kOdmTransportMode);
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

  auto const td_offsets = motis::odm::get_td_offsets(rides, kOdmTransportMode);

  print(td_offsets);

  ASSERT_TRUE(td_offsets.contains(location_idx_t{1U}));
  ASSERT_EQ(td_offsets.at(location_idx_t{1U}).size(), 2U);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].valid_from_, unixtime_t{10h});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].duration_, 1h);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[0].mode(), kOdmTransportMode);

  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].valid_from_,
            unixtime_t{10h + 4min});
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].duration_,
            footpath::kMaxDuration);
  EXPECT_EQ(td_offsets.at(location_idx_t{1U})[1].mode(), kOdmTransportMode);
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

TEST(odm, get_td_offsets_split_basic) {
  // Service 10:00-11:00, the ride has to end by 11:00 -> latest departure
  // 10:40, the offset ends at 10:41.
  EXPECT_EQ(
      (std::vector{td_offset::make(unixtime_t{10h}, 20min, kOdmTransportMode),
                   td_offset::make(unixtime_t{10h + 41min},
                                   footpath::kMaxDuration, kOdmTransportMode)}),
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
  EXPECT_EQ(
      (std::vector{td_offset::make(unixtime_t{10h}, 20min, kOdmTransportMode),
                   td_offset::make(unixtime_t{11h + 41min},
                                   footpath::kMaxDuration, kOdmTransportMode)}),
      taxi_td_offsets({{unixtime_t{10h}, unixtime_t{11h}},
                       {unixtime_t{10h + 30min}, unixtime_t{12h}}}));
}

}  // namespace motis::odm