#include "gtest/gtest.h"

#include <limits>

#include "nigiri/loader/gtfs/load_timetable.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/rt/create_rt_timetable.h"
#include "nigiri/rt/rt_timetable.h"

#include "motis/rt_random_delays.h"

namespace n = nigiri;
using namespace date;
using namespace std::chrono_literals;
using motis::generate_random_delays;
using motis::random_delay_options;

namespace {

n::loader::mem_dir test_files() {
  return n::loader::mem_dir::read(R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,Deutsche Bahn,https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_desc,stop_lat,stop_lon,stop_url,location_type,parent_station
A,A,,0.0,1.0,,
B,B,,0.02,1.03,,
C,C,,0.04,1.06,,

# calendar.txt
service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,start_date,end_date
S1,1,1,1,1,1,1,1,20190501,20190531

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_desc,route_type
R1,DB,RE 1,,,3

# trips.txt
route_id,service_id,trip_id,trip_headsign,block_id
R1,S1,T1,RE 1,
R1,S1,T2,RE 1,
R1,S1,T3,RE 1,

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence,pickup_type,drop_off_type
T1,10:00:00,10:00:00,A,1,0,0
T1,11:00:00,11:02:00,B,2,0,0
T1,12:00:00,12:00:00,C,3,0,0
T2,14:00:00,14:00:00,A,1,0,0
T2,15:00:00,15:05:00,B,2,0,0
T2,16:00:00,16:00:00,C,3,0,0
T3,23:00:00,23:00:00,A,1,0,0
T3,24:30:00,24:35:00,B,2,0,0
T3,25:30:00,25:30:00,C,3,0,0
)");
}

n::timetable load_tt() {
  auto tt = n::timetable{};
  n::loader::register_special_stations(tt);
  tt.date_range_ = {date::sys_days{2019_y / May / 1},
                    date::sys_days{2019_y / May / 10}};
  n::loader::gtfs::load_timetable({}, n::source_idx_t{0}, test_files(), tt);
  n::loader::finalize(tt);
  return tt;
}

constexpr auto const kBaseDay = date::sys_days{2019_y / May / 5};

// Everything the routing could see from the generated real-time data.
std::string dump(n::timetable const& tt, n::rt_timetable const& rtt) {
  auto ss = std::stringstream{};
  for (auto t = n::transport_idx_t{0U}; t != tt.transport_route_.size(); ++t) {
    ss << "T" << t << ": "
       << rtt.traffic_days(rtt.transport_traffic_days_[t]).to_string() << "\n";
  }
  for (auto rt_t = n::rt_transport_idx_t{0U}; rt_t != rtt.n_rt_transports();
       ++rt_t) {
    auto const t = rtt.resolve_static(rt_t);
    ss << "RT" << rt_t << " (T" << t.t_idx_ << ", day=" << t.day_ << "): ";
    for (auto const x : rtt.rt_transport_stop_times_[rt_t]) {
      ss << x << " ";
    }
    ss << "\n";
  }
  return ss.str();
}

n::rt_timetable generate(n::timetable const& tt,
                         random_delay_options const& opt) {
  auto rtt = n::rt::create_rt_timetable(tt, kBaseDay);
  generate_random_delays(tt, rtt, opt);
  return rtt;
}

}  // namespace

TEST(rt, random_delays_deterministic) {
  auto const tt = load_tt();
  auto const opt = random_delay_options{.seed_ = 42U,
                                        .delay_probability_ = 0.5,
                                        .cancel_probability_ = 0.1,
                                        .max_delay_ = 30U};

  EXPECT_EQ(dump(tt, generate(tt, opt)), dump(tt, generate(tt, opt)));

  auto other_seed = opt;
  other_seed.seed_ = 43U;
  EXPECT_NE(dump(tt, generate(tt, opt)), dump(tt, generate(tt, other_seed)));
}

TEST(rt, random_delays_probabilities) {
  auto const tt = load_tt();

  auto const no_rt = generate(tt, {.delay_probability_ = 0.0});
  EXPECT_EQ(0U, no_rt.n_rt_transports());

  auto const all_delayed =
      generate(tt, {.delay_probability_ = 1.0, .n_days_ = 1U});
  EXPECT_EQ(tt.transport_route_.size(), all_delayed.n_rt_transports());

  auto const all_cancelled =
      generate(tt, {.cancel_probability_ = 1.0, .n_days_ = 1U});
  EXPECT_EQ(0U, all_cancelled.n_rt_transports());
  for (auto t = n::transport_idx_t{0U}; t != tt.transport_route_.size(); ++t) {
    EXPECT_FALSE(all_cancelled.is_transport_active(
        t, tt.day_idx(kBaseDay - date::days{1})));
    EXPECT_TRUE(all_cancelled.is_transport_active(t, tt.day_idx(kBaseDay)));
  }
}

TEST(rt, random_delays_day_range) {
  auto const tt = load_tt();

  // Base day outside of the timetable date range: nothing to generate.
  auto far_away =
      n::rt::create_rt_timetable(tt, date::sys_days{2020_y / May / 1});
  generate_random_delays(tt, far_away, {.delay_probability_ = 1.0});
  EXPECT_EQ(0U, far_away.n_rt_transports());

  // Days that can not be represented as `delta_t` relative to the base day
  // are not generated.
  auto clamped = n::rt::create_rt_timetable(tt, kBaseDay);
  generate_random_delays(tt, clamped,
                         {.delay_probability_ = 1.0, .n_days_ = 100U});
  for (auto rt_t = n::rt_transport_idx_t{0U}; rt_t != clamped.n_rt_transports();
       ++rt_t) {
    for (auto const t : clamped.rt_transport_stop_times_[rt_t]) {
      EXPECT_GT(t, std::numeric_limits<n::delta_t>::min());
      EXPECT_LT(t, std::numeric_limits<n::delta_t>::max());
    }
  }
}

TEST(rt, random_delays_valid_times) {
  auto const tt = load_tt();
  auto const opt = random_delay_options{
      .seed_ = 7U, .delay_probability_ = 1.0, .max_delay_ = 30U};
  auto const rtt = generate(tt, opt);

  EXPECT_NE(0U, rtt.n_rt_transports());
  for (auto rt_t = n::rt_transport_idx_t{0U}; rt_t != rtt.n_rt_transports();
       ++rt_t) {
    auto const t = rtt.resolve_static(rt_t);
    auto const n_stops =
        static_cast<n::stop_idx_t>(rtt.rt_transport_location_seq_[rt_t].size());

    // The static transport is not active on this day anymore.
    EXPECT_FALSE(rtt.is_transport_active(t.t_idx_, t.day_));

    auto pred = n::unixtime_t::min();
    for (auto stop_idx = n::stop_idx_t{0U}; stop_idx != n_stops; ++stop_idx) {
      for (auto const ev_type : {n::event_type::kArr, n::event_type::kDep}) {
        if ((ev_type == n::event_type::kArr && stop_idx == 0U) ||
            (ev_type == n::event_type::kDep && stop_idx == n_stops - 1U)) {
          continue;
        }
        auto const scheduled = tt.event_time(t, stop_idx, ev_type);
        auto const rt = rtt.unix_event_time(rt_t, stop_idx, ev_type);
        EXPECT_LE(pred, rt);  // monotonically increasing
        EXPECT_LE(scheduled, rt);  // never earlier than scheduled
        EXPECT_LE(rt - scheduled, n::duration_t{opt.max_delay_});
        pred = rt;
      }
    }
  }
}
