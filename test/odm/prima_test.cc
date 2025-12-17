#include "gtest/gtest.h"

#include "nigiri/loader/dir.h"
#include "nigiri/loader/gtfs/load_timetable.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/common/parse_time.h"
#include "nigiri/routing/journey.h"
#include "nigiri/special_stations.h"

#include "motis/odm/odm.h"
#include "motis/odm/prima.h"
#include "motis/transport_mode_ids.h"

#include "motis-api/motis-api.h"

namespace n = nigiri;
namespace nr = nigiri::routing;
using namespace motis::odm;
using namespace std::chrono_literals;
using namespace date;

n::loader::mem_dir tt_files() {
  return n::loader::mem_dir::read(R"__(
"(
# stops.txt
stop_id,stop_name,stop_desc,stop_lat,stop_lon,stop_url,location_type,parent_station
A,A,A,0.1,0.1,,,,
B,B,B,0.2,0.2,,,,
C,C,C,0.3,0.3,,,,
D,D,D,0.4,0.4,,,,
)__");
}

constexpr auto blacklist_request =
    R"({"start":{"lat":0E0,"lng":0E0},"target":{"lat":0E0,"lng":0E0},"startBusStops":[{"lat":1E-1,"lng":1E-1},{"lat":2E-1,"lng":2E-1}],"targetBusStops":[{"lat":3.0000000000000004E-1,"lng":3.0000000000000004E-1},{"lat":4E-1,"lng":4E-1}],"earliest":0,"latest":172800000,"startFixed":true,"capacities":{"wheelchairs":1,"bikes":0,"passengers":1,"luggage":0}})";

constexpr auto invalid_response = R"({"message":"Internal Error"})";

constexpr auto blacklist_response = R"(
{
  "start": [[{"startTime": 32400000, "endTime": 43200000}],[{"startTime": 43200000, "endTime": 64800000}]],
  "target": [[{"startTime": 43200000, "endTime": 64800000}],[]],
  "direct": []
}
)";

TEST(odm, prima_update) {
  n::timetable tt;
  tt.date_range_ = {date::sys_days{2017_y / January / 1},
                    date::sys_days{2017_y / January / 2}};
  n::loader::register_special_stations(tt);
  auto const src = n::source_idx_t{0};
  n::loader::gtfs::load_timetable({.default_tz_ = "Europe/Berlin"}, src,
                                  tt_files(), tt);
  n::loader::finalize(tt);

  auto const get_loc_idx = [&](auto&& s) {
    return tt.locations_.location_id_to_idx_.at({.id_ = s, .src_ = src});
  };

  auto const loc = osr::location{};
  auto p = prima{"prima_url", loc, loc, motis::api::plan_params{}};
  p.fixed_ = n::event_type::kDep;
  p.cap_ = {.wheelchairs_ = 1, .bikes_ = 0, .passengers_ = 1, .luggage_ = 0};
  p.first_mile_taxi_ = {
      {get_loc_idx("A"), n::duration_t{60min}, motis::kOdmTransportModeId},
      {get_loc_idx("B"), n::duration_t{60min}, motis::kOdmTransportModeId}};
  p.last_mile_taxi_ = {
      {get_loc_idx("C"), n::duration_t{60min}, motis::kOdmTransportModeId},
      {get_loc_idx("D"), n::duration_t{60min}, motis::kOdmTransportModeId}};

  EXPECT_EQ(p.make_blacklist_taxi_request(
                tt, {n::unixtime_t{0h}, n::unixtime_t{48h}}),
            blacklist_request);

  EXPECT_FALSE(p.consume_blacklist_taxi_response(invalid_response));
  EXPECT_TRUE(p.consume_blacklist_taxi_response(blacklist_response));

  ASSERT_EQ(p.first_mile_taxi_.size(), 2);
  EXPECT_EQ(p.first_mile_taxi_[0].target_, get_loc_idx("A"));
  ASSERT_EQ(p.first_mile_taxi_times_[0].size(), 1);
  EXPECT_EQ(p.first_mile_taxi_times_[0][0].from_, to_unix(32400000));
  EXPECT_EQ(p.first_mile_taxi_times_[0][0].to_, to_unix(43200000));
  EXPECT_EQ(p.first_mile_taxi_[1].target_, get_loc_idx("B"));
  ASSERT_EQ(p.first_mile_taxi_times_[1].size(), 1);
  EXPECT_EQ(p.first_mile_taxi_times_[1][0].from_, to_unix(43200000));
  EXPECT_EQ(p.first_mile_taxi_times_[1][0].to_, to_unix(64800000));

  ASSERT_EQ(p.last_mile_taxi_.size(), 2);
  EXPECT_EQ(p.last_mile_taxi_[0].target_, get_loc_idx("C"));
  ASSERT_EQ(p.last_mile_taxi_times_[0].size(), 1);
  EXPECT_EQ(p.last_mile_taxi_times_[0][0].from_, to_unix(43200000));
  EXPECT_EQ(p.last_mile_taxi_times_[0][0].to_, to_unix(64800000));
  EXPECT_EQ(p.last_mile_taxi_[1].target_, get_loc_idx("D"));
  EXPECT_EQ(p.last_mile_taxi_times_[1].size(), 0);
}
