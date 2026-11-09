#include <benchmark/benchmark.h>

#include <memory>
#include <vector>

#include "heap_order_book.h"
#include "map_order_book.h"
#include "tick_order_book.h"
#include "workloads.h"

using namespace std;

namespace {

template <typename Book> unique_ptr<Book> build(const Workload &w);

template <> unique_ptr<TickOrderBook> build(const Workload &w) {
    return make_unique<TickOrderBook>(kTicker, w.min_price, w.max_price, 1, w.max_orders);
}
template <> unique_ptr<HeapOrderBook> build(const Workload &w) {
    return make_unique<HeapOrderBook>(kTicker, w.max_orders, w.max_levels);
}
template <> unique_ptr<MapOrderBook> build(const Workload &w) {
    return make_unique<MapOrderBook>(kTicker, w.max_orders);
}

template <typename Book>
void runWorkload(benchmark::State &state, const Workload *w) {
    size_t resting = 0;
    for (auto _ : state) {
        state.PauseTiming();
        unique_ptr<Book> book = build<Book>(*w);
        for (const Op &op : w->prefill) apply(*book, op);
        state.ResumeTiming();

        for (const Op &op : w->ops) apply(*book, op);
        benchmark::DoNotOptimize(book->orderCount());

        state.PauseTiming();
        resting = book->orderCount();
        book.reset();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * w->ops.size()));
    state.counters["resting_at_end"] = static_cast<double>(resting);
    state.counters["per_op"] = benchmark::Counter(static_cast<double>(w->ops.size()),
                                                  benchmark::Counter::kIsIterationInvariantRate |
                                                      benchmark::Counter::kInvert);
}

template <typename Book>
void topOfBookDepth(benchmark::State &state, const Workload *w) {
    unique_ptr<Book> book = build<Book>(*w);
    for (const Op &op : w->prefill) apply(*book, op);
    for (const Op &op : w->ops) apply(*book, op);
    LevelInfo bids[10], asks[10];
    for (auto _ : state) {
        size_t n = book->depth(Side::Buy, 10, bids);
        n += book->depth(Side::Sell, 10, asks);
        benchmark::DoNotOptimize(n);
        benchmark::DoNotOptimize(bids);
        benchmark::DoNotOptimize(asks);
    }
    state.counters["levels"] = static_cast<double>(book->levelCount(Side::Buy) + book->levelCount(Side::Sell));
}

}

int main(int argc, char **argv) {
    constexpr size_t kOps = 200'000;
    static vector<Workload> workloads;
    for (WorkloadKind kind : {WorkloadKind::AddOnly, WorkloadKind::CancelHeavy, WorkloadKind::Aggressive,
                              WorkloadKind::Mixed, WorkloadKind::WideSparse}) {
        WorkloadBuilder builder(42);
        workloads.push_back(builder.build(kind, kOps));
    }

    for (const Workload &w : workloads) {
        benchmark::RegisterBenchmark(("workload/" + w.name + "/tick_array").c_str(), runWorkload<TickOrderBook>, &w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("workload/" + w.name + "/sorted_map").c_str(), runWorkload<MapOrderBook>, &w)
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(("workload/" + w.name + "/priority_queue").c_str(), runWorkload<HeapOrderBook>, &w)
            ->Unit(benchmark::kMillisecond);
    }

    const Workload &mixed = workloads[3];
    const Workload &wide = workloads[4];
    benchmark::RegisterBenchmark("depth10/mixed/tick_array", topOfBookDepth<TickOrderBook>, &mixed);
    benchmark::RegisterBenchmark("depth10/mixed/sorted_map", topOfBookDepth<MapOrderBook>, &mixed);
    benchmark::RegisterBenchmark("depth10/mixed/priority_queue", topOfBookDepth<HeapOrderBook>, &mixed);
    benchmark::RegisterBenchmark("depth10/wide_sparse/tick_array", topOfBookDepth<TickOrderBook>, &wide);
    benchmark::RegisterBenchmark("depth10/wide_sparse/sorted_map", topOfBookDepth<MapOrderBook>, &wide);
    benchmark::RegisterBenchmark("depth10/wide_sparse/priority_queue", topOfBookDepth<HeapOrderBook>, &wide);

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
