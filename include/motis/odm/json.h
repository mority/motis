#pragma once

#include <string>

#include "boost/json.hpp"

#include "motis/odm/prima_state.h"

namespace motis::odm {

std::string serialize(prima_state const&, n::timetable const&);

void update(prima_state&, std::string_view);

}  // namespace motis::odm