#include "map_order_book.h"

#include <algorithm>
#include <format>

MapOrderBook::MapOrderBook(Ticker ticker, size_t expected_orders) : ticker_(ticker) {
    if (expected_orders) index_.reserve(expected_orders);
}

AddResult MapOrderBook::add(const Order &order) {
    AddResult result;
    Order o = order;
    o.filled = 0;
    o.status = OrderStatus::New;

    if (o.ticker != ticker_) result.reject = RejectReason::WrongTicker;
    else if (o.qty == 0) result.reject = RejectReason::ZeroQuantity;
    else if (o.type == OrderType::Limit && o.price <= 0) result.reject = RejectReason::InvalidPrice;
    else if (index_.contains(o.order_id)) result.reject = RejectReason::DuplicateOrderId;
    if (!result.accepted()) return result;

    emit(o, OrderEvent::Accepted);
    if (o.side == Side::Buy) match(o, asks_, result);
    else match(o, bids_, result);
    if (o.leaves() == 0) return result;

    if (o.type == OrderType::Market) {
        result.cancelled += o.leaves();
        o.status = OrderStatus::Canceled;
        emit(o, OrderEvent::Canceled);
        return result;
    }

    Level &level = o.side == Side::Buy ? bids_[o.price] : asks_[o.price];
    level.total += o.leaves();
    level.orders.push_back(o);
    index_.emplace(o.order_id, Locator{&level, prev(level.orders.end())});
    result.rested = o.leaves();
    return result;
}

bool MapOrderBook::cancel(OrderId order_id) {
    auto found = index_.find(order_id);
    if (found == index_.end()) return false;
    Locator loc = found->second;
    index_.erase(found);

    Order &o = *loc.it;
    o.status = OrderStatus::Canceled;
    emit(o, OrderEvent::Canceled);
    const Side side = o.side;
    const Price p = o.price;
    loc.level->total -= o.leaves();
    loc.level->orders.erase(loc.it);
    if (loc.level->orders.empty()) {
        if (side == Side::Buy) bids_.erase(p);
        else asks_.erase(p);
    }
    return true;
}

optional<Price> MapOrderBook::bestBid() const noexcept {
    return bids_.empty() ? nullopt : optional<Price>(bids_.begin()->first);
}

optional<Price> MapOrderBook::bestAsk() const noexcept {
    return asks_.empty() ? nullopt : optional<Price>(asks_.begin()->first);
}

Qty MapOrderBook::volumeAt(Side side, Price p) const {
    if (side == Side::Buy) {
        auto it = bids_.find(p);
        return it == bids_.end() ? 0 : it->second.total;
    }
    auto it = asks_.find(p);
    return it == asks_.end() ? 0 : it->second.total;
}

const Order *MapOrderBook::find(OrderId order_id) const {
    auto it = index_.find(order_id);
    return it == index_.end() ? nullptr : &*it->second.it;
}

vector<OrderId> MapOrderBook::queueAt(Side side, Price p) const {
    vector<OrderId> ids;
    const Level *level = nullptr;
    if (side == Side::Buy) {
        if (auto it = bids_.find(p); it != bids_.end()) level = &it->second;
    } else if (auto it = asks_.find(p); it != asks_.end()) {
        level = &it->second;
    }
    if (level)
        for (const Order &o : level->orders) ids.push_back(o.order_id);
    return ids;
}

size_t MapOrderBook::depth(Side side, size_t n, LevelInfo *out) const noexcept {
    return side == Side::Buy ? walk(bids_, n, out) : walk(asks_, n, out);
}

string MapOrderBook::toString(size_t depth_limit) const {
    string out = format("MapOrderBook {} | {} orders | {} bid levels | {} ask levels\n", ticker_.view(), orderCount(),
                        bids_.size(), asks_.size());
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

template <typename Levels>
void MapOrderBook::match(Order &taker, Levels &opposite, AddResult &result) {
    while (taker.leaves() > 0 && !opposite.empty()) {
        auto best = opposite.begin();
        const Price p = best->first;
        const bool crosses = taker.side == Side::Buy ? p <= taker.price : p >= taker.price;
        if (taker.type == OrderType::Limit && !crosses) break;

        Level &level = best->second;
        Order &maker = level.orders.front();
        const Qty qty = min(taker.leaves(), maker.leaves());
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
                                     taker_buys ? maker.client_id : taker.client_id, taker.side, p, qty});
        emit(taker, OrderEvent::Traded);
        emit(maker, OrderEvent::Traded);

        if (maker.leaves() == 0) {
            index_.erase(maker.order_id);
            level.orders.pop_front();
            if (level.orders.empty()) opposite.erase(best);
        }
    }
}

template <typename Levels>
size_t MapOrderBook::walk(const Levels &levels, size_t n, LevelInfo *out) const noexcept {
    size_t written = 0;
    for (auto it = levels.begin(); it != levels.end() && written < n; ++it)
        out[written++] = LevelInfo{it->first, it->second.total, static_cast<uint32_t>(it->second.orders.size())};
    return written;
}
