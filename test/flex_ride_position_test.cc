#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include "fmt/format.h"

#include "gtest/gtest.h"

#include "utl/enumerate.h"
#include "utl/helpers/algorithm.h"
#include "utl/init_from.h"
#include "utl/overloaded.h"
#include "utl/to_vec.h"

#include "geo/latlng.h"

#include "nigiri/footpath.h"
#include "nigiri/timetable.h"

#include "osr/routing/route.h"

#include "motis/config.h"
#include "motis/data.h"
#include "motis/endpoints/routing.h"
#include "motis/flex/flex.h"
#include "motis/flex/flex_areas.h"
#include "motis/flex/flex_routing_data.h"
#include "motis/import.h"
#include "motis/match_platforms.h"
#include "motis/osr/max_distance.h"
#include "motis/osr/one_to_many_searches.h"
#include "motis/osr/parameters.h"
#include "motis/place.h"
#include "motis/transport_mode.h"

using namespace motis;
using namespace date;
namespace fs = std::filesystem;

// End to end: the FLEX leg the API prints starts inside the pickup window,
// for rides from the real osr chain (walk to the vehicle first, so the ride
// position and its flooring in add_flex_td_offsets matter).
//
// test_case.osm.pbf has a short drivable stretch west of DA Hbf: from
// Lise-Meitner-Straße (da_south) north to Dornheimer Weg (da_north). From
// Traubenweg (kTraubenweg, outside both areas) it is a ~6 min walk to
// da_south; from DA_10 ~12 min to da_north. Direct trips run from the west
// end of Lise-Meitner-Straße (kLiseMeitner, outside da_south) to Dornheimer
// Weg (kDornheimerWeg): ~7 min on foot, ~3.5 min with the ride.
//   FLEX_FM: pickup da_south -> drop-off da_north (first mile to DA_10)
//   FLEX_LM: pickup da_north -> drop-off da_south (last mile from DA_10)
// Both with pickup window {0} and drop-off window {1} (local time).
// ICE_OUT DA_10 11:00 -> FFM_10 12:00, ICE_IN FFM_10 08:40 -> DA_10 09:40
// (Europe/Berlin, UTC+2 in May).
constexpr auto kRidePositionGtfs = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_lat,stop_lon,location_type,parent_station,platform_code
DA,DA Hbf,49.87260,8.63085,1,,
DA_10,DA Hbf,49.87336,8.62926,0,DA,10
FFM,FFM Hbf,50.10701,8.66341,1,,
FFM_10,FFM Hbf,50.10593,8.66118,0,FFM,10

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type,route_color,route_text_color
ICE,DB,ICE,,,101,,
FLEX,DB,FlexDA,Flex Darmstadt,,715,FDB813,000000

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id
ICE,S_ALL,ICE_OUT,,
ICE,S_ALL,ICE_IN,,
FLEX,S_ALL,FLEX_FM,,
FLEX,S_ALL,FLEX_LM,,

# booking_rules.txt
booking_rule_id,booking_type,prior_notice_duration_min,prior_notice_duration_max
BR,1,0,86400

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,location_group_id,location_id,stop_sequence,start_pickup_drop_off_window,end_pickup_drop_off_window,pickup_booking_rule_id,drop_off_booking_rule_id,pickup_type,drop_off_type
ICE_OUT,11:00:00,11:00:00,DA_10,,,0,,,,,0,0
ICE_OUT,12:00:00,12:00:00,FFM_10,,,1,,,,,0,0
ICE_IN,08:40:00,08:40:00,FFM_10,,,0,,,,,0,0
ICE_IN,09:40:00,09:40:00,DA_10,,,1,,,,,0,0
FLEX_FM,,,,,da_south,0,{0},BR,,2,1
FLEX_FM,,,,,da_north,1,{1},,BR,1,2
FLEX_LM,,,,,da_north,0,{0},BR,,2,1
FLEX_LM,,,,,da_south,1,{1},,BR,1,2

# calendar.txt
service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,start_date,end_date
S_ALL,1,1,1,1,1,1,1,20190501,20190503

# locations.geojson
{{"type":"FeatureCollection","features":[{{"id":"da_south","type":"Feature","geometry":{{"type":"Polygon","coordinates":[[[8.6270,49.8715],[8.6290,49.8715],[8.6290,49.8745],[8.6270,49.8745],[8.6270,49.8715]]]}},"properties":{{"stop_name":"DA South"}}}},{{"id":"da_north","type":"Feature","geometry":{{"type":"Polygon","coordinates":[[[8.6268,49.8747],[8.6286,49.8747],[8.6286,49.8760],[8.6268,49.8760],[8.6268,49.8747]]]}},"properties":{{"stop_name":"DA North"}}}}]}}
)";

constexpr auto kTraubenweg = "49.87331,8.62300";
constexpr auto kFfm10 = "50.10593,8.66118";
constexpr auto kLiseMeitner = "49.87201,8.62635";
constexpr auto kDornheimerWeg = "49.87531,8.62748";

namespace {

struct fixture {
  fixture(std::string_view const sub_dir,
          std::string_view const pickup,
          std::string_view const drop_off) {
    auto ec = std::error_code{};
    auto const path = fs::path{"test/data/flex_ride_position"} / sub_dir;
    fs::remove_all(path, ec);
    cfg_ = config{.osm_ = {"test/resources/test_case.osm.pbf"},
                  .timetable_ =
                      config::timetable{
                          .first_day_ = "2019-05-01",
                          .num_days_ = 3,
                          .datasets_ = {{"test",
                                         {.path_ = fmt::format(
                                              fmt::runtime(kRidePositionGtfs),
                                              pickup, drop_off)}}}},
                  .street_routing_ = true,
                  .osr_footpath_ = true};
    import(cfg_, path);
    d_ = std::make_unique<data>(path, cfg_);
  }

  api::plan_response plan(std::string const& query) const {
    return utl::init_from<ep::routing>(*d_).value()(query);
  }

  config cfg_;
  std::unique_ptr<data> d_;
};

std::chrono::sys_seconds utc(int const day, int const h, int const m) {
  return sys_days{2019_y / May / day} + std::chrono::hours{h} +
         std::chrono::minutes{m};
}

// Time of day (UTC): the search also returns the next days' connections.
std::chrono::minutes utc_minute_of_day(openapi::date_time_t const& t) {
  return std::chrono::duration_cast<std::chrono::minutes>(
      *t - std::chrono::floor<date::days>(*t));
}

constexpr auto hm(int const h, int const m) {
  return std::chrono::hours{h} + std::chrono::minutes{m};
}

bool any_on_day(
    std::vector<std::pair<api::Leg const*, api::Leg const*>> const& legs,
    int const day) {
  return std::any_of(begin(legs), end(legs), [&](auto const& x) {
    return std::chrono::floor<date::days>(*x.first->startTime_) ==
           sys_days{2019_y / May / day};
  });
}

// All FLEX legs of the given connections, with the leg before each. The
// pointers refer into `connections`, which has to outlive them.
std::vector<std::pair<api::Leg const*, api::Leg const*>> flex_legs(
    std::vector<api::Itinerary> const& connections) {
  auto ret = std::vector<std::pair<api::Leg const*, api::Leg const*>>{};
  for (auto const& j : connections) {
    for (auto i = 0U; i != j.legs_.size(); ++i) {
      if (j.legs_[i].mode_ == api::ModeEnum::FLEX) {
        ret.emplace_back(&j.legs_[i], i == 0U ? nullptr : &j.legs_[i - 1U]);
      }
    }
  }
  return ret;
}

std::string first_mile(std::string_view const time,
                       std::string_view const from = kTraubenweg) {
  return fmt::format(
      "/api/v6/plan?fromPlace={}&toPlace={}&time={}"
      "&preTransitModes=FLEX&maxPreTransitTime=3600&postTransitModes=WALK"
      "&directModes=WALK",
      from, kFfm10, time);
}

std::string last_mile(std::string_view const time) {
  return fmt::format(
      "/api/v6/plan?fromPlace={}&toPlace={}&time={}"
      "&preTransitModes=WALK&postTransitModes=FLEX&maxPostTransitTime=3600"
      "&directModes=WALK",
      kFfm10, kTraubenweg, time);
}

std::string direct(std::string_view const time) {
  return fmt::format(
      "/api/v6/plan?fromPlace={}&toPlace={}&time={}"
      "&directModes=FLEX&maxDirectTime=3600",
      kLiseMeitner, kDornheimerWeg, time);
}

}  // namespace

// [T, T] pickup at 10:10 local (08:10Z), drop-off 10:10-28:59.
TEST(motis, flex_ride_position_fixed_departure) {
  auto const f = fixture{"fixed", "10:10:00,10:10:00", "10:10:00,28:59:00"};

  // First mile: walk to da_south, ride exactly at 08:10Z, ICE_OUT at 09:00Z.
  // Several starts along the street: different walks to the vehicle, so the
  // pickup offset lands on different seconds within its minute.
  for (auto const from :
       {kTraubenweg, "49.87330,8.62340", "49.87329,8.62382", "49.87328,8.62420",
        "49.87327,8.62460", "49.87326,8.62490"}) {
    SCOPED_TRACE(from);
    auto const res = f.plan(first_mile("2019-05-01T07:30Z", from));
    auto const legs = flex_legs(res.itineraries_);
    ASSERT_FALSE(legs.empty());
    EXPECT_TRUE(any_on_day(legs, 1));
    for (auto const& [flex, before] : legs) {
      EXPECT_EQ(hm(8, 10), utc_minute_of_day(flex->startTime_));
      EXPECT_EQ("da_south", flex->from_.flexId_.value_or("-"));
      EXPECT_EQ("da_north", flex->to_.flexId_.value_or("-"));
      // Identified like a transit leg, but not taken for one.
      EXPECT_EQ("Deutsche Bahn", flex->agencyName_.value_or("-"));
      EXPECT_EQ("https://deutschebahn.com", flex->agencyUrl_.value_or("-"));
      EXPECT_EQ("test_FLEX", flex->routeId_.value_or("-"));
      EXPECT_EQ("FlexDA", flex->routeShortName_.value_or("-"));
      EXPECT_EQ("Flex Darmstadt", flex->routeLongName_.value_or("-"));
      EXPECT_EQ(715, flex->routeType_.value_or(-1));
      EXPECT_EQ("fdb813", flex->routeColor_.value_or("-"));
      EXPECT_EQ("000000", flex->routeTextColor_.value_or("-"));
      EXPECT_FALSE(flex->displayName_.has_value());
      EXPECT_FALSE(flex->tripId_.has_value());
      ASSERT_NE(nullptr, before);
      EXPECT_EQ(api::ModeEnum::WALK, before->mode_);
      EXPECT_EQ(hm(8, 10), utc_minute_of_day(before->endTime_));
      EXPECT_LT(*before->startTime_, *before->endTime_);  // walk first
    }
  }

  // Last mile: ICE_IN at DA_10 07:40Z, walk to da_north, ride at 08:10Z.
  {
    auto const res = f.plan(last_mile("2019-05-01T06:30Z"));
    auto const legs = flex_legs(res.itineraries_);
    ASSERT_FALSE(legs.empty());
    EXPECT_TRUE(any_on_day(legs, 1));
    for (auto const& [flex, before] : legs) {
      EXPECT_EQ(hm(8, 10), utc_minute_of_day(flex->startTime_));
      EXPECT_EQ("da_north", flex->from_.flexId_.value_or("-"));
      EXPECT_EQ("da_south", flex->to_.flexId_.value_or("-"));
      ASSERT_NE(nullptr, before);
      EXPECT_EQ(hm(8, 10), utc_minute_of_day(before->endTime_));
    }
  }

  // Direct: moved to the fixed departure; after it, the next day's.
  {
    auto const res = f.plan(direct("2019-05-01T07:30Z"));
    auto const legs = flex_legs(res.direct_);
    ASSERT_EQ(1U, legs.size());
    EXPECT_EQ(utc(1, 8, 10), *legs.front().first->startTime_);
  }
  {
    auto const res = f.plan(direct("2019-05-01T08:11Z"));
    auto const legs = flex_legs(res.direct_);
    ASSERT_EQ(1U, legs.size());
    EXPECT_EQ(utc(2, 8, 10), *legs.front().first->startTime_);
  }
}

// Regular window: pickup 10:00-10:40 local (08:00-08:40Z).
TEST(motis, flex_ride_position_regular_window) {
  auto const f = fixture{"regular", "10:00:00,10:40:00", "10:00:00,13:00:00"};

  // First mile: the ride starts inside the window (the latest departure
  // that still catches ICE_OUT is chosen, so anywhere before 08:40Z).
  {
    auto const res = f.plan(first_mile("2019-05-01T07:30Z"));
    auto const legs = flex_legs(res.itineraries_);
    ASSERT_FALSE(legs.empty());
    EXPECT_TRUE(any_on_day(legs, 1));
    for (auto const& [flex, before] : legs) {
      EXPECT_GE(utc_minute_of_day(flex->startTime_), hm(8, 0));
      EXPECT_LT(utc_minute_of_day(flex->startTime_), hm(8, 40));
    }
  }

  // Last mile: arriving 07:40Z, the ride starts after the walk, in the
  // window.
  {
    auto const res = f.plan(last_mile("2019-05-01T06:30Z"));
    auto const legs = flex_legs(res.itineraries_);
    ASSERT_FALSE(legs.empty());
    EXPECT_TRUE(any_on_day(legs, 1));
    for (auto const& [flex, before] : legs) {
      EXPECT_GE(utc_minute_of_day(flex->startTime_), hm(8, 0));
      EXPECT_LT(utc_minute_of_day(flex->startTime_), hm(8, 40));
    }
  }

  // Direct, depart 09:50 local: walk first, the ride starts when the
  // window opens, 10:00 local.
  {
    auto const res = f.plan(direct("2019-05-01T07:50Z"));
    auto const legs = flex_legs(res.direct_);
    ASSERT_EQ(1U, legs.size());
    EXPECT_EQ(utc(1, 8, 0), *legs.front().first->startTime_);
  }

  // Direct, depart 10:41 local: no ride that day, the next day's window.
  {
    auto const res = f.plan(direct("2019-05-01T08:41Z"));
    auto const legs = flex_legs(res.direct_);
    ASSERT_EQ(1U, legs.size());
    EXPECT_EQ(utc(2, 8, 0), *legs.front().first->startTime_);
  }
}

