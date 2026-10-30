#include "gtest/gtest.h"

#include <filesystem>
#include <string>

#include "utl/verify.h"

#include "nigiri/timetable.h"

#include "osr/ways.h"

#include "motis/config.h"
#include "motis/data.h"
#include "motis/import.h"
#include "motis/pickup_dropoff.h"

namespace n = nigiri;
using namespace motis;

namespace {

// OSM: Darmstadt Hbf and surroundings.
constexpr auto const kGtfs = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_lat,stop_lon,location_type,parent_station,platform_code
DA,DA Hbf,49.87260,8.63085,1,,
DA_10,DA Hbf,49.87336,8.62926,0,DA,10
DA_N,DA Hbf Nord,49.87420,8.62940,0,,
DA_BUS,DA Hbf Bus,49.87230,8.63160,1,,
DA_BUS_1,DA Hbf Bus,49.87220,8.63180,0,DA_BUS,1

# calendar.txt
service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,start_date,end_date
S,1,1,1,1,1,1,1,20190501,20190503

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
BUS,DB,Bus,,,3
FLEX,DB,Flex,,,715

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id
BUS,S,BUS,,
BUS,S,BUS2,,
FLEX,S,FLEX,,
)";

constexpr auto const kStopTimes = R"(
# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,location_group_id,location_id,stop_sequence,start_pickup_drop_off_window,end_pickup_drop_off_window,pickup_type,drop_off_type
BUS,10:00:00,10:00:00,DA_10,,,0,,,0,0
BUS,10:05:00,10:05:00,DA_N,,,1,,,0,0
BUS2,11:00:00,11:00:00,DA_BUS,,,0,,,0,0
BUS2,11:05:00,11:05:00,DA_N,,,1,,,0,0
)";

constexpr auto const kFlexArea = R"(
FLEX,,,,,da_flex,0,06:00:00,22:00:00,2,2
FLEX,,,,,da_flex,1,06:00:00,22:00:00,2,2

# locations.geojson
{"type":"FeatureCollection","features":[{"id":"da_flex","type":"Feature","geometry":{"type":"Polygon","coordinates":[[[8.620,49.865],[8.640,49.865],[8.640,49.880],[8.620,49.880],[8.620,49.865]]]},"properties":{"stop_name":"DA Flex Area"}}]}
)";

constexpr auto const kFlexGroup = R"(
FLEX,,,,da_group,,0,06:00:00,22:00:00,2,2
FLEX,,,,da_group,,1,06:00:00,22:00:00,2,2

# location_groups.txt
location_group_id,location_group_name
da_group,DA Flex Group

# location_group_stops.txt
location_group_id,stop_id
da_group,DA_N
)";

enum class flex_data { kNone, kArea, kGroup };

data import_with(std::string_view const dir,
                 config::timetable::pickup_dropoff const mode,
                 flex_data const f) {
  auto ec = std::error_code{};
  std::filesystem::remove_all(dir, ec);
  auto const gtfs = std::string{kGtfs} + kStopTimes +
                    (f == flex_data::kArea    ? kFlexArea
                     : f == flex_data::kGroup ? kFlexGroup
                                              : "");
  auto const c = config{
      .osm_ = {"test/resources/darmstadt_hbf.osm.pbf"},
      .timetable_ = config::timetable{.first_day_ = "2019-05-01",
                                      .num_days_ = 2,
                                      .pickup_dropoff_points_ = mode,
                                      .datasets_ = {{"test", {.path_ = gtfs}}}},
      .street_routing_ = true};
  import(c, dir);
  return data{dir, c};
}

n::location_idx_t get_location(data const& d, std::string_view const id) {
  auto const& ids = d.tt_->locations_.location_id_to_idx_;
  auto const it = ids.find(n::location_id{id, n::source_idx_t{0U}});
  utl::verify(it != end(ids), "location {} not found", id);
  return it->second;
}

}  // namespace

TEST(motis, pickup_dropoff_points_flex) {
  auto const d =
      import_with("test/data_pickup_dropoff_flex",
                  config::timetable::pickup_dropoff::flex, flex_data::kArea);
  auto const& p = *d.pickup_dropoff_;
  auto const& w = *d.w_;

  auto const da = get_location(d, "DA_10");
  ASSERT_FALSE(p.drop_off_[da].empty());
  ASSERT_FALSE(p.pickup_[da].empty());

  // DA: parent without own events. DA_BUS: parent served directly (against
  // the GTFS spec, but nigiri loads it).
  EXPECT_TRUE(p.drop_off_[get_location(d, "DA")].empty());
  EXPECT_FALSE(p.drop_off_[get_location(d, "DA_BUS")].empty());

  // Not on the service road in the tunnel along the tracks (level=-1).
  auto const tunnel = osr::osm_node_idx_t{5256701321U};
  for (auto const& points : {p.drop_off_[da], p.pickup_[da]}) {
    for (auto const& x : points) {
      EXPECT_TRUE(w.r_->node_properties_[x.node_].is_car_accessible());
      EXPECT_LE(x.walk_, std::chrono::minutes{5});
      EXPECT_NE(tunnel, w.node_to_osm_[x.node_]);
    }
  }
}

TEST(motis, pickup_dropoff_points_flex_without_areas) {
  auto const d =
      import_with("test/data_pickup_dropoff_no_area",
                  config::timetable::pickup_dropoff::flex, flex_data::kNone);
  auto const da = get_location(d, "DA_10");
  EXPECT_TRUE(d.pickup_dropoff_->drop_off_[da].empty());
  EXPECT_TRUE(d.pickup_dropoff_->pickup_[da].empty());
}

TEST(motis, pickup_dropoff_points_flex_location_group) {
  auto const d =
      import_with("test/data_pickup_dropoff_group",
                  config::timetable::pickup_dropoff::flex, flex_data::kGroup);
  auto const& p = *d.pickup_dropoff_;
  EXPECT_FALSE(p.drop_off_[get_location(d, "DA_N")].empty());
  EXPECT_TRUE(p.drop_off_[get_location(d, "DA_10")].empty());
}

TEST(motis, pickup_dropoff_points_all) {
  auto const d =
      import_with("test/data_pickup_dropoff_all",
                  config::timetable::pickup_dropoff::all, flex_data::kNone);
  auto const da = get_location(d, "DA_10");
  EXPECT_FALSE(d.pickup_dropoff_->drop_off_[da].empty());
  EXPECT_FALSE(d.pickup_dropoff_->pickup_[da].empty());
}
