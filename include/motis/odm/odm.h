#pragma once

#include "nigiri/routing/journey.h"
#include "nigiri/types.h"

#include "motis-api/motis-api.h"

namespace motis::odm {

constexpr auto const kODMTransferBuffer = nigiri::duration_t{5};
constexpr auto const kWalk =
    static_cast<nigiri::transport_mode_id_t>(api::ModeEnum::WALK);

enum which_mile { kFirstMile, kLastMile };

bool is_odm_leg(nigiri::routing::journey::leg const&);

}  // namespace motis::odm