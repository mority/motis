#include "motis/endpoints/pickup_dropoff.h"

#include "net/bad_request_exception.h"
#include "net/too_many_exception.h"

#include "osr/geojson.h"

#include "nigiri/timetable.h"

#include "motis-api/motis-api.h"
#include "motis/parse_location.h"
#include "motis/tag_lookup.h"

namespace json = boost::json;
namespace n = nigiri;

namespace motis::ep {

constexpr auto const kLimit = 8192U;

json::value pickup_dropoff::operator()(boost::urls::url_view const& url) const {
  auto const query = api::stops_params{url.params()};
  auto const min = parse_location(query.min_);
  auto const max = parse_location(query.max_);

  utl::verify<net::bad_request_exception>(
      min.has_value(), "min not a coordinate: {}", query.min_);
  utl::verify<net::bad_request_exception>(
      max.has_value(), "max not a coordinate: {}", query.max_);

  auto features = json::array{};
  loc_rtree_.find({min->pos_, max->pos_}, [&](n::location_idx_t const l) {
    auto const& pickup = points_.pickup_[l];
    auto const& drop_off = points_.drop_off_[l];
    if (pickup.empty() && drop_off.empty()) {
      return;
    }
    utl::verify<net::too_many_exception>(features.size() < kLimit,
                                         "too many items");

    auto const id = tags_.id(tt_, l);
    auto const name = tt_.translate(query.language_, tt_.locations_.names_[l]);
    auto const pos = tt_.locations_.coordinates_[l];
    features.emplace_back(
        json::value{{"type", "Feature"},
                    {"geometry", osr::to_point(osr::point::from_latlng(pos))},
                    {"properties",
                     {{"type", "location"},
                      {"id", id},
                      {"name", name},
                      {"pickups", pickup.size()},
                      {"drop_offs", drop_off.size()}}}});

    auto const add = [&](auto const& points, std::string_view const type,
                         std::string_view const link_type) {
      for (auto const& x : points) {
        auto const props =
            json::value{{"type", type},
                        {"id", id},
                        {"name", name},
                        {"walk_seconds", x.walk_.count()},
                        {"osm_node_id", to_idx(w_.node_to_osm_[x.node_])}};
        auto const node_pos = w_.get_node_pos(x.node_);
        features.emplace_back(json::value{{"type", "Feature"},
                                          {"geometry", osr::to_point(node_pos)},
                                          {"properties", props}});

        auto link_props = props.as_object();
        link_props.insert_or_assign("type", link_type);
        features.emplace_back(json::value{
            {"type", "Feature"},
            {"geometry",
             osr::to_line_string({osr::point::from_latlng(pos), node_pos})},
            {"properties", std::move(link_props)}});
      }
    };
    add(drop_off, "drop_off", "drop_off_link");
    add(pickup, "pickup", "pickup_link");
  });

  return {{"type", "FeatureCollection"}, {"features", std::move(features)}};
}

}  // namespace motis::ep
