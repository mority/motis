#include "conf/configuration.h"

#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <ranges>

#include "boost/json/parse.hpp"
#include "boost/json/serialize.hpp"
#include "boost/json/value_from.hpp"
#include "boost/json/value_to.hpp"
#include "boost/url/url.hpp"

#include "fmt/std.h"

#include "utl/enumerate.h"
#include "utl/get_or_create.h"
#include "utl/helpers/algorithm.h"
#include "utl/init_from.h"
#include "utl/parallel_for.h"
#include "utl/parser/cstr.h"
#include "utl/sorted_diff.h"
#include "utl/timing.h"

#include "nigiri/routing/query.h"
#include "nigiri/timetable.h"

#include "motis-api/motis-api.h"
#include "motis/config.h"
#include "motis/data.h"
#include "motis/endpoints/routing.h"
#include "motis/place.h"
#include "motis/point_rtree.h"
#include "motis/tag_lookup.h"

#include "./flags.h"

namespace n = nigiri;
namespace fs = std::filesystem;
namespace po = boost::program_options;
namespace json = boost::json;

namespace motis {

static std::atomic_uint32_t seed{0U};
static constexpr auto const kIntermodalMaxDist = 600U;  // m

std::uint32_t rand_in(std::uint32_t const from, std::uint32_t const to) {
  auto a = ++seed;
  a = (a ^ 61U) ^ (a >> 16U);
  a = a + (a << 3U);
  a = a ^ (a >> 4U);
  a = a * 0x27d4eb2d;
  a = a ^ (a >> 15U);
  return from + (a % (to - from));
}

template <typename It>
It rand_in(It const begin, It const end) {
  return std::next(
      begin,
      rand_in(0U, static_cast<std::uint32_t>(std::distance(begin, end))));
}

template <typename Collection>
Collection::value_type rand_in(Collection const& c) {
  using std::begin;
  using std::end;
  utl::verify(!c.empty(), "empty collection");
  return *rand_in(begin(c), end(c));
}

n::location_idx_t random_stop(n::timetable const& tt,
                              std::vector<n::location_idx_t> const& stops) {
  auto s = n::location_idx_t::invalid();
  do {
    s = rand_in(stops);
  } while (tt.location_routes_[s].empty());
  return s;
}

int generate(int ac, char** av) {
  auto data_path = fs::path{"data"};
  auto n = 100U;
  auto intermodal = false;
  auto bike = false;
  auto car = false;

  auto desc = po::options_description{"Options"};
  desc.add_options()  //
      ("help", "Prints this help message")  //
      ("n,n", po::value(&n)->default_value(n), "number of queries")  //
      ("intermodal,i", po::bool_switch(&intermodal),
       "generate intermodal queries")  //
      ("bike", po::bool_switch(&bike), "adds BIKE to intermodal queries")  //
      ("car", po::bool_switch(&car), "adds CAR to intermodal queries");
  add_data_path_opt(desc, data_path);
  auto vm = parse_opt(ac, av, desc);

  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  auto const c = config::read(data_path / "config.yml");
  utl::verify(c.timetable_.has_value(), "timetable required");

  auto d = data{data_path, c};
  utl::verify(d.tt_, "timetable required");

  auto stops = std::vector<n::location_idx_t>{};
  stops.resize(d.tt_->n_locations());
  for (auto i = 0U; i != stops.size(); ++i) {
    stops[i] = n::location_idx_t{i};
  }

  static auto r = std::random_device{};
  static auto e = std::default_random_engine{r()};

  {
    auto out = std::ofstream{"queries.txt"};
    for (auto i = 0U; i != n; ++i) {
      auto p = api::plan_params{};

      auto const random_time = [&]() {
        static auto time_distribution =
            std::uniform_int_distribution<n::unixtime_t::rep>{
                d.tt_->external_interval().from_.time_since_epoch().count(),
                d.tt_->external_interval().to_.time_since_epoch().count() - 1};
        return n::unixtime_t{n::unixtime_t::duration{time_distribution(e)}};
      };
      p.time_ = random_time();

      if (intermodal) {
        auto const random_coords = [&]() {
          static auto distance_distribution =
              std::uniform_int_distribution<unsigned>{0, kIntermodalMaxDist};
          static auto bearing_distribution =
              std::uniform_int_distribution<unsigned>{0, 359};
          auto const coords = destination_point(
              d.tt_->locations_.coordinates_[random_stop(*d.tt_, stops)],
              distance_distribution(e), bearing_distribution(e));

          return fmt::format("{},{},0", coords.lat_, coords.lng_);
        };

        p.fromPlace_ = random_coords();
        p.toPlace_ = random_coords();

        if (bike) {
          p.directModes_.emplace_back(api::ModeEnum::BIKE);
          p.preTransitModes_.emplace_back(api::ModeEnum::BIKE);
          p.postTransitModes_.emplace_back(api::ModeEnum::BIKE);
        }
        if (car) {
          p.directModes_.emplace_back(api::ModeEnum::CAR);
          p.preTransitModes_.emplace_back(api::ModeEnum::CAR);
          p.postTransitModes_.emplace_back(api::ModeEnum::CAR);
        }
      } else {
        p.fromPlace_ = d.tags_->id(*d.tt_, random_stop(*d.tt_, stops));
        p.toPlace_ = d.tags_->id(*d.tt_, random_stop(*d.tt_, stops));
      }

      out << p.to_url("/api/v1/plan") << "\n";
    }
  }

  return 0;
}

int batch(int ac, char** av) {
  auto data_path = fs::path{"data"};
  auto queries_path = fs::path{"queries.txt"};
  auto responses_path = fs::path{"responses.txt"};

  auto desc = po::options_description{"Options"};
  desc.add_options()  //
      ("help", "Prints this help message")  //
      ("queries,q", po::value(&queries_path)->default_value(queries_path),
       "queries file")  //
      ("responses,r", po::value(&responses_path)->default_value(responses_path),
       "response file");
  add_data_path_opt(desc, data_path);

  auto vm = parse_opt(ac, av, desc);
  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  auto queries = std::vector<api::plan_params>{};
  {
    auto f = cista::mmap{queries_path.generic_string().c_str(),
                         cista::mmap::protection::READ};
    utl::for_each_token(utl::cstr{f.view()}, '\n', [&](utl::cstr s) {
      queries.push_back(api::plan_params{boost::urls::url{s.view()}.params()});
    });
  }

  auto const c = config::read(data_path / "config.yml");
  utl::verify(c.timetable_.has_value(), "timetable required");

  auto d = data{data_path, c};
  utl::verify(d.tt_, "timetable required");

  auto mtx = std::mutex{};
  auto out = std::ofstream{responses_path};
  auto total = std::atomic_uint64_t{};
  auto const routing = utl::init_from<ep::routing>(d).value();
  utl::parallel_for_run(queries.size(), [&](std::size_t const id) {
    UTL_START_TIMING(total);
    auto response = routing(queries.at(id).to_url("/api/v1/plan"));
    UTL_STOP_TIMING(total);

    auto const timing = static_cast<std::uint64_t>(UTL_TIMING_MS(total));
    response.debugOutput_.emplace("id", id);
    response.debugOutput_.emplace("timing", timing);
    {
      auto const lock = std::scoped_lock{mtx};
      out << json::serialize(json::value_from(response)) << "\n";
    }
    total += timing;
  });

  std::cout << "AVG: "
            << (static_cast<double>(total) /
                static_cast<double>(queries.size()))
            << "\n";

  return 0U;
}

int compare(int ac, char** av) {
  auto queries_path = fs::path{"queries.txt"};
  auto responses_paths = std::vector<std::string>{};
  auto desc = po::options_description{"Options"};
  desc.add_options()  //
      ("help", "Prints this help message")  //
      ("queries,q", po::value(&queries_path)->default_value(queries_path),
       "queries file")  //
      ("responses,r",
       po::value(&responses_paths)
           ->multitoken()
           ->default_value(responses_paths),
       "response files");

  auto vm = parse_opt(ac, av, desc);
  if (vm.count("help")) {
    std::cout << desc << "\n";
    return 0;
  }

  auto const open_file = [](fs::path const& p) {
    auto f = std::ifstream{};
    f.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    try {
      f.open(p);
    } catch (std::exception const& e) {
      throw utl::fail("could not open file \"{}\": {}", p, e.what());
    }
    return f;
  };

  auto const read_line = [](std::ifstream& f) -> std::optional<std::string> {
    if (f.peek() == EOF || f.eof()) {
      return std::nullopt;
    }
    std::string line;
    std::getline(f, line);
    return line;
  };

  struct info {
    unsigned id_;
    std::optional<api::plan_params> params_{};
    std::vector<std::optional<api::plan_response>> responses_{};
  };
  auto response_buf = hash_map<unsigned, info>{};
  auto const get = [&](unsigned const id) -> info& {
    return utl::get_or_create(response_buf, id, [&]() {
      auto x = info{.id_ = id};
      x.responses_.resize(responses_paths.size());
      return x;
    });
  };
  auto const is_finished = [](info const& x) {
    return x.params_.has_value() &&
           utl::all_of(x.responses_, [](auto&& r) { return r.has_value(); });
  };
  auto const params = [](api::Itinerary const& x) {
    return std::tie(x.startTime_, x.endTime_, x.transfers_);
  };
  auto const print_params = [](api::Itinerary const& x) {
    auto const pre_transit = [&]() {
      auto ss = std::stringstream{};
      if (x.legs_.front().mode_ != api::ModeEnum::TRANSIT) {
        ss << x.legs_.front().mode_ << " "
           << std::chrono::duration_cast<std::chrono::minutes>(
                  std::chrono::seconds{x.legs_.front().duration_})
                  .count();
      }
      return ss.str();
    };
    auto const post_transit = [&]() {
      auto ss = std::stringstream{};
      if (x.legs_.back().mode_ != api::ModeEnum::TRANSIT) {
        ss << x.legs_.back().mode_ << " "
           << std::chrono::duration_cast<std::chrono::minutes>(
                  std::chrono::seconds{x.legs_.back().duration_})
                  .count();
      }
      return ss.str();
    };
    auto const time = [](auto const& t) {
      return std::format("{0:%F}T{0:%R}", t);
    };
    std::cout << time(x.startTime_.time_) << ", " << time(x.endTime_.time_)
              << ", " << x.transfers_ << ", " << pre_transit() << ", "
              << post_transit();
  };
  auto const print_none = []() { std::cout << "\t\t\t\t\t\t"; };
  auto n_equal = 0U;
  auto const print_differences = [&](info const& x) {
    auto const& ref = x.responses_[0].value().itineraries_;
    for (auto i = 1U; i < x.responses_.size(); ++i) {
      auto const uut = x.responses_[i].value().itineraries_;
      if (std::ranges::equal(ref | std::views::transform(params),
                             uut | std::views::transform(params))) {
        ++n_equal;
        continue;
      }

      std::cout << "QUERY=" << x.id_ << "\n";
      utl::sorted_diff(
          ref, uut,
          [&](api::Itinerary const& a, api::Itinerary const& b) {
            return params(a) < params(b);
          },
          [&](api::Itinerary const&, api::Itinerary const&) {
            return false;  // always call for equal
          },
          utl::overloaded{
              [&](utl::op op, api::Itinerary const& j) {
                if (op == utl::op::kAdd) {
                  print_none();
                  std::cout << "\t\t\t";
                  print_params(j);
                  std::cout << "\n";
                } else {
                  print_params(j);
                  std::cout << "\t\t\t";
                  print_none();
                  std::cout << "\n";
                }
              },
              [&](api::Itinerary const& a, api::Itinerary const& b) {
                print_params(a);
                std::cout << "\t\t\t";
                print_params(b);
                std::cout << "\n";
              }});
      std::cout << "\n\n";
    }
  };
  auto n_consumed = 0U;
  auto const consume_if_finished = [&](info const& x) {
    if (!is_finished(x)) {
      return;
    }
    print_differences(x);
    response_buf.erase(x.id_);
    ++n_consumed;
  };

  auto query_file = open_file(queries_path);
  auto responses_files =
      utl::to_vec(responses_paths, [&](auto&& p) { return open_file(p); });

  auto query_id = 0U;
  auto done = false;
  while (!done) {
    done = true;

    if (auto const q = read_line(query_file); q.has_value()) {
      auto& info = get(query_id++);
      info.params_ = api::plan_params{boost::urls::url{*q}.params()};
      consume_if_finished(info);
      done = false;
    }

    for (auto const [i, res_file] : utl::enumerate(responses_files)) {
      if (auto const r = read_line(res_file); r.has_value()) {
        auto res =
            boost::json::value_to<api::plan_response>(boost::json::parse(*r));
        utl::sort(res.itineraries_,
                  [&](auto&& a, auto&& b) { return params(a) < params(b); });
        auto const id = res.debugOutput_.at("id");
        auto& info = get(static_cast<unsigned>(id));
        info.responses_[i] = std::move(res);
        consume_if_finished(info);
        done = false;
      }
    }
  }

  std::cout << "consumed: " << n_consumed << "\n";
  std::cout << "buffered: " << response_buf.size() << "\n";
  std::cout << "   equal: " << n_equal << "\n";

  return 0;
}

}  // namespace motis