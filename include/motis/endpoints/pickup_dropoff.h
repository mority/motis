#pragma once

#include "boost/json/value.hpp"
#include "boost/url/url_view.hpp"

#include "nigiri/types.h"

#include "motis/fwd.h"
#include "motis/pickup_dropoff.h"
#include "motis/point_rtree.h"

namespace motis::ep {

struct pickup_dropoff {
  boost::json::value operator()(boost::urls::url_view const&) const;

  tag_lookup const& tags_;
  nigiri::timetable const& tt_;
  point_rtree<nigiri::location_idx_t> const& loc_rtree_;
  osr::ways const& w_;
  pickup_dropoff_points const& points_;
};

}  // namespace motis::ep
