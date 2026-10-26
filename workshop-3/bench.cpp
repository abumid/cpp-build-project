#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

#include "mem_pool.h"
#include "order.h"
#include "order_book.h"

using namespace std;
using Clk = chrono::steady_clock;

template <typename F>
static double nsPerOp(size_t ops, F &&f) {
    auto t0 = Clk::now();
    f();
    return chrono::duration<double, nano>(Clk::now() - t0).count() / static_cast<double>(ops);
}

static Millis zeroClock() { return 0; }

int main() {
    constexpr size_t N = 100'000, ROUNDS = 50;
    vector<Order *> ptrs;
    ptrs.reserve(N);
    volatile int64_t sink = 0;

    double heap = nsPerOp(N * ROUNDS, [&] {
        for (size_t r = 0; r < ROUNDS; ++r) {
            for (size_t i = 0; i < N; ++i) ptrs.push_back(new Order());
            for (Order *p : ptrs) { sink = sink + p->price; delete p; }
            ptrs.clear();
        }
    });

    MemPool<Order> pool(N);
    double pooled = nsPerOp(N * ROUNDS, [&] {
        for (size_t r = 0; r < ROUNDS; ++r) {
            for (size_t i = 0; i < N; ++i) ptrs.push_back(pool.allocate());
            for (Order *p : ptrs) { sink = sink + p->price; pool.deallocate(p); }
            ptrs.clear();
        }
    });

    printf("new/delete      : %6.2f ns per alloc+free\n", heap);
    printf("MemPool<Order>  : %6.2f ns per alloc+free  (%.1fx)\n", pooled, heap / pooled);

    SymbolInfo info = SymbolInfo::btcusdt();
    info.max_num_orders = 0;
    info.min_notional = 0;
    constexpr size_t OPS = 2'000'000;
    OrderBook book(info, 200'000, 8'192, &zeroClock);
    mt19937 rng(1);
    normal_distribution<double> px(6'725'000, 800);
    uniform_int_distribution<int> qty(1, 100), pct(0, 99);
    struct Op { bool cancel; Order o; OrderId id; };
    vector<Op> ops;
    ops.reserve(OPS);
    OrderId issued = 0;
    for (size_t i = 0; i < OPS; ++i) {
        if (pct(rng) < 40 && issued > 5000) {
            ops.push_back({true, {}, uniform_int_distribution<OrderId>(issued - 5000, issued)(rng)});
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            ops.push_back({false, Order::limit("BTCUSDT", rng() % 1000, s, static_cast<Decimal>(px(rng)) * "0.01"_d,
                                               qty(rng) * "0.0001"_d).withStp(StpMode::None), 0});
            ++issued;
        }
    }
    NullListener none;
    double book_ns = nsPerOp(OPS, [&] {
        for (const Op &op : ops) {
            if (op.cancel) book.cancel(op.id, none);
            else book.add(op.o, none);
        }
    });
    printf("OrderBook       : %6.2f ns per add/cancel  (%zu ops, %zu resting, last %s)\n", book_ns, OPS,
           book.orderCount(), toString(book.lastPrice(), 2).c_str());
    printf("\n%s", book.toString(5).c_str());
    return static_cast<int>(sink & 0);
}