// The duration of a FLEX offset is the osr travel time rounded up to whole
// minutes, like every other offset: an offset promises that the stop can be
// reached after this time. Truncating made FLEX up to a minute optimistic and
// let a walk-only car_sharing path beat the WALK offset for the same stop.
// Checked against the paths add_flex_td_offsets keeps for reconstruction, for
// the first and the last mile from several positions along Traubenweg (their
// walks to the vehicle end on different seconds).
TEST(motis, flex_offset_duration_rounded_up) {
  auto const f = fixture{"rounding", "10:00:00,10:40:00", "10:00:00,13:00:00"};
  auto const r = utl::init_from<ep::routing>(*f.d_).value();

  auto n_offsets = 0U;
  auto n_fractional = 0U;  // paths whose duration is no whole minute
  auto const check = [&](geo::latlng const& pos, osr::direction const dir) {
    auto frd = flex::flex_routing_data{};
    auto offsets = nigiri::routing::td_offsets_t{};
    auto stats = std::map<std::string, std::uint64_t>{};
    auto states = one_to_many_side{};
    flex::add_flex_td_offsets(
        *r.w_, *r.l_, r.pl_, r.matches_, r.way_matches_, *r.tt_, *r.fa_,
        *r.loc_tree_,
        nigiri::unixtime_t{sys_days{2019_y / May / 1} + hm(7, 30)},
        osr::location{pos, osr::kNoLevel}, dir, std::chrono::hours{1}, 250.0,
        osr_parameters{}, frd, offsets, stats, &states);
    EXPECT_FALSE(offsets.empty());

    for (auto const& [l, v] : offsets) {
      for (auto const& o : v) {
        if (o.duration_ == nigiri::footpath::kMaxDuration) {
          continue;  // closes a window, no path behind it
        }
        auto const search = states.by_mode_.find(
            transport_mode_t{o.transport_mode_, o.transport_mode_payload_});
        ASSERT_NE(states.by_mode_.end(), search);
        auto const& s = states.searches_[search->second.search_idx_];
        auto const dest = s.dest_idx_.find(l);
        ASSERT_NE(s.dest_idx_.end(), dest);
        auto const& p =
            s.state_->results()[search->second.offset_ + dest->second];
        ASSERT_TRUE(p.has_value());

        auto const seconds = static_cast<int>(p->duration_.count());
        EXPECT_EQ((seconds + 59) / 60, static_cast<int>(o.duration_.count()))
            << "path of " << seconds << " s, offset " << o.duration_.count()
            << " min";
        ++n_offsets;
        if (seconds % 60 != 0) {
          ++n_fractional;
        }
      }
    }
  };

  for (auto const& pos :
       {geo::latlng{49.87331, 8.62300}, geo::latlng{49.87330, 8.62340},
        geo::latlng{49.87329, 8.62382}, geo::latlng{49.87328, 8.62420},
        geo::latlng{49.87327, 8.62460}, geo::latlng{49.87326, 8.62490}}) {
    check(pos, osr::direction::kForward);
    check(pos, osr::direction::kBackward);
  }
  EXPECT_GT(n_offsets, 0U);
  // Otherwise rounding up and truncating agree and the test proves nothing.
  EXPECT_GT(n_fractional, 0U);
}

// One street routing per stop at the query position's side of the ride, the
// stops at the other side as osr classes (flex::flex_offset_routing), has to
// give exactly the offsets of one routing per (boarding, alighting) pair.
//
// Around FFM Hbf: ffm_area is split into ffm_west and ffm_east; ffm_group is
// the stop at Münchener Straße. From ffm_area the rides go to all three, with
// different windows, and the three-row MULTI adds ffm_west -> ffm_east; the
// R_ trips run the other way for the last mile. `n_groups` more location
// groups (all with the stop at Münchener Straße) exceed the classes one osr
// search holds.
namespace {

constexpr auto kClassesGtfsHead = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_lat,stop_lon,location_type,parent_station,platform_code
DA,DA Hbf,49.87260,8.63085,1,,
DA_10,DA Hbf,49.87336,8.62926,0,DA,10
FFM,FFM Hbf,50.10701,8.66341,1,,
FFM_10,FFM Hbf,50.10593,8.66118,0,FFM,10
FFM_FLEX,Münchener Straße,50.10830,8.67165,0,,

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
ICE,DB,ICE,,,101
FLEX,DB,FlexFFM,,,715

# booking_rules.txt
booking_rule_id,booking_type,prior_notice_duration_min,prior_notice_duration_max
BR,1,0,86400

# calendar.txt
service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,start_date,end_date
S_ALL,1,1,1,1,1,1,1,20190501,20190503

# locations.geojson
{"type":"FeatureCollection","features":[{"id":"ffm_area","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[8.655,50.100],[8.672,50.100],[8.672,50.112],[8.655,50.112],[8.655,50.100]]]},"properties":{"stop_name":"FFM Flex Area"}},{"id":"ffm_west","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[8.655,50.100],[8.663,50.100],[8.663,50.112],[8.655,50.112],[8.655,50.100]]]},"properties":{"stop_name":"FFM West"}},{"id":"ffm_east","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[8.663,50.100],[8.672,50.100],[8.672,50.112],[8.663,50.112],[8.663,50.100]]]},"properties":{"stop_name":"FFM East"}}]}
)";

constexpr auto kClassesStopTimes =
    R"(ICE_OUT,10:30:00,10:30:00,FFM_10,,,0,,,,,0,0
ICE_OUT,11:00:00,11:00:00,DA_10,,,1,,,,,0,0
ICE_IN,08:30:00,08:30:00,DA_10,,,0,,,,,0,0
ICE_IN,09:00:00,09:00:00,FFM_10,,,1,,,,,0,0
EAST,,,,,ffm_area,0,08:00:00,12:00:00,BR,BR,2,2
EAST,,,,,ffm_east,1,08:00:00,11:00:00,BR,BR,2,2
WEST,,,,,ffm_area,0,08:00:00,12:00:00,BR,BR,2,2
WEST,,,,,ffm_west,1,10:00:00,13:00:00,BR,BR,2,2
GROUP,,,,,ffm_area,0,08:00:00,14:00:00,BR,BR,2,2
GROUP,,,,ffm_group,,1,08:00:00,14:00:00,BR,BR,2,2
MULTI,,,,,ffm_area,0,08:00:00,10:30:00,BR,BR,2,2
MULTI,,,,,ffm_west,1,08:00:00,10:00:00,BR,BR,2,2
MULTI,,,,,ffm_east,2,10:00:00,11:30:00,BR,BR,2,2
R_EAST,,,,,ffm_east,0,08:00:00,12:00:00,BR,BR,2,2
R_EAST,,,,,ffm_area,1,08:00:00,12:00:00,BR,BR,2,2
R_WEST,,,,,ffm_west,0,09:00:00,11:00:00,BR,BR,2,2
R_WEST,,,,,ffm_area,1,09:00:00,13:00:00,BR,BR,2,2
R_GROUP,,,,ffm_group,,0,08:00:00,14:00:00,BR,BR,2,2
R_GROUP,,,,,ffm_area,1,08:00:00,14:00:00,BR,BR,2,2
R_MULTI,,,,,ffm_east,0,08:00:00,09:30:00,BR,BR,2,2
R_MULTI,,,,,ffm_west,1,09:00:00,10:30:00,BR,BR,2,2
R_MULTI,,,,,ffm_area,2,10:00:00,12:00:00,BR,BR,2,2
)";

std::string classes_gtfs(unsigned const n_groups) {
  auto gtfs = std::string{kClassesGtfsHead};
  auto trips = std::string{
      "\n# trips.txt\nroute_id,service_id,trip_id,trip_headsign,block_id\n"};
  for (auto const* t : {"ICE_OUT", "ICE_IN", "EAST", "WEST", "GROUP", "MULTI",
                        "R_EAST", "R_WEST", "R_GROUP", "R_MULTI"}) {
    fmt::format_to(std::back_inserter(trips), "{},S_ALL,{},,\n",
                   std::string_view{t}.starts_with("ICE") ? "ICE" : "FLEX", t);
  }
  auto stop_times = std::string{
      "\n# stop_times.txt\n"
      "trip_id,arrival_time,departure_time,stop_id,location_group_id,"
      "location_id,stop_sequence,start_pickup_drop_off_window,"
      "end_pickup_drop_off_window,pickup_booking_rule_id,"
      "drop_off_booking_rule_id,pickup_type,drop_off_type\n"};
  stop_times += kClassesStopTimes;
  auto groups = std::string{
      "\n# location_groups.txt\nlocation_group_id,location_group_name\n"
      "ffm_group,FFM Flex Group\n"};
  auto group_stops = std::string{
      "\n# location_group_stops.txt\nlocation_group_id,stop_id\n"
      "ffm_group,FFM_FLEX\n"};
  for (auto i = 0U; i != n_groups; ++i) {
    fmt::format_to(std::back_inserter(trips),
                   "FLEX,S_ALL,M_{0},,\n"
                   "FLEX,S_ALL,RM_{0},,\n",
                   i);
    fmt::format_to(std::back_inserter(stop_times),
                   "M_{0},,,,,ffm_area,0,08:00:00,14:00:00,BR,BR,2,2\n"
                   "M_{0},,,,g_{0},,1,08:00:00,14:00:00,BR,BR,2,2\n"
                   "RM_{0},,,,g_{0},,0,08:00:00,14:00:00,BR,BR,2,2\n"
                   "RM_{0},,,,,ffm_area,1,08:00:00,14:00:00,BR,BR,2,2\n",
                   i);
    fmt::format_to(std::back_inserter(groups), "g_{0},Group {0}\n", i);
    fmt::format_to(std::back_inserter(group_stops), "g_{},FFM_FLEX\n", i);
  }
  return gtfs + trips + stop_times + groups + group_stops;
}

struct classes_fixture {
  classes_fixture(std::string_view const sub_dir, unsigned const n_groups) {
    auto ec = std::error_code{};
    auto const path = fs::path{"test/data/flex_offset_routing"} / sub_dir;
    fs::remove_all(path, ec);
    cfg_ = config{
        .osm_ = {"test/resources/test_case.osm.pbf"},
        .timetable_ =
            config::timetable{
                .first_day_ = "2019-05-01",
                .num_days_ = 3,
                .datasets_ = {{"test", {.path_ = classes_gtfs(n_groups)}}}},
        .street_routing_ = true,
        .osr_footpath_ = true};
    import(cfg_, path);
    d_ = std::make_unique<data>(path, cfg_);
  }

  config cfg_;
  std::unique_ptr<data> d_;
};

constexpr auto kClassesStart =
    nigiri::unixtime_t{sys_days{2019_y / May / 1} + hm(7, 30)};
constexpr auto kClassesMax = std::chrono::seconds{3600};
constexpr auto kClassesMatchingDistance = 250.0;

constexpr auto kFfmPositions =
    std::array{geo::latlng{50.1040, 8.6600}, geo::latlng{50.1100, 8.6700},
               geo::latlng{50.1065, 8.6585}, geo::latlng{50.1083, 8.6716}};

using offset_row = std::tuple<nigiri::location_idx_t::value_t,
                              nigiri::unixtime_t::rep,
                              nigiri::duration_t::rep,
                              transport_mode_t::mode_t,
                              transport_mode_t::payload_t>;

std::vector<offset_row> to_rows(nigiri::routing::td_offsets_t const& offsets) {
  auto ret = std::vector<offset_row>{};
  for (auto const& [l, v] : offsets) {
    for (auto const& o : v) {
      ret.emplace_back(to_idx(l), o.valid_from_.time_since_epoch().count(),
                       o.duration_.count(), o.transport_mode_,
                       o.transport_mode_payload_);
    }
  }
  std::ranges::sort(ret);
  return ret;
}

// What add_flex_td_offsets computed before its routings were classed: one
// car_sharing search per (boarding, alighting) pair of get_flex_routings.
nigiri::routing::td_offsets_t per_pair_offsets(ep::routing const& r,
                                               geo::latlng const& pos,
                                               osr::direction const dir) {
  namespace n = nigiri;
  auto const& tt = *r.tt_;
  auto const osr_params = osr_parameters{};
  auto const params =
      to_profile_parameters(osr::search_profile::kCarSharing, osr_params);
  auto const loc = osr::location{pos, osr::kNoLevel};
  auto const near_stops = r.loc_tree_->in_radius(
      pos, get_max_distance(osr::search_profile::kCarSharing, osr_params,
                            kClassesMax));
  auto const near_stop_locations =
      utl::to_vec(near_stops, [&](n::location_idx_t const l) {
        return get_location(&tt, r.w_, r.pl_, r.matches_, tt_location{l});
      });
  auto pos_match = osr::match_result{};
  r.l_->match(params, loc, false, dir, kClassesMatchingDistance, nullptr,
              osr::search_profile::kCarSharing, {}, pos_match);
  auto const near_stop_matches = get_reverse_platform_way_matches(
      *r.l_, r.way_matches_, osr::search_profile::kCarSharing, near_stops,
      near_stop_locations, dir, kClassesMatchingDistance);

  // get_relevant_days in flex.cc
  auto const t_day = std::chrono::floor<date::days>(kClassesStart);
  auto const first_day = tt.day_idx(t_day - date::days{2});
  auto const end_day = tt.day_idx(t_day + date::days{3});

  auto ret = n::routing::td_offsets_t{};
  for (auto const& [_, ids] :
       flex::get_flex_routings(tt, *r.loc_tree_, kClassesStart, pos, dir,
                               kClassesMax, osr_params)) {
    auto frd = flex::flex_routing_data{};
    auto const sharing = flex::prepare_sharing_data(
        tt, *r.w_, *r.l_, r.pl_, *r.fa_, r.matches_, ids.front(), frd);
    auto const state = osr::route_one_to_many(
        params, *r.w_, *r.l_, osr::search_profile::kCarSharing, loc,
        near_stop_locations, pos_match[osr::match_idx_t{0U}], near_stop_matches,
        kClassesMax, dir, nullptr, &sharing, nullptr);
    for (auto const [i, l] : utl::enumerate(near_stops)) {
      auto const& p = state->results()[i];
      auto const rental = state->rental_durations(i);
      if (!p.has_value() || !rental.has_value()) {
        continue;
      }
      auto const ride = flex::flex_ride{
          .pickup_ =
              std::chrono::duration_cast<n::duration_t>(rental->ride_start_),
          .drop_off_ =
              std::chrono::duration_cast<n::duration_t>(rental->ride_end_)};
      auto const duration = n::duration_t{static_cast<n::duration_t::rep>(
          std::ceil(p->duration_.count() / 60.0))};
      for (auto const id : ids) {
        auto const& traffic_days =
            tt.bitfields_
                [tt.flex_transport_traffic_days_[id.get_flex_transport()]];
        for (auto d = to_idx(first_day); d != to_idx(end_day); ++d) {
          if (!traffic_days.test(d)) {
            continue;
          }
          auto const day = tt.internal_interval().from_ + d * date::days{1U};
          auto const dep_iv = flex::get_departure_window(tt, id, day, ride);
          if (dep_iv.from_ < dep_iv.to_ &&
              duration < n::footpath::kMaxDuration) {
            auto const mode =
                transport_mode(api::ModeEnum::FLEX, id.to_payload());
            ret[l].push_back(
                n::routing::td_offset::make(dep_iv.from_, duration, mode));
            ret[l].push_back(n::routing::td_offset::make(
                dep_iv.to_, n::footpath::kMaxDuration, mode));
          }
        }
      }
    }
  }
  return ret;
}

