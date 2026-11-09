#include "tick_order_book.h"

#include <algorithm>
#include <format>

TickOrderBook::TickOrderBook(Ticker ticker, Price min_price, Price max_price, Price tick_size, size_t max_orders)
    : ticker_(ticker), min_price_(min_price), max_price_(max_price), tick_(tick_size),
      levels_(static_cast<size_t>((max_price - min_price) / tick_size + 1)), order_pool_(max_orders),
      by_id_(max_orders) {
    best_ask_ = noAsk();
}

AddResult TickOrderBook::add(const Order &order) noexcept {
    AddResult result;
    Order o = order;
    o.filled = 0;
    o.status = OrderStatus::New;

    if (o.ticker != ticker_) result.reject = RejectReason::WrongTicker;
    else if (o.qty == 0) result.reject = RejectReason::ZeroQuantity;
    else if (o.type == OrderType::Limit && o.price <= 0) result.reject = RejectReason::InvalidPrice;
    else if (o.type == OrderType::Limit && !onTick(o.price)) result.reject = RejectReason::OutOfRange;
    else if (o.order_id == IdMap<Node *>::kEmpty) result.reject = RejectReason::InvalidOrderId;
    else if (by_id_.find(o.order_id) != nullptr) result.reject = RejectReason::DuplicateOrderId;
    if (!result.accepted()) return result;

    emit(o, OrderEvent::Accepted);
    match(o, result);
    if (o.leaves() == 0) return result;

    if (o.type == OrderType::Limit && rest(o)) {
        result.rested = o.leaves();
        return result;
    }
    result.cancelled += o.leaves();
    o.status = OrderStatus::Canceled;
    emit(o, OrderEvent::Canceled);
    return result;
}

bool TickOrderBook::cancel(OrderId order_id) noexcept {
    Node **slot = by_id_.find(order_id);
    if (slot == nullptr) return false;
    Node *node = *slot;
    node->order.status = OrderStatus::Canceled;
    emit(node->order, OrderEvent::Canceled);
    removeNode(node);
    return true;
}

optional<Price> TickOrderBook::bestBid() const noexcept {
    return best_bid_ >= 0 ? optional<Price>(priceOf(best_bid_)) : nullopt;
}

optional<Price> TickOrderBook::bestAsk() const noexcept {
    return best_ask_ < noAsk() ? optional<Price>(priceOf(best_ask_)) : nullopt;
}

Qty TickOrderBook::volumeAt(Side side, Price p) const noexcept {
    if (!onTick(p)) return 0;
    const Level &level = levels_[static_cast<size_t>(indexOf(p))];
    return level.head && level.side == side ? level.total : 0;
}

const Order *TickOrderBook::find(OrderId order_id) const noexcept {
    Node *const *slot = by_id_.find(order_id);
    return slot ? &(*slot)->order : nullptr;
}

vector<OrderId> TickOrderBook::queueAt(Side side, Price p) const {
    vector<OrderId> ids;
    if (!onTick(p)) return ids;
    const Level &level = levels_[static_cast<size_t>(indexOf(p))];
    if (level.side != side) return ids;
    for (const Node *n = level.head; n; n = n->next) ids.push_back(n->order.order_id);
    return ids;
}

size_t TickOrderBook::depth(Side side, size_t n, LevelInfo *out) const noexcept {
    size_t written = 0;
    n = min(n, side == Side::Buy ? live_bids_ : live_asks_);
    if (side == Side::Buy) {
        for (Index i = best_bid_; i >= 0 && written < n; --i)
            if (const Level &l = levels_[static_cast<size_t>(i)]; l.head) out[written++] = {priceOf(i), l.total, l.count};
    } else {
        for (Index i = best_ask_; i < noAsk() && written < n; ++i)
            if (const Level &l = levels_[static_cast<size_t>(i)]; l.head) out[written++] = {priceOf(i), l.total, l.count};
    }
    return written;
}

string TickOrderBook::toString(size_t depth_limit) const {
    string out = format("TickOrderBook {} | {} orders | {} bid levels | {} ask levels | {} ticks\n", ticker_.view(),
                        orderCount(), live_bids_, live_asks_, levels_.size());
    vector<LevelInfo> asks(depth_limit), bids(depth_limit);
    asks.resize(depth(Side::Sell, depth_limit, asks.data()));
    bids.resize(depth(Side::Buy, depth_limit, bids.data()));
    auto row = [&out](const char *label, const LevelInfo &l) {
        out += format("  {} {:>9} | total {:>6} | {} orders\n", label, priceToString(l.price), l.total, l.orders);
    };
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) row("ASK", *it);
    if (!asks.empty() && !bids.empty())
        out += format("  ---------- spread {} ----------\n", priceToString(asks.front().price - bids.front().price));
    else
        out += "  --------------------------------\n";
    for (const LevelInfo &l : bids) row("BID", l);
    return out;
}

void TickOrderBook::match(Order &taker, AddResult &result) noexcept {
    const bool buying = taker.side == Side::Buy;
    const Index limit = taker.type == OrderType::Market ? (buying ? noAsk() - 1 : 0) : indexOf(taker.price);

    while (taker.leaves() > 0) {
        const Index best = buying ? best_ask_ : best_bid_;
        if (buying ? (best >= noAsk() || best > limit) : (best < 0 || best < limit)) break;
        Node *maker = levels_[static_cast<size_t>(best)].head;
        execute(taker, maker, min(taker.leaves(), maker->order.leaves()), result);
    }
}

void TickOrderBook::execute(Order &taker, Node *maker_node, Qty qty, AddResult &result) noexcept {
    Order &maker = maker_node->order;
    Level &level = levels_[static_cast<size_t>(maker_node->index)];

    maker.filled += qty;
    taker.filled += qty;
    level.total -= qty;
    result.filled += qty;
    maker.status = maker.leaves() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
    taker.status = taker.leaves() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;

    const bool taker_buys = taker.side == Side::Buy;
    if (listener_)
        listener_->onTrade(Trade{taker_buys ? taker.order_id : maker.order_id,
                                 taker_buys ? maker.order_id : taker.order_id,
                                 taker_buys ? taker.client_id : maker.client_id,
                                 taker_buys ? maker.client_id : taker.client_id, taker.side,
                                 priceOf(maker_node->index), qty});
    emit(taker, OrderEvent::Traded);
    emit(maker, OrderEvent::Traded);
    if (maker.leaves() == 0) removeNode(maker_node);
}

bool TickOrderBook::rest(Order &order) noexcept {
    Node *node = order_pool_.allocate();
    if (node == nullptr) return false;
    node->order = order;
    const Index i = indexOf(order.price);
    link(i, node);
    by_id_.insert(order.order_id, node);
    if (order.side == Side::Buy) best_bid_ = max(best_bid_, i);
    else best_ask_ = min(best_ask_, i);
    return true;
}

void TickOrderBook::link(Index i, Node *node) noexcept {
    Level &level = levels_[static_cast<size_t>(i)];
    if (level.head == nullptr) {
        level.side = node->order.side;
        ++(level.side == Side::Buy ? live_bids_ : live_asks_);
    }
    node->index = i;
    node->prev = level.tail;
    node->next = nullptr;
    if (level.tail) level.tail->next = node;
    else level.head = node;
    level.tail = node;
    level.total += node->order.leaves();
    ++level.count;
}

void TickOrderBook::unlink(Node *node) noexcept {
    Level &level = levels_[static_cast<size_t>(node->index)];
    if (node->prev) node->prev->next = node->next;
    else level.head = node->next;
    if (node->next) node->next->prev = node->prev;
    else level.tail = node->prev;
    level.total -= node->order.leaves();
    --level.count;
}

void TickOrderBook::removeNode(Node *node) noexcept {
    const Index i = node->index;
    const Side side = node->order.side;
    unlink(node);
    by_id_.erase(node->order.order_id);
    order_pool_.deallocate(node);
    if (levels_[static_cast<size_t>(i)].head == nullptr) levelEmptied(i, side);
}

void TickOrderBook::levelEmptied(Index i, Side side) noexcept {
    if (side == Side::Buy) {
        if (--live_bids_ == 0) best_bid_ = -1;
        else if (i == best_bid_)
            while (levels_[static_cast<size_t>(best_bid_)].head == nullptr) --best_bid_;
    } else {
        if (--live_asks_ == 0) best_ask_ = noAsk();
        else if (i == best_ask_)
            while (levels_[static_cast<size_t>(best_ask_)].head == nullptr) ++best_ask_;
    }
}
