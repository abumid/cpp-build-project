#include "order_book.h"

#include <format>

string Trade::toString() const {
    return format("Trade[{} {} @ {} buy:#{} sell:#{}]", ::toString(aggressor), qty, priceToString(price), buy_order_id,
                  sell_order_id);
}

string_view toString(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None: return "None";
        case RejectReason::WrongTicker: return "WrongTicker";
        case RejectReason::ZeroQuantity: return "ZeroQuantity";
        case RejectReason::InvalidPrice: return "InvalidPrice";
        case RejectReason::InvalidOrderId: return "InvalidOrderId";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::AlreadyExpired: return "AlreadyExpired";
    }
    return "?";
}

OrderBook::OrderBook(Ticker ticker, size_t max_orders, size_t max_levels, StpMode stp)
    : ticker_(ticker), stp_(stp), order_pool_(max_orders), level_pool_(max_levels), bids_(max_levels),
      asks_(max_levels), by_id_(max_orders), by_client_(max_orders), ttl_heap_(max_orders) {}

AddResult OrderBook::add(const Order &order) noexcept {
    AddResult result;
    Order o = order;
    o.filled = 0;
    o.status = OrderStatus::New;
    o.entry_time = now_;

    if (o.ticker != ticker_) result.reject = RejectReason::WrongTicker;
    else if (o.qty == 0) result.reject = RejectReason::ZeroQuantity;
    else if (o.type == OrderType::Limit && o.price <= 0) result.reject = RejectReason::InvalidPrice;
    else if (o.order_id == IdMap<Node *>::kEmpty) result.reject = RejectReason::InvalidOrderId;
    else if (by_id_.find(o.order_id) != nullptr) result.reject = RejectReason::DuplicateOrderId;
    else if (isExpired(o)) result.reject = RejectReason::AlreadyExpired;
    if (!result.accepted()) return result;

    emit(o, OrderEvent::Accepted);
    result = o.side == Side::Buy ? match(o, asks_) : match(o, bids_);

    if (o.leaves() == 0) return result;

    const bool rested = o.type == OrderType::Limit && (o.side == Side::Buy ? rest(o, bids_) : rest(o, asks_));
    if (rested) {
        result.rested = o.leaves();
        return result;
    }

    result.cancelled += o.leaves();
    o.status = OrderStatus::Canceled;
    emit(o, OrderEvent::Canceled);
    return result;
}

bool OrderBook::cancel(OrderId order_id) noexcept {
    Node **slot = by_id_.find(order_id);
    if (slot == nullptr) return false;
    finishNode(*slot, OrderStatus::Canceled, OrderEvent::Canceled);
    return true;
}

bool OrderBook::update(OrderId order_id, Qty new_open_qty) noexcept {
    Node **slot = by_id_.find(order_id);
    if (slot == nullptr) return false;
    if (new_open_qty == 0) return cancel(order_id);

    Node *node = *slot;
    Order &o = node->order;
    const Qty open = o.leaves();
    if (new_open_qty == open) return true;

    if (new_open_qty < open) {
        const Qty cut = open - new_open_qty;
        o.qty -= cut;
        node->level->total -= cut;
    } else {
        PriceLevel *level = node->level;
        unlinkLevel(node);
        o.qty += new_open_qty - open;
        o.entry_time = now_;
        linkLevel(level, node);
    }
    emit(o, OrderEvent::Updated);
    return true;
}

size_t OrderBook::disconnect(ClientId client_id) noexcept {
    size_t cancelled = 0;
    while (Node **head = by_client_.find(client_id)) {
        finishNode(*head, OrderStatus::Canceled, OrderEvent::Canceled);
        ++cancelled;
    }
    return cancelled;
}

size_t OrderBook::expireOrders() noexcept {
    size_t expired = 0;
    while (!ttl_heap_.empty() && ttl_heap_.top().first <= now_) {
        const TtlEntry entry = ttl_heap_.top();
        ttl_heap_.pop();
        Node **slot = by_id_.find(entry.second);
        if (slot != nullptr && (*slot)->order.expire_at == entry.first) {
            finishNode(*slot, OrderStatus::Expired, OrderEvent::Expired);
            ++expired;
        }
    }
    return expired;
}

optional<Price> OrderBook::bestBid() noexcept {
    PriceLevel *level = bestLevel(bids_);
    return level ? optional<Price>(level->price) : nullopt;
}

optional<Price> OrderBook::bestAsk() noexcept {
    PriceLevel *level = bestLevel(asks_);
    return level ? optional<Price>(level->price) : nullopt;
}

Qty OrderBook::volumeAt(Side side, Price p) const noexcept {
    const IdMap<PriceLevel *> &levels = side == Side::Buy ? bids_.levels : asks_.levels;
    PriceLevel *const *slot = levels.find(static_cast<uint64_t>(p));
    return slot ? (*slot)->total : 0;
}

const Order *OrderBook::find(OrderId order_id) const noexcept {
    Node *const *slot = by_id_.find(order_id);
    return slot ? &(*slot)->order : nullptr;
}

vector<OrderId> OrderBook::queueAt(Side side, Price p) const {
    vector<OrderId> ids;
    const IdMap<PriceLevel *> &levels = side == Side::Buy ? bids_.levels : asks_.levels;
    if (PriceLevel *const *slot = levels.find(static_cast<uint64_t>(p)))
        for (const Node *n = (*slot)->head; n; n = n->next) ids.push_back(n->order.order_id);
    return ids;
}

vector<OrderId> OrderBook::ordersOf(ClientId client_id) const {
    vector<OrderId> ids;
    if (Node *const *head = by_client_.find(client_id))
        for (const Node *n = *head; n; n = n->client_next) ids.push_back(n->order.order_id);
    return ids;
}

string OrderBook::toString(size_t depth) const {
    auto collect = [](const auto &side) {
        vector<const PriceLevel *> out;
        for (Price p : side.heap.raw()) {
            const PriceLevel *level = *side.levels.find(static_cast<uint64_t>(p));
            if (level->head) out.push_back(level);
        }
        return out;
    };
    vector<const PriceLevel *> asks = collect(asks_), bids = collect(bids_);
    sort(asks.begin(), asks.end(), [](auto a, auto b) { return a->price < b->price; });
    sort(bids.begin(), bids.end(), [](auto a, auto b) { return a->price > b->price; });
    if (asks.size() > depth) asks.resize(depth);
    if (bids.size() > depth) bids.resize(depth);

    string out = format("OrderBook {} | {} orders | {} bid levels | {} ask levels | t={}\n", ticker_.view(),
                        orderCount(), bids_.live, asks_.live, now_);
    auto row = [&out](const char *label, const PriceLevel *level) {
        out += format("  {} {:>9} | total {:>6} |", label, priceToString(level->price), level->total);
        for (const Node *n = level->head; n; n = n->next)
            out += format(" [#{} c{} {}]", n->order.order_id, n->order.client_id, n->order.leaves());
        out += '\n';
    };
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) row("ASK", *it);
    if (!asks.empty() && !bids.empty())
        out += format("  ---------- spread {} ----------\n", priceToString(asks.front()->price - bids.front()->price));
    else
        out += "  --------------------------------\n";
    for (const PriceLevel *level : bids) row("BID", level);
    return out;
}

template <typename S>
OrderBook::PriceLevel *OrderBook::bestLevel(S &side) noexcept {
    while (!side.heap.empty()) {
        const Price p = side.heap.top();
        PriceLevel *level = *side.levels.find(static_cast<uint64_t>(p));
        if (level->head) return level;
        side.heap.pop();
        side.levels.erase(static_cast<uint64_t>(p));
        level_pool_.deallocate(level);
    }
    return nullptr;
}

template <typename S>
void OrderBook::compact(S &side) noexcept {
    vector<Price> &prices = side.heap.raw();
    size_t kept = 0;
    for (Price p : prices) {
        PriceLevel *level = *side.levels.find(static_cast<uint64_t>(p));
        if (level->head) {
            prices[kept++] = p;
        } else {
            side.levels.erase(static_cast<uint64_t>(p));
            level_pool_.deallocate(level);
        }
    }
    prices.resize(kept);
    side.heap.rebuild();
}

template <typename S>
OrderBook::PriceLevel *OrderBook::levelFor(S &side, Price p, Side s) noexcept {
    if (PriceLevel **slot = side.levels.find(static_cast<uint64_t>(p))) return *slot;
    if (level_pool_.available() == 0) {
        compact(bids_);
        compact(asks_);
    }
    PriceLevel *level = level_pool_.allocate(PriceLevel{p, s});
    if (level == nullptr) return nullptr;
    side.levels.insert(static_cast<uint64_t>(p), level);
    side.heap.push(p);
    return level;
}

template <typename S>
AddResult OrderBook::match(Order &taker, S &opposite) noexcept {
    AddResult result;
    while (taker.leaves() > 0) {
        PriceLevel *level = bestLevel(opposite);
        if (level == nullptr) break;
        const bool crosses = taker.side == Side::Buy ? level->price <= taker.price : level->price >= taker.price;
        if (taker.type == OrderType::Limit && !crosses) break;

        Node *maker = level->head;
        if (isExpired(maker->order)) {
            finishNode(maker, OrderStatus::Expired, OrderEvent::Expired);
            continue;
        }
        if (stp_ == StpMode::DecrementAndCancel && maker->order.client_id == taker.client_id) {
            selfTradeDecrement(taker, maker, result);
            continue;
        }
        execute(taker, maker, min(taker.leaves(), maker->order.leaves()), result);
    }
    return result;
}

template <typename S>
bool OrderBook::rest(Order &order, S &side) noexcept {
    PriceLevel *level = levelFor(side, order.price, order.side);
    if (level == nullptr) return false;
    Node *node = order_pool_.allocate();
    if (node == nullptr) return false;

    node->order = order;
    linkLevel(level, node);
    linkClient(node);
    by_id_.insert(order.order_id, node);
    if (order.hasTtl()) pushTtl(order);
    return true;
}

void OrderBook::execute(Order &taker, Node *maker_node, Qty qty, AddResult &result) noexcept {
    Order &maker = maker_node->order;
    const Price p = maker_node->level->price;

    maker.filled += qty;
    taker.filled += qty;
    maker_node->level->total -= qty;
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
    if (maker.leaves() == 0) finishNode(maker_node, OrderStatus::Filled, OrderEvent::Traded);
    else emit(maker, OrderEvent::Traded);
}

void OrderBook::selfTradeDecrement(Order &taker, Node *maker_node, AddResult &result) noexcept {
    Order &maker = maker_node->order;
    const Qty cut = min(taker.leaves(), maker.leaves());

    taker.qty -= cut;
    result.cancelled += cut;
    maker.qty -= cut;
    maker_node->level->total -= cut;

    if (maker.leaves() == 0) finishNode(maker_node, OrderStatus::Canceled, OrderEvent::SelfTradeCanceled);
    else emit(maker, OrderEvent::SelfTradeCanceled);

    if (taker.leaves() == 0) {
        taker.status = OrderStatus::Canceled;
        emit(taker, OrderEvent::SelfTradeCanceled);
    }
}

void OrderBook::finishNode(Node *node, OrderStatus status, OrderEvent event) noexcept {
    node->order.status = status;
    emit(node->order, event);
    unlinkLevel(node);
    unlinkClient(node);
    by_id_.erase(node->order.order_id);
    order_pool_.deallocate(node);
}

void OrderBook::linkLevel(PriceLevel *level, Node *node) noexcept {
    if (level->head == nullptr) ++(level->side == Side::Buy ? bids_.live : asks_.live);
    node->level = level;
    node->prev = level->tail;
    node->next = nullptr;
    if (level->tail) level->tail->next = node;
    else level->head = node;
    level->tail = node;
    level->total += node->order.leaves();
}

void OrderBook::unlinkLevel(Node *node) noexcept {
    PriceLevel *level = node->level;
    if (node->prev) node->prev->next = node->next;
    else level->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else level->tail = node->prev;
    level->total -= node->order.leaves();
    if (level->head == nullptr) --(level->side == Side::Buy ? bids_.live : asks_.live);
}

void OrderBook::linkClient(Node *node) noexcept {
    node->client_prev = nullptr;
    node->client_next = nullptr;
    if (Node **head = by_client_.find(node->order.client_id)) {
        node->client_next = *head;
        (*head)->client_prev = node;
        *head = node;
    } else {
        by_client_.insert(node->order.client_id, node);
    }
}

void OrderBook::unlinkClient(Node *node) noexcept {
    if (node->client_next) node->client_next->client_prev = node->client_prev;
    if (node->client_prev) {
        node->client_prev->client_next = node->client_next;
    } else if (node->client_next) {
        *by_client_.find(node->order.client_id) = node->client_next;
    } else {
        by_client_.erase(node->order.client_id);
    }
}

void OrderBook::pushTtl(const Order &order) noexcept {
    if (ttl_heap_.full()) {
        vector<TtlEntry> &entries = ttl_heap_.raw();
        size_t kept = 0;
        for (const TtlEntry &e : entries) {
            Node **slot = by_id_.find(e.second);
            if (slot && (*slot)->order.expire_at == e.first) entries[kept++] = e;
        }
        entries.resize(kept);
        ttl_heap_.rebuild();
    }
    ttl_heap_.push({order.expire_at, order.order_id});
}

void OrderBook::emit(const Order &order, OrderEvent event) noexcept {
    if (listener_) listener_->onOrderEvent(order, event);
}
