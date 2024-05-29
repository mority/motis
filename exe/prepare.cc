#include <filesystem>
#include <iostream>

#include "boost/program_options.hpp"

#include "nigiri/timetable.h"

#include "osr/lookup.h"
#include "osr/platforms.h"
#include "osr/routing/route.h"
#include "osr/ways.h"

#include "icc/match.h"

namespace fs = std::filesystem;
namespace bpo = boost::program_options;
namespace n = nigiri;

int main(int ac, char** av) {
  auto tt_path = fs::path{"tt.bin"};
  auto osr_path = fs::path{"osr"};

  auto desc = bpo::options_description{"Options"};
  desc.add_options()  //
      ("help,h", "produce this help message")  //
      ("tt", bpo::value(&tt_path)->default_value(tt_path), "timetable path")  //
      ("osr", bpo::value(&osr_path)->default_value(osr_path), "osr data");
  auto const pos =
      bpo::positional_options_description{}.add("tt", -1).add("osr", 1);

  auto vm = bpo::variables_map{};
  bpo::store(
      bpo::command_line_parser(ac, av).options(desc).positional(pos).run(), vm);
  bpo::notify(vm);

  if (vm.count("help") != 0U) {
    std::cout << desc << "\n";
    return 0;
  }

  auto tt = n::timetable::read(cista::memory_holder{
      cista::file{tt_path.generic_string().c_str(), "r"}.content()});
  auto const w = osr::ways{osr_path, cista::mmap::protection::READ};
  auto pl = osr::platforms{osr_path, cista::mmap::protection::READ};
  pl.build_rtree(w);

  auto const m = icc::match(*tt, pl, w);
}