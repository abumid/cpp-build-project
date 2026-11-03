#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "id_map.h"
#include "mem_pool.h"
#include "order.h"

using namespace std;

template <typename T, typename Compare>
class ReservedHeap : public priority_queue<T, vector<T>, Compare> {
    using Base = priority_queue<T, vector<T>, Compare>;

public:
    explicit ReservedHeap(size_t capacity) : Base(Compare{}, reserved(capacity)), capacity_(capacity) {}

    bool full() const noexcept { return this->c.size() >= capacity_; }
    vector<T> &raw() noexcept { return this->c; }
    const vector<T> &raw() const noexcept { return this->c; }
    void rebuild() { make_heap(this->c.begin(), this->c.end(), this->comp); }

private:
    static vector<T> reserved(size_t n) {
        vector<T> v;
        v.reserve(n);
        return v;
    }
    size_t capacity_;
};

struct ForBids {
    bool operator()(Price a, Price b) const noexcept { return a < b; }
};

struct ForAsks {
    bool operator()(Price a, Price b) const noexcept { return a > b; }
};

using TtlEntry = pair<Millis, OrderId>;

struct Trade {
    OrderId buy_order_id;
    OrderId sell_order_id;
    ClientId buyer;
    ClientId seller;
    Side aggressor;
    Price price;
    Qty qty;

    string toString() const;
};

enum class OrderEvent : uint8_t { Accepted, Traded, Canceled, Expired, Updated, SelfTradeCanceled };

class OrderBookListener {
public:
    virtual ~OrderBookListener() = default;
    virtual void onTrade(const Trade &) {}
    virtual void onOrderEvent(const Order &, OrderEvent) {}
};

enum class RejectReason : uint8_t { None, WrongTicker, ZeroQuantity, InvalidPrice, InvalidOrderId, DuplicateOrderId, AlreadyExpired };

string_view toString(RejectReason r) noexcept;

struct AddResult {
    RejectReason reject = RejectReason::None;
    Qty filled = 0;
    Qty rested = 0;
    Qty cancelled = 0;

    bool accepted() const noexcept { return reject == RejectReason::None; }
};

enum class StpMode : uint8_t { None, DecrementAndCancel };

class OrderBook final {
public:
    OrderBook(Ticker ticker, size_t max_orders, size_t max_levels, StpMode stp = StpMode::None);

    void setListener(OrderBookListener *listener) noexcept { listener_ = listener; }
    void setTime(Millis now) noexcept { now_ = now; }
    Millis time() const noexcept { return now_; }

    AddResult add(const Order &order) noexcept;
    bool cancel(OrderId order_id) noexcept;
    bool update(OrderId order_id, Qty new_open_qty) noexcept;
    size_t disconnect(ClientId client_id) noexcept;
    size_t expireOrders() noexcept;

    optional<Price> bestBid() noexcept;
    optional<Price> bestAsk() noexcept;
    Qty volumeAt(Side side, Price p) const noexcept;
    const Order *find(OrderId order_id) const noexcept;
    vector<OrderId> queueAt(Side side, Price p) const;
    vector<OrderId> ordersOf(ClientId client_id) const;

    size_t orderCount() const noexcept { return order_pool_.size(); }
    size_t levelCount(Side side) const noexcept { return side == Side::Buy ? bids_.live : asks_.live; }
    size_t heapSize(Side side) const noexcept { return side == Side::Buy ? bids_.heap.size() : asks_.heap.size(); }
    size_t ttlHeapSize() const noexcept { return ttl_heap_.size(); }

    string toString(size_t depth = 10) const;

    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;

private:
    struct PriceLevel;

    struct Node {
        Order order;
        Node *prev = nullptr;
        Node *next = nullptr;
        Node *client_prev = nullptr;
        Node *client_next = nullptr;
        PriceLevel *level = nullptr;
    };

    struct PriceLevel {
        Price price;
        Side side;
        Qty total = 0;
        Node *head = nullptr;
        Node *tail = nullptr;
    };

    template <typename Compare>
    struct BookSide {
        ReservedHeap<Price, Compare> heap;
        IdMap<PriceLevel *> levels;
        size_t live = 0;

        explicit BookSide(size_t max_levels) : heap(max_levels), levels(max_levels) {}
    };

    template <typename S> PriceLevel *bestLevel(S &side) noexcept;
    template <typename S> PriceLevel *levelFor(S &side, Price p, Side s) noexcept;
    template <typename S> void compact(S &side) noexcept;
    template <typename S> AddResult match(Order &taker, S &opposite) noexcept;
    template <typename S> bool rest(Order &order, S &side) noexcept;

    void execute(Order &taker, Node *maker, Qty qty, AddResult &result) noexcept;
    void selfTradeDecrement(Order &taker, Node *maker, AddResult &result) noexcept;
    void finishNode(Node *node, OrderStatus status, OrderEvent event) noexcept;
    void linkLevel(PriceLevel *level, Node *node) noexcept;
    void unlinkLevel(Node *node) noexcept;
    void linkClient(Node *node) noexcept;
    void unlinkClient(Node *node) noexcept;
    void pushTtl(const Order &order) noexcept;
    void emit(const Order &order, OrderEvent event) noexcept;
    bool isExpired(const Order &order) const noexcept { return order.hasTtl() && order.expire_at <= now_; }

    Ticker ticker_;
    StpMode stp_;
    Millis now_ = 0;
    OrderBookListener *listener_ = nullptr;

    MemPool<Node> order_pool_;
    MemPool<PriceLevel> level_pool_;
    BookSide<ForBids> bids_;
    BookSide<ForAsks> asks_;
    IdMap<Node *> by_id_;
    IdMap<Node *> by_client_;
    ReservedHeap<TtlEntry, greater<TtlEntry>> ttl_heap_;
};
