#include "motis/odm/odm.h"

#include "nigiri/routing/journey.h"

#include "motis/transport_mode_ids.h"

namespace motis::odm {

namespace n = nigiri;
namespace nr = nigiri::routing;

bool is_odm_leg(nr::journey::leg const& l) {
  return std::holds_alternative<nr::offset>(l.uses_) &&
         std::get<nr::offset>(l.uses_).transport_mode_id_ ==
             kOdmTransportModeId;
}

n::duration_t odm_time(nr::journey const& j) {
  return j.legs_.empty() ? n::duration_t{0}
         : is_odm_leg(j.legs_.front())
             ? std::get<nr::offset>(j.legs_.front().uses_).duration()
             : n::duration_t{0} +
                   ((j.legs_.size() > 1 && is_odm_leg(j.legs_.back()))
                        ? std::get<nr::offset>(j.legs_.back().uses_).duration()
                        : n::duration_t{0});
}

bool is_direct_odm(nr::journey const& j) {
  return j.legs_.size() == 1U &&
         std::holds_alternative<nr::offset>(j.legs_.front().uses_) &&
         std::get<nr::offset>(j.legs_.front().uses_).transport_mode_id_ ==
             kOdmTransportModeId;
}

}  // namespace motis::odm