#include "motis/td_trace.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <type_traits>

#include "fmt/format.h"

#include "cista/hash.h"

#include "utl/verify.h"

#include "nigiri/td_footpath.h"

namespace fs = std::filesystem;
namespace n = nigiri;

namespace motis {

namespace {

constexpr auto const kMagic = std::uint64_t{0x3130454341525444};  // "DTRACE01"

template <typename T>
void put(std::ostream& out, T const& x) {
  static_assert(std::is_trivially_copyable_v<T>);
  out.write(reinterpret_cast<char const*>(&x), sizeof(T));
}

template <typename T>
void put(std::ostream& out, std::vector<T> const& v) {
  static_assert(std::is_trivially_copyable_v<T>);
  put(out, static_cast<std::uint64_t>(v.size()));
  out.write(reinterpret_cast<char const*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(T)));
}

template <typename T>
void get(std::istream& in, T& x) {
  static_assert(std::is_trivially_copyable_v<T>);
  in.read(reinterpret_cast<char*>(&x), sizeof(T));
}

template <typename T>
void get(std::istream& in, std::vector<T>& v) {
  auto size = std::uint64_t{};
  get(in, size);
  v.resize(size);
  in.read(reinterpret_cast<char*>(v.data()),
          static_cast<std::streamsize>(size * sizeof(T)));
}

n::duration_t min_duration(std::vector<n::routing::offset> const& offsets) {
  auto m = n::duration_t{std::numeric_limits<n::duration_t::rep>::max()};
  for (auto const& o : offsets) {
    m = std::min(m, o.duration());
  }
  return m;
}

#ifdef NIGIRI_TD_TRACE
void on_lookup(void* const ctx,
               void const* const first,
               n::direction const dir,
               n::unixtime_t const t) {
  auto& trace = *static_cast<td_trace*>(ctx);
  auto const it = trace.by_address_.find(first);
  if (it == end(trace.by_address_)) {
    ++trace.n_unmatched_;
    return;
  }
  trace.lookups_.push_back({.seq_ = it->second,
                            .dir_ = static_cast<std::uint8_t>(dir),
                            .t_ = t});
}
#endif

}  // namespace

std::optional<fs::path> const& td_trace::dir() {
  static auto const d = []() -> std::optional<fs::path> {
    auto const* const e = std::getenv("MOTIS_TD_TRACE");
    if (e == nullptr || *e == '\0') {
      return std::nullopt;
    }
#ifndef NIGIRI_TD_TRACE
    std::cerr << "MOTIS_TD_TRACE is set, but this binary was built without "
                 "-DNIGIRI_TD_TRACE and would record no lookups\n";
    std::exit(1);
#endif
    fs::create_directories(e);
    return fs::path{e};
  }();
  return d;
}

void td_trace::capture_before_pruning(n::routing::query const& q) {
  min_start_plain_ = min_duration(q.start_);
  min_dest_plain_ = min_duration(q.destination_);
}

void td_trace::capture_query(n::routing::query const& q) {
  fastest_direct_ = q.fastest_direct_;
  for (auto const& [side, offsets] :
       {std::pair{std::uint8_t{0U}, &q.td_start_},
        std::pair{std::uint8_t{1U}, &q.td_dest_}}) {
    for (auto const& [l, v] : *offsets) {
      if (!v.empty()) {
        by_address_.emplace(v.data(), static_cast<std::uint32_t>(seqs_.size()));
      }
      seqs_.push_back({.side_ = side, .location_ = l, .offsets_ = v});
    }
  }
}

void td_trace::write(fs::path const& dir) const {
  auto const path = dir / fmt::format("{:016x}.bin", cista::hash(url_));
  auto out = std::ofstream{path, std::ios::binary};
  utl::verify(out.good(), "td_trace: cannot write {}", path.string());

  put(out, kMagic);
  put(out, std::vector<char>{begin(url_), end(url_)});
  put(out, arrive_by_);
  put(out, fastest_direct_.has_value());
  put(out, fastest_direct_.value_or(n::duration_t{0}));
  put(out, min_start_plain_);
  put(out, min_dest_plain_);
  put(out, windows_);
  put(out, static_cast<std::uint64_t>(seqs_.size()));
  for (auto const& s : seqs_) {
    put(out, s.side_);
    put(out, s.location_);
    put(out, s.offsets_);
  }
  put(out, lookups_);
  put(out, n_unmatched_);
}

td_trace td_trace::read(fs::path const& path) {
  auto in = std::ifstream{path, std::ios::binary};
  utl::verify(in.good(), "td_trace: cannot read {}", path.string());

  auto t = td_trace{};
  auto magic = std::uint64_t{};
  get(in, magic);
  utl::verify(magic == kMagic, "td_trace: {} is not a trace", path.string());

  auto url = std::vector<char>{};
  get(in, url);
  t.url_ = std::string{begin(url), end(url)};
  get(in, t.arrive_by_);
  auto has_fastest_direct = false;
  auto fastest_direct = n::duration_t{};
  get(in, has_fastest_direct);
  get(in, fastest_direct);
  if (has_fastest_direct) {
    t.fastest_direct_ = fastest_direct;
  }
  get(in, t.min_start_plain_);
  get(in, t.min_dest_plain_);
  get(in, t.windows_);
  auto n_seqs = std::uint64_t{};
  get(in, n_seqs);
  t.seqs_.resize(n_seqs);
  for (auto& s : t.seqs_) {
    get(in, s.side_);
    get(in, s.location_);
    get(in, s.offsets_);
  }
  get(in, t.lookups_);
  get(in, t.n_unmatched_);
  utl::verify(in.good(), "td_trace: {} is truncated", path.string());
  return t;
}

#ifdef NIGIRI_TD_TRACE
td_trace_scope::td_trace_scope(td_trace& trace) {
  n::td_trace_ctx = &trace;
  n::td_trace_hook = &on_lookup;
}

td_trace_scope::~td_trace_scope() {
  n::td_trace_hook = nullptr;
  n::td_trace_ctx = nullptr;
}
#endif

}  // namespace motis
