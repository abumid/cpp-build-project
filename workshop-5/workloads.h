#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "map_order_book.h"

using namespace std;

enum class OpType : uint8_t { Add, Cancel };

struct Op {
    OpType type;
    OrderId cancel_id;
    Order order;
};

enum class WorkloadKind : uint8_t { AddOnly, CancelHeavy, Aggressive, Mixed, WideSparse };

struct Workload {
    string name;
    string description;
    vector<Op> prefill;
    vector<Op> ops;
    Price min_price;
    Price max_price;
    size_t max_orders;
    size_t max_levels;
};

inline constexpr Price kMid = price(1000, 0);
inline constexpr Price kRange = 50'000;
inline const Ticker kTicker = "BENCH";

class WorkloadBuilder {
public:
    explicit WorkloadBuilder(uint32_t seed) : rng_(seed), shadow_(kTicker, 400'000) {}

    Workload build(WorkloadKind kind, size_t n_ops) {
        Workload w;
        w.min_price = kMid - kRange;
        w.max_price = kMid + kRange;
        w.max_levels = static_cast<size_t>(2 * kRange + 1);

        switch (kind) {
            case WorkloadKind::AddOnly:
                w.name = "add_only";
                w.description = "passive limit orders within 200 ticks of the touch, nothing crosses";
                for (size_t i = 0; i < n_ops; ++i) w.ops.push_back(passive(200));
                break;
            case WorkloadKind::CancelHeavy:
                w.name = "cancel_heavy";
                w.description = "steady book of ~20k orders: 49% passive adds, 49% cancels, 2% small market orders (25 cancels per trade)";
                prefill(w, 20'000, 100);
                for (size_t i = 0; i < n_ops; ++i) {
                    const int roll = pct();
                    if (roll < 49) w.ops.push_back(passive(100));
                    else if (roll < 98 && !live_.empty()) w.ops.push_back(cancelLive());
                    else w.ops.push_back(market(1, 20));
                }
                break;
            case WorkloadKind::Aggressive:
                w.name = "aggressive";
                w.description = "matching-heavy: 50% passive adds near the touch, 50% orders that cross 0-5 ticks deep";
                prefill(w, 20'000, 50);
                for (size_t i = 0; i < n_ops; ++i) w.ops.push_back(pct() < 50 ? passive(10) : crossing(5));
                break;
            case WorkloadKind::Mixed:
                w.name = "mixed";
                w.description = "45% passive adds, 35% cancels, 10% orders around the mid (some cross), 10% market orders";
                prefill(w, 20'000, 100);
                for (size_t i = 0; i < n_ops; ++i) {
                    const int roll = pct();
                    if (roll < 45) w.ops.push_back(passive(50));
                    else if (roll < 80 && !live_.empty()) w.ops.push_back(cancelLive());
                    else if (roll < 90) w.ops.push_back(nearMid(20));
                    else w.ops.push_back(market(1, 50));
                }
                break;
            case WorkloadKind::WideSparse:
                w.name = "wide_sparse";
                w.description = "~2k orders spread over +/-40,000 ticks: 45% adds, 45% cancels (half of them at the best price), 10% market";
                for (size_t i = 0; i < 2'000; ++i) w.prefill.push_back(sparse(40'000));
                for (size_t i = 0; i < n_ops; ++i) {
                    const int roll = pct();
                    if (roll < 45) w.ops.push_back(sparse(40'000));
                    else if (roll < 90 && !live_.empty()) w.ops.push_back(roll < 68 ? cancelBest() : cancelLive());
                    else w.ops.push_back(market(1, 5));
                }
                break;
        }
        w.max_orders = static_cast<size_t>(next_id_) + 16;
        return w;
    }

private:
    int pct() { return uniform_int_distribution<int>(0, 99)(rng_); }
    Qty qty(Qty lo, Qty hi) { return uniform_int_distribution<Qty>(lo, hi)(rng_); }
    Side side() { return pct() < 50 ? Side::Buy : Side::Sell; }

    Price bid() { return shadow_.bestBid().value_or(kMid - 1); }
    Price ask() { return shadow_.bestAsk().value_or(kMid + 1); }

    Price clampPrice(Price p) const { return max(kMid - kRange, min(kMid + kRange, p)); }

    Op record(const Order &o) {
        shadow_.add(o);
        if (shadow_.find(o.order_id)) live_.push_back(o.order_id);
        return Op{OpType::Add, 0, o};
    }

    Op passive(Price spread) {
        const Side s = side();
        const Price offset = uniform_int_distribution<Price>(0, spread)(rng_);
        const Price p = s == Side::Buy ? clampPrice(min(bid(), ask() - 1) - offset) : clampPrice(max(ask(), bid() + 1) + offset);
        return record(Order::limit(kTicker, next_id_++, 1 + pct() % 50, s, p, qty(1, 100)));
    }

    Op sparse(Price spread) {
        const Side s = side();
        const Price offset = uniform_int_distribution<Price>(1, spread)(rng_);
        const Price p = s == Side::Buy ? min(kMid - offset, ask() - 1) : max(kMid + offset, bid() + 1);
        return record(Order::limit(kTicker, next_id_++, 1 + pct() % 50, s, clampPrice(p), qty(1, 100)));
    }

    Op nearMid(double sigma) {
        normal_distribution<double> d(0.0, sigma);
        const Price p = clampPrice(kMid + static_cast<Price>(d(rng_)));
        return record(Order::limit(kTicker, next_id_++, 1 + pct() % 50, side(), p, qty(1, 100)));
    }

    Op crossing(Price depth) {
        const Side s = side();
        const Price through = uniform_int_distribution<Price>(0, depth)(rng_);
        const Price p = s == Side::Buy ? clampPrice(ask() + through) : clampPrice(bid() - through);
        return record(Order::limit(kTicker, next_id_++, 1 + pct() % 50, s, p, qty(50, 400)));
    }

    Op market(Qty lo, Qty hi) {
        return record(Order::market(kTicker, next_id_++, 1 + pct() % 50, side(), qty(lo, hi)));
    }

    Op cancelLive() {
        while (true) {
            const size_t i = uniform_int_distribution<size_t>(0, live_.size() - 1)(rng_);
            const OrderId id = live_[i];
            live_[i] = live_.back();
            live_.pop_back();
            if (shadow_.find(id)) {
                shadow_.cancel(id);
                return Op{OpType::Cancel, id, {}};
            }
            if (live_.empty()) return passive(10);
        }
    }

    Op cancelBest() {
        const Side s = side();
        LevelInfo top;
        if (shadow_.depth(s, 1, &top) == 1) {
            const vector<OrderId> queue = shadow_.queueAt(s, top.price);
            shadow_.cancel(queue.front());
            return Op{OpType::Cancel, queue.front(), {}};
        }
        return cancelLive();
    }

    void prefill(Workload &w, size_t n, Price spread) {
        for (size_t i = 0; i < n; ++i) w.prefill.push_back(passive(spread));
    }

    mt19937 rng_;
    MapOrderBook shadow_;
    vector<OrderId> live_;
    OrderId next_id_ = 1;
};

template <typename Book>
inline void apply(Book &book, const Op &op) {
    if (op.type == OpType::Add) book.add(op.order);
    else book.cancel(op.cancel_id);
}