bool far_stop_contains(nigiri::timetable const& tt,
                       flex::flex_areas const& fa,
                       nigiri::flex_stop_t const s,
                       geo::latlng const& p) {
  return s.apply(utl::overloaded{
      [&](nigiri::flex_area_idx_t const a) { return fa.is_in_area(a, p); },
      [&](nigiri::location_group_idx_t const g) {
        return utl::any_of(tt.location_group_locations_[g],
                           [&](nigiri::location_idx_t const l) {
                             return geo::distance(tt.locations_.coordinates_[l],
                                                  p) < 1.0;
                           });
      }});
}

struct classes_outcome {
  unsigned n_offsets_{0U}, n_classed_{0U}, n_far_class_{0U};
};

// Compares the classed offsets with the per-pair ones, and checks every
// offer's path: kept for reconstruction under its mode, laid out again from
// the stored additional nodes and classes, and the vehicle is left (forward)
// or taken (backward) in the offer's far stop.
classes_outcome check_classed_offsets(ep::routing const& r,
                                      geo::latlng const& pos,
                                      osr::direction const dir) {
  auto const& tt = *r.tt_;
  auto frd = flex::flex_routing_data{};
  auto offsets = nigiri::routing::td_offsets_t{};
  auto stats = std::map<std::string, std::uint64_t>{};
  auto states = one_to_many_side{};
  flex::add_flex_td_offsets(
      *r.w_, *r.l_, r.pl_, r.matches_, r.way_matches_, tt, *r.fa_, *r.loc_tree_,
      kClassesStart, osr::location{pos, osr::kNoLevel}, dir, kClassesMax,
      kClassesMatchingDistance, osr_parameters{}, frd, offsets, stats, &states);
  EXPECT_EQ(to_rows(per_pair_offsets(r, pos, dir)), to_rows(offsets));

  auto out = classes_outcome{};
  for (auto const& [l, v] : offsets) {
    for (auto const& o : v) {
      if (o.duration_ == nigiri::footpath::kMaxDuration) {
        continue;  // closes a window
      }
      ++out.n_offsets_;
      auto const search = states.by_mode_.find(o.mode());
      EXPECT_NE(states.by_mode_.end(), search);
      if (search == states.by_mode_.end()) {
        continue;
      }
      auto const& s = states.searches_[search->second.search_idx_];
      auto const dest = s.dest_idx_.find(l);
      EXPECT_NE(s.dest_idx_.end(), dest);
      if (dest == s.dest_idx_.end()) {
        continue;
      }
      auto const k = search->second.offset_ + dest->second;
      auto const& p = s.state_->results()[k];
      EXPECT_TRUE(p.has_value());
      if (!p.has_value()) {
        continue;
      }
      EXPECT_EQ((p->duration_.count() + 59) / 60, o.duration_.count());
      if (s.flex_additional_nodes_.n_classes_ > 1U) {
        ++out.n_classed_;
      }
      if (search->second.offset_ != 0U) {
        ++out.n_far_class_;
      }

      auto const sharing = s.flex_additional_nodes_.to_sharing_data();
      auto const reconstructed =
          s.state_->reconstruct(*r.w_, *r.l_, k, &sharing);
      EXPECT_TRUE(reconstructed.has_value());
      if (!reconstructed.has_value()) {
        continue;
      }
      EXPECT_EQ(p->duration_, reconstructed->duration_);

      auto const id = flex::mode_payload{o.transport_mode_payload_};
      auto const stop_seq =
          tt.flex_stop_seq_
              [tt.flex_transport_stop_seq_[id.get_flex_transport()]];
      auto const& segments = reconstructed->segments_;
      auto const is_car = [](osr::path::segment const& x) {
        return x.mode_ == osr::mode::kCar;
      };
      if (dir == osr::direction::kForward) {
        auto const last =
            std::find_if(rbegin(segments), rend(segments), is_car);
        EXPECT_NE(rend(segments), last);
        if (last != rend(segments) && !last->polyline_.empty()) {
          EXPECT_TRUE(far_stop_contains(tt, *r.fa_, stop_seq[id.get_to_stop()],
                                        last->polyline_.back()))
              << last->polyline_.back().lat_ << ","
              << last->polyline_.back().lng_;
        }
      } else {
        auto const first = utl::find_if(segments, is_car);
        EXPECT_NE(end(segments), first);
        if (first != end(segments) && !first->polyline_.empty()) {
          EXPECT_TRUE(far_stop_contains(tt, *r.fa_,
                                        stop_seq[id.get_from_stop()],
                                        first->polyline_.front()))
              << first->polyline_.front().lat_ << ","
              << first->polyline_.front().lng_;
        }
      }
    }
  }
  return out;
}

}  // namespace

