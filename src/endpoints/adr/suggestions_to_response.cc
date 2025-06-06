#include "motis/endpoints/adr/suggestions_to_response.h"

#include "utl/for_each_bit_set.h"
#include "utl/helpers/algorithm.h"
#include "utl/overloaded.h"
#include "utl/to_vec.h"

#include "nigiri/timetable.h"

#include "adr/typeahead.h"

#include "motis/journey_to_response.h"
#include "motis/tag_lookup.h"

namespace a = adr;
namespace n = nigiri;

namespace motis {

long get_area_lang_idx(a::typeahead const& t,
                       a::language_list_t const& languages,
                       a::area_idx_t const a) {
  for (auto i = 0U; i != languages.size(); ++i) {
    auto const j = languages.size() - i - 1U;
    auto const lang_idx = a::find_lang(t.area_name_lang_[a], languages[j]);
    if (lang_idx != -1) {
      return lang_idx;
    }
  }
  return -1;
}

api::geocode_response suggestions_to_response(
    adr::typeahead const& t,
    n::timetable const* tt,
    tag_lookup const* tags,
    osr::ways const* w,
    osr::platforms const* pl,
    platform_matches_t const* matches,
    basic_string<a::language_idx_t> const& lang_indices,
    std::vector<adr::token> const& token_pos,
    std::vector<adr::suggestion> const& suggestions) {
  return utl::to_vec(suggestions, [&](a::suggestion const& s) {
    auto const areas = t.area_sets_[s.area_set_];
    auto type = api::LocationTypeEnum{};
    auto street = std::optional<std::string>{};
    auto house_number = std::optional<std::string>{};
    auto id = std::string{};
    auto level = std::optional<double>{};
    auto name = std::visit(
        utl::overloaded{
            [&](a::place_idx_t const p) {
              type = t.place_type_[p] == a::place_type::kExtra
                         ? api::LocationTypeEnum::STOP
                         : api::LocationTypeEnum::PLACE;
              if (type == api::LocationTypeEnum::STOP) {
                if (tt != nullptr && tags != nullptr) {
                  auto const l = n::location_idx_t{t.place_osm_ids_[p]};
                  level = get_level(w, pl, matches, l);
                  id = tags->id(*tt, n::location_idx_t{t.place_osm_ids_[p]});
                } else {
                  id = fmt::format("stop/{}", p);
                }
              } else {
                id = fmt::format("{}/{}",
                                 t.place_is_way_[to_idx(p)] ? "way" : "node",
                                 t.place_osm_ids_[p]);
              }
              return std::string{t.strings_[s.str_].view()};
            },
            [&](a::address const addr) {
              type = api::LocationTypeEnum::ADDRESS;
              if (addr.house_number_ != a::address::kNoHouseNumber) {
                street = t.strings_[s.str_].view();
                house_number = t.strings_[t.house_numbers_[addr.street_]
                                                          [addr.house_number_]]
                                   .view();
                return fmt::format("{} {}", *street, *house_number);
              } else {
                return std::string{t.strings_[s.str_].view()};
              }
            }},
        s.location_);

    auto tokens = std::vector<std::vector<double>>{};
    utl::for_each_set_bit(s.matched_tokens_, [&](auto const i) {
      assert(i < token_pos.size());
      tokens.emplace_back(
          std::vector<double>{static_cast<double>(token_pos[i].start_idx_),
                              static_cast<double>(token_pos[i].size_)});
    });

    auto const is_matched = [&](std::size_t const i) {
      return (((1U << i) & s.matched_areas_) != 0U);
    };

    auto api_areas = std::vector<api::Area>{};
    for (auto const [i, a] : utl::enumerate(areas)) {
      auto const admin_lvl = t.area_admin_level_[a];
      if (admin_lvl == a::kPostalCodeAdminLevel) {
        continue;
      }

      auto const language = is_matched(i)
                                ? s.matched_area_lang_[i]
                                : get_area_lang_idx(t, lang_indices, a);
      auto const area_name =
          t.strings_[t.area_names_[a][language == -1
                                          ? a::kDefaultLangIdx
                                          : static_cast<unsigned>(language)]]
              .view();
      api_areas.emplace_back(api::Area{
          .name_ = std::string{area_name},
          .adminLevel_ = static_cast<double>(to_idx(admin_lvl)),
          .matched_ = is_matched(i),
          .unique_ = s.unique_area_idx_.has_value() && *s.unique_area_idx_ == i,
          .default_ = s.city_area_idx_.has_value() && *s.city_area_idx_ == i});
    }

    return api::Match{
        .type_ = type,
        .tokens_ = std::move(tokens),
        .name_ = std::move(name),
        .id_ = std::move(id),
        .lat_ = s.coordinates_.as_latlng().lat_,
        .lon_ = s.coordinates_.as_latlng().lng_,
        .level_ = level,
        .street_ = std::move(street),
        .houseNumber_ = std::move(house_number),
        .zip_ = s.zip_area_idx_.and_then([&](unsigned const zip_area_idx) {
          return std::optional<std::string>{
              t.strings_[t.area_names_[areas[zip_area_idx]][a::kDefaultLangIdx]]
                  .view()};
        }),
        .areas_ = std::move(api_areas),
        .score_ = s.score_};
  });
}

}  // namespace motis