#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "book_types.h"
#include "id_map.h"
#include "mem_pool.h"

using namespace std;

class TickOrderBook final {
public:
    TickOrderBook(Ticker ticker, Price min_price, Price max_price, Price tick_size, size_t max_orders);

    void setListener(OrderBookListener *listener) noexcept { listener_ = listener; }

    AddResult add(const Order &order) noexcept;
    bool cancel(OrderId order_id) noexcept;

    optional<Price> bestBid() const noexcept;
    optional<Price> bestAsk() const noexcept;
    Qty volumeAt(Side side, Price p) const noexcept;
    const Order *find(OrderId order_id) const noexcept;
    vector<OrderId> queueAt(Side side, Price p) const;
    size_t depth(Side side, size_t n, LevelInfo *out) const noexcept;

    size_t orderCount() const noexcept { return order_pool_.size(); }
    size_t levelCount(Side side) const noexcept { return side == Side::Buy ? live_bids_ : live_asks_; }
    size_t tickCount() const noexcept { return levels_.size(); }
    Price minPrice() const noexcept { return min_price_; }
    Price maxPrice() const noexcept { return max_price_; }
    Price tickSize() const noexcept { return tick_; }

    string toString(size_t depth = 10) const;

    TickOrderBook(const TickOrderBook &) = delete;
    TickOrderBook &operator=(const TickOrderBook &) = delete;

private:
    using Index = int64_t;

    struct Node {
        Order order;
        Node *prev = nullptr;
        Node *next = nullptr;
        Index index = 0;
    };

    struct Level {
        Node *head = nullptr;
        Node *tail = nullptr;
        Qty total = 0;
        uint32_t count = 0;
        Side side = Side::Buy;
    };

    bool onTick(Price p) const noexcept { return p >= min_price_ && p <= max_price_ && (p - min_price_) % tick_ == 0; }
    Index indexOf(Price p) const noexcept { return (p - min_price_) / tick_; }
    Price priceOf(Index i) const noexcept { return min_price_ + i * tick_; }
    Index noAsk() const noexcept { return static_cast<Index>(levels_.size()); }

    void match(Order &taker, AddResult &result) noexcept;
    void execute(Order &taker, Node *maker, Qty qty, AddResult &result) noexcept;
    bool rest(Order &order) noexcept;
    void link(Index i, Node *node) noexcept;
    void unlink(Node *node) noexcept;
    void removeNode(Node *node) noexcept;
    void levelEmptied(Index i, Side side) noexcept;
    void emit(const Order &order, OrderEvent event) noexcept {
        if (listener_) listener_->onOrderEvent(order, event);
    }

    Ticker ticker_;
    Price min_price_;
    Price max_price_;
    Price tick_;
    OrderBookListener *listener_ = nullptr;

    vector<Level> levels_;
    Index best_bid_ = -1;
    Index best_ask_ = 0;
    size_t live_bids_ = 0;
    size_t live_asks_ = 0;

    MemPool<Node> order_pool_;
    IdMap<Node *> by_id_;
};