TEST(motis, flex_offset_routing_matches_pairs) {
  auto const f = classes_fixture{"pairs", 0U};
  auto const r = utl::init_from<ep::routing>(*f.d_).value();

  for (auto const dir : {osr::direction::kForward, osr::direction::kBackward}) {
    SCOPED_TRACE(std::string{osr::to_str(dir)});
    auto out = classes_outcome{};
    for (auto const& pos : kFfmPositions) {
      SCOPED_TRACE(fmt::format("{},{}", pos.lat_, pos.lng_));
      auto const o = check_classed_offsets(r, pos, dir);
      out.n_offsets_ += o.n_offsets_;
      out.n_classed_ += o.n_classed_;
      out.n_far_class_ += o.n_far_class_;
    }
    // Offers have to come from searches with several classes, and from
    // classes other than the first, for the comparison to mean anything.
    EXPECT_GT(out.n_offsets_, 0U);
    EXPECT_GT(out.n_classed_, 0U);
    EXPECT_GT(out.n_far_class_, 0U);
  }

  // The plan endpoint lays out FLEX legs from the classed searches.
  auto const plan = [&](std::string const& q) {
    return utl::init_from<ep::routing>(*f.d_).value()(q);
  };
  auto const first = plan(fmt::format(
      "/api/v6/plan?fromPlace={}&toPlace=49.87336,8.62926&time="
      "2019-05-01T07:30Z&preTransitModes=FLEX&maxPreTransitTime=3600"
      "&postTransitModes=WALK&directModes=WALK",
      fmt::format("{},{}", kFfmPositions[0].lat_, kFfmPositions[0].lng_)));
  EXPECT_FALSE(flex_legs(first.itineraries_).empty());
  auto const last = plan(fmt::format(
      "/api/v6/plan?fromPlace=49.87336,8.62926&toPlace={}&time="
      "2019-05-01T06:00Z&preTransitModes=WALK&postTransitModes=FLEX"
      "&maxPostTransitTime=3600&directModes=WALK",
      fmt::format("{},{}", kFfmPositions[1].lat_, kFfmPositions[1].lng_)));
  EXPECT_FALSE(flex_legs(last.itineraries_).empty());
}

// More far stops than one osr search has classes: the rest go into a second
// search for the same stop, with the same offsets.
TEST(motis, flex_offset_routing_many_far_stops) {
  auto const f = classes_fixture{"many", 70U};
  auto const r = utl::init_from<ep::routing>(*f.d_).value();
  auto const& tt = *r.tt_;
  auto const pos = kFfmPositions[3];

  for (auto const dir : {osr::direction::kForward, osr::direction::kBackward}) {
    SCOPED_TRACE(std::string{osr::to_str(dir)});
    auto const routings = flex::get_flex_offset_routings(
        tt,
        flex::get_flex_routings(tt, *r.loc_tree_, kClassesStart, pos, dir,
                                kClassesMax, osr_parameters{}),
        dir);
    auto n_far = std::vector<std::size_t>{};
    for (auto const& x : routings) {
      EXPECT_LE(x.far_.size(), osr::kMaxSharingClasses);
      EXPECT_EQ(x.far_.size(), x.transports_.size());
      n_far.push_back(x.far_.size());
    }
    // ffm_area: 70 groups + ffm_group + ffm_west + ffm_east
    EXPECT_TRUE(utl::any_of(
        n_far, [](auto const n) { return n == osr::kMaxSharingClasses; }));
    check_classed_offsets(r, pos, dir);
  }
}
