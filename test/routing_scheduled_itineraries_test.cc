#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <tuple>
#include <vector>

#include "date/date.h"

#include "utl/init_from.h"

#include "nigiri/rt/gtfsrt_update.h"

#include "motis-api/motis-api.h"
#include "motis/config.h"
#include "motis/data.h"
#include "motis/endpoints/routing.h"
#include "motis/import.h"

#include "./util.h"

using namespace date;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace motis;
namespace n = nigiri;

namespace {

// Two competing trips A -> B, both without transfers, on 2019-05-01.
// Times are Europe/Berlin (CEST, UTC+2) -> 09:00 local == 07:00Z.
//
//   T1  A 09:00 -> B 10:00   fastest according to the SCHEDULE
//   T2  A 09:10 -> B 10:15
//
// The test then delays T1 by +5 min on departure and +40 min on arrival, so
// under REALTIME T1 arrives 10:40 and T2 (10:15) wins instead. Scheduled and
// realtime slot therefore select *different* trips, which is what makes the
// dual-slot response meaningful to assert on.
constexpr auto const kTwoTripsGTFS = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_lat,stop_lon,location_type,parent_station
A,A,50.00000,8.00000,0,
B,B,51.00000,9.00000,0,

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
R1,DB,R1,,,3
R2,DB,R2,,,3

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id
R1,S1,T1,,
R2,S1,T2,,

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence,pickup_type,drop_off_type
T1,09:00:00,09:00:00,A,0,0,0
T1,10:00:00,10:00:00,B,1,0,0
T2,09:10:00,09:10:00,A,0,0,0
T2,10:15:00,10:15:00,B,1,0,0

# calendar_dates.txt
service_id,date,exception_type
S1,20190501,1
)"sv;

// +5 min on T1's departure at A, +40 min on its arrival at B.
void delay_t1(data& d) {
  auto const stats = n::rt::gtfsrt_update_msg(
      *d.tt_, *d.rt_->rtt_, n::source_idx_t{0}, "test",
      test::to_feed_msg(
          {test::trip_update{
              .trip_ = {.trip_id_ = "T1", .date_ = std::string{"20190501"}},
              .stop_updates_ = {{.stop_id_ = "A",
                                 .seq_ = std::optional{0U},
                                 .ev_type_ = n::event_type::kDep,
                                 .delay_minutes_ = 5},
                                {.stop_id_ = "B",
                                 .seq_ = std::optional{1U},
                                 .ev_type_ = n::event_type::kArr,
                                 .delay_minutes_ = 40}}}},
          sys_days{2019_y / May / 1} + 6h));
  ASSERT_EQ(1U, stats.total_entities_success_);
}

// The key `motis compare` uses, and what the rt-mode experiments rely on.
using itinerary_key =
    std::tuple<std::chrono::sys_seconds, std::chrono::sys_seconds, std::int64_t>;

std::vector<itinerary_key> keys(std::vector<api::Itinerary> const& v) {
  auto r = std::vector<itinerary_key>{};
  for (auto const& i : v) {
    r.emplace_back(*i.startTime_, *i.endTime_, i.transfers_);
  }
  std::sort(begin(r), end(r));
  return r;
}

// 2019-05-01 is CEST (UTC+2), so 09:00 local == 07:00Z.
constexpr std::chrono::sys_seconds utc(std::chrono::hours const h,
                                       std::chrono::minutes const m) {
  return std::chrono::sys_seconds{sys_days{2019_y / May / 1}} + h + m;
}

constexpr auto const kBaseQuery =
    "?fromPlace=test_A"
    "&toPlace=test_B"
    "&time=2019-05-01T06:50Z"
    "&timetableView=false";

}  // namespace

// `Itinerary.startTime`/`endTime` are taken straight from the nigiri journey
// (journey_to_response.cc) and are NOT recomputed from the annotated legs.
// For the scheduled slot of realtimeMode=REALTIME_AND_SCHEDULED that means
// they stay SCHEDULED times, while the per-leg times carry realtime data.
//
// Everything comparing a `both` response against a standalone OFF/REALTIME run
// on (startTime, endTime, transfers) depends on that split -- if itinerary
// level times were ever recomputed from the annotated legs, those comparisons
// would silently start comparing realtime against scheduled times.
TEST(motis, routing_scheduled_itineraries_keep_scheduled_times) {
  auto ec = std::error_code{};
  std::filesystem::remove_all("test/data_scheduled_itineraries", ec);

  auto const c =
      config{.server_ = {{.web_folder_ = "ui/build", .n_threads_ = 1U}},
             .timetable_ = config::timetable{
                 .first_day_ = "2019-05-01",
                 .num_days_ = 2,
                 .datasets_ = {{"test", {.path_ = std::string{kTwoTripsGTFS}}}}}};
  import(c, "test/data_scheduled_itineraries");
  auto d = data{"test/data_scheduled_itineraries", c};
  d.init_rtt(sys_days{2019_y / May / 1});
  delay_t1(d);
  auto const routing = utl::init_from<ep::routing>(d).value();

  auto const off = routing(std::string{kBaseQuery} + "&realtimeMode=OFF");
  auto const rt = routing(std::string{kBaseQuery} + "&realtimeMode=REALTIME");
  auto const both =
      routing(std::string{kBaseQuery} + "&realtimeMode=REALTIME_AND_SCHEDULED");

  // The delay has to actually change the answer, otherwise the rest of this
  // test would pass for the wrong reason.
  ASSERT_EQ(1U, off.itineraries_.size());
  ASSERT_EQ(1U, rt.itineraries_.size());
  EXPECT_EQ(utc(8h, 0min), *off.itineraries_.front().endTime_);
  EXPECT_EQ(utc(8h, 15min), *rt.itineraries_.front().endTime_);

  // --- the dual slot returns both results in one response -------------------
  ASSERT_TRUE(both.scheduledItineraries_.has_value());
  ASSERT_EQ(1U, both.itineraries_.size());
  ASSERT_EQ(1U, both.scheduledItineraries_->size());

  EXPECT_EQ(keys(rt.itineraries_), keys(both.itineraries_));
  EXPECT_EQ(keys(off.itineraries_), keys(*both.scheduledItineraries_));

  // --- the invariant --------------------------------------------------------
  auto const& sched = both.scheduledItineraries_->front();
  ASSERT_FALSE(sched.legs_.empty());
  auto const& first = sched.legs_.front();
  auto const& last = sched.legs_.back();

  // itinerary level times are the SCHEDULED ones
  EXPECT_EQ(utc(7h, 0min), *sched.startTime_);
  EXPECT_EQ(utc(8h, 0min), *sched.endTime_);
  EXPECT_EQ(*first.scheduledStartTime_, *sched.startTime_);
  EXPECT_EQ(*last.scheduledEndTime_, *sched.endTime_);

  // ... and are NOT the annotated realtime ones
  EXPECT_NE(*first.startTime_, *sched.startTime_);
  EXPECT_NE(*last.endTime_, *sched.endTime_);

  // leg level times DO carry the realtime annotation (+5 min / +40 min)
  EXPECT_TRUE(first.realTime_);
  EXPECT_EQ(utc(7h, 5min), *first.startTime_);
  EXPECT_EQ(utc(8h, 40min), *last.endTime_);
}
