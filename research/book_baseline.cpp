// Offline matched-operation microbenchmark against the existing independent
// std::map/std::list reference. Allocation/setup and event generation untimed.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "event_gen.hpp"
#include "reference_book.hpp"
#include "hft/book/order_book.hpp"
#include "hft/core/memory.hpp"
using namespace hft;
struct Fast {
  core::Region region;
  core::Arena arena;
  book::OrderBook book;
  bool init(const book::BookConfig& c) {
    region=core::Region::create(64u<<20,core::RegionOptions{false,false,true,-1});
    if (!region) return false;
    arena=core::Arena(region);
    return book.init(arena,c);
  }
};
int main() {
  constexpr std::size_t warm=20000, count=200000;
  book::BookConfig c;c.base=core::Price::from_units(100);c.tick_raw=100;c.num_levels=4096;c.max_orders=1u<<14;
  std::puts("seed,method,ns_per_event,digest");
  for (unsigned rep=0;rep<5;++rep) {
    const unsigned seed=2024+rep;
    std::vector<testsupport::Event> events;events.reserve(warm+count);
    {
      Fast scratch;if (!scratch.init(c)) return 2;
      testsupport::RefBook oracle(c);
      testsupport::EventGen gen({c.base,c.tick_raw,c.num_levels,5000,false},seed);
      for (std::size_t i=0;i<warm+count;++i) {
        auto e=gen.next();auto result=testsupport::apply(scratch.book,e);
        gen.commit(e,result);events.push_back(e);
        if (result!=testsupport::apply(oracle,e)) return 3;
        if (i%1000==0 && scratch.book.digest()!=oracle.digest()) return 4;
      }
    }
    Fast fast;if (!fast.init(c)) return 2;
    testsupport::RefBook ref(c);
    for (std::size_t i=0;i<warm;++i) {testsupport::apply(fast.book,events[i]);testsupport::apply(ref,events[i]);}
    auto measure=[&](auto& b) {
      std::uint64_t errors=0;
      const auto t=std::chrono::steady_clock::now();
      for (std::size_t i=warm;i<events.size();++i) errors+=testsupport::apply(b,events[i])!=book::BookResult::Ok;
      const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-t).count()/static_cast<double>(count);
      return std::pair<double,std::uint64_t>{ns,errors};
    };
    std::pair<double,std::uint64_t> a,b;
    if (rep%2==0) {a=measure(fast.book);b=measure(ref);} else {b=measure(ref);a=measure(fast.book);}
    if (a.second || b.second || fast.book.digest()!=ref.digest()) return 5;
    std::printf("%u,flat,%.6f,%016llx\n",seed,a.first,static_cast<unsigned long long>(fast.book.digest()));
    std::printf("%u,map_list_reference,%.6f,%016llx\n",seed,b.first,static_cast<unsigned long long>(ref.digest()));
  }
}
