#pragma once

#include <optional>
#include <variant>
#include <vector>

#include "osr/location.h"

#include "nigiri/types.h"

#include "motis-api/motis-api.h"

#include "motis/fwd.h"
#include "motis/gbfs/routing_data.h"
#include "motis/place.h"

namespace nigiri {
struct timetable;
struct rt_timetable;
}  // namespace nigiri

namespace nigiri::routing {
struct query;
struct journey;
}  // namespace nigiri::routing

namespace motis::ep {
struct routing;
}

namespace motis::odm {

struct meta_router {
  meta_router(ep::routing const&,
              api::plan_params const&,
              std::vector<api::ModeEnum> const& pre_transit_modes,
              std::vector<api::ModeEnum> const& post_transit_modes,
              std::vector<api::ModeEnum> const& direct_modes,
              std::variant<osr::location, tt_location> const& from,
              std::variant<osr::location, tt_location> const& to,
              api::Place const& from_p,
              api::Place const& to_p,
              nigiri::routing::query const& start_time,
              std::vector<api::Itinerary> const& direct,
              nigiri::duration_t fastest_direct_,
              bool odm_pre_transit,
              bool odm_post_transit,
              bool odm_direct);

  api::plan_response run();

private:
  ep::routing const& r_;
  api::plan_params const& query_;
  std::vector<api::ModeEnum> const& pre_transit_modes_;
  std::vector<api::ModeEnum> const& post_transit_modes_;
  std::vector<api::ModeEnum> const& direct_modes_;
  std::variant<osr::location, tt_location> const& from_;
  std::variant<osr::location, tt_location> const& to_;
  api::Place const& from_p_;
  api::Place const& to_p_;
  nigiri::routing::query const& start_time_;
  std::vector<api::Itinerary> const& direct_;
  nigiri::duration_t fastest_direct_;
  bool odm_pre_transit_;
  bool odm_post_transit_;
  bool odm_direct_;

  nigiri::timetable const* tt_;
  std::shared_ptr<rt> const rt_;
  nigiri::rt_timetable const* rtt_;
  motis::elevators const* e_;
  gbfs::gbfs_routing_data gbfs_rd_;
  std::variant<osr::location, tt_location> const& start_;
  std::variant<osr::location, tt_location> const& dest_;
  std::vector<api::ModeEnum> const& start_modes_;
  std::vector<api::ModeEnum> const& dest_modes_;

  std::optional<std::vector<api::RentalFormFactorEnum>> const&
      start_form_factors_;
  std::optional<std::vector<api::RentalFormFactorEnum>> const&
      dest_form_factors_;
  std::optional<std::vector<api::RentalPropulsionTypeEnum>> const&
      start_propulsion_types_;
  std::optional<std::vector<api::RentalPropulsionTypeEnum>> const&
      dest_propulsion_types_;
  std::optional<std::vector<std::string>> const& start_rental_providers_;
  std::optional<std::vector<std::string>> const& dest_rental_providers_;
};

}  // namespace motis::odm