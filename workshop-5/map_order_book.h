#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "book_types.h"

using namespace std;

class MapOrderBook final {
public:
    explicit MapOrderBook(Ticker ticker, size_t expected_orders = 0);

    void setListener(OrderBookListener *listener) noexcept { listener_ = listener; }

    AddResult add(const Order &order);
    bool cancel(OrderId order_id);

    optional<Price> bestBid() const noexcept;
    optional<Price> bestAsk() const noexcept;
    Qty volumeAt(Side side, Price p) const;
    const Order *find(OrderId order_id) const;
    vector<OrderId> queueAt(Side side, Price p) const;
    size_t depth(Side side, size_t n, LevelInfo *out) const noexcept;

    size_t orderCount() const noexcept { return index_.size(); }
    size_t levelCount(Side side) const noexcept { return side == Side::Buy ? bids_.size() : asks_.size(); }

    string toString(size_t depth = 10) const;

private:
    struct Level {
        list<Order> orders;
        Qty total = 0;
    };

    struct Locator {
        Level *level;
        list<Order>::iterator it;
    };

    template <typename Levels> void match(Order &taker, Levels &opposite, AddResult &result);
    template <typename Levels> size_t walk(const Levels &levels, size_t n, LevelInfo *out) const noexcept;
    void emit(const Order &order, OrderEvent event) {
        if (listener_) listener_->onOrderEvent(order, event);
    }

    Ticker ticker_;
    OrderBookListener *listener_ = nullptr;
    map<Price, Level, greater<Price>> bids_;
    map<Price, Level> asks_;
    unordered_map<OrderId, Locator> index_;
};
