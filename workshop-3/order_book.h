#pragma once

#include <cassert>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "id_map.h"
#include "mem_pool.h"
#include "order.h"

using namespace std;

struct SymbolInfo {
    Symbol symbol;
    Asset base_asset;
    Asset quote_asset;

    Decimal min_price = 0;
    Decimal max_price = 0;
    Decimal tick_size = 0;

    Decimal min_qty = 0;
    Decimal max_qty = 0;
    Decimal step_size = 0;

    Decimal market_min_qty = 0;
    Decimal market_max_qty = 0;
    Decimal market_step_size = 0;

    Decimal min_notional = 0;
    Decimal max_notional = 0;
    bool apply_min_to_market = false;
    bool apply_max_to_market = false;

    uint32_t max_num_orders = 0;
    StpMode default_stp = StpMode::ExpireMaker;

    static SymbolInfo btcusdt() {
        SymbolInfo s;
        s.symbol = "BTCUSDT";
        s.base_asset = "BTC";
        s.quote_asset = "USDT";
        s.min_price = "0.01"_d;
        s.max_price = "1000000"_d;
        s.tick_size = "0.01"_d;
        s.min_qty = "0.00001"_d;
        s.max_qty = "9000"_d;
        s.step_size = "0.00001"_d;
        s.market_min_qty = 0;
        s.market_max_qty = "100"_d;
        s.market_step_size = "0.00001"_d;
        s.min_notional = "5"_d;
        s.max_notional = "9000000"_d;
        s.apply_min_to_market = true;
        s.apply_max_to_market = false;
        s.max_num_orders = 200;
        s.default_stp = StpMode::ExpireMaker;
        return s;
    }
};

enum class ExecType : uint8_t { New, Trade, Canceled, Expired, TradePrevention };

constexpr string_view toString(ExecType e) noexcept {
    switch (e) {
        case ExecType::New: return "NEW";
        case ExecType::Trade: return "TRADE";
        case ExecType::Canceled: return "CANCELED";
        case ExecType::Expired: return "EXPIRED";
        case ExecType::TradePrevention: return "TRADE_PREVENTION";
    }
    return "?";
}

struct ExecutionReport {
    ExecType exec_type;
    Order order;
    Decimal last_qty = 0;
    Decimal last_price = 0;
    TradeId trade_id = 0;
    bool is_maker = false;
    Millis event_time = 0;

    string toString() const {
        return format(
            R"({{"e":"executionReport","E":{},"s":"{}","c":"{}","S":"{}","o":"{}","f":"{}","q":"{}","p":"{}",)"
            R"("x":"{}","X":"{}","i":{},"l":"{}","z":"{}","L":"{}","T":{},"t":{},"m":{},"Z":"{}","Q":"{}","V":"{}"}})",
            event_time, order.symbol.view(), order.client_order_id.view(), ::toString(order.side),
            ::toString(order.type), ::toString(order.time_in_force), ::toString(order.orig_qty),
            ::toString(order.price), ::toString(exec_type), ::toString(order.status), order.order_id,
            ::toString(last_qty), ::toString(order.executed_qty), ::toString(last_price), event_time,
            exec_type == ExecType::Trade ? static_cast<int64_t>(trade_id) : -1, is_maker,
            ::toString(order.cum_quote_qty), ::toString(order.orig_quote_qty), ::toString(order.stp));
    }
};

struct TradeEvent {
    Symbol symbol;
    TradeId trade_id;
    Decimal price;
    Decimal qty;
    OrderId buyer_order_id;
    OrderId seller_order_id;
    bool is_buyer_maker;
    Millis time;

    string toString() const {
        return format(R"({{"e":"trade","E":{},"s":"{}","t":{},"p":"{}","q":"{}","T":{},"m":{},"M":true}})", time,
                      symbol.view(), trade_id, ::toString(price), ::toString(qty), time, is_buyer_maker);
    }
};

struct DepthUpdate {
    Symbol symbol;
    UpdateId update_id;
    Side side;
    Decimal price;
    Decimal qty;

    string toString() const {
        const string level = format(R"([["{}","{}"]])", ::toString(price), ::toString(qty));
        return format(R"({{"e":"depthUpdate","s":"{}","U":{},"u":{},"b":{},"a":{}}})", symbol.view(), update_id,
                      update_id, side == Side::Buy ? level : "[]", side == Side::Sell ? level : "[]");
    }
};

struct NullListener {
    void onExecutionReport(const ExecutionReport &) noexcept {}
    void onTrade(const TradeEvent &) noexcept {}
    void onDepthUpdate(const DepthUpdate &) noexcept {}
};

struct OrderResponse {
    int code = 0;
    string_view msg;
    Order order;

    bool ok() const noexcept { return code == 0; }

    string toString() const {
        if (!ok()) return format(R"({{"code":{},"msg":"{}"}})", code, msg);
        return order.toString();
    }
};

inline Millis systemMillis() noexcept {
    return static_cast<Millis>(
        chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch()).count());
}

class OrderBook final {
public:
    using Clock = Millis (*)();
    static constexpr UserId kAnyUser = ~UserId{0};

    OrderBook(const SymbolInfo &info, size_t max_orders, size_t max_price_levels, Clock clock = &systemMillis)
        : info_(info), clock_(clock), order_pool_(max_orders), level_pool_(max_price_levels),
          by_id_(max_orders), by_client_id_(max_orders), open_per_user_(max_orders),
          price_places_(decimalsOf(info.tick_size)), qty_places_(decimalsOf(info.step_size)) {
        bids_.reserve(max_price_levels);
        asks_.reserve(max_price_levels);
    }

    OrderResponse add(const Order &request) noexcept {
        NullListener none;
        return add(request, none);
    }

    template <typename Listener>
    OrderResponse add(const Order &request, Listener &listener) noexcept {
        Order o = request;
        o.executed_qty = 0;
        o.cum_quote_qty = 0;
        o.status = OrderStatus::New;
        if (o.stp == StpMode::Default) o.stp = info_.default_stp;

        if (auto err = validate(o)) return *err;

        const bool may_rest = o.type == OrderType::LimitMaker ||
                              (o.type == OrderType::Limit && o.time_in_force == TimeInForce::GTC);

        if (o.type == OrderType::LimitMaker && wouldTake(o))
            return reject(-2010, "Order would immediately match and take.", o);

        if (may_rest) {
            const uint32_t *open = open_per_user_.find(o.user_id);
            if (info_.max_num_orders && open && *open >= info_.max_num_orders)
                return reject(-1013, "Filter failure: MAX_NUM_ORDERS", o);
            if (order_pool_.available() == 0 || (level_pool_.available() == 0 && !levelExists(o.side, o.price)))
                return reject(-1008, "Server is currently overloaded with other requests. Please try again in a few minutes.", o);
        }

        if (!o.client_order_id.empty() && findByClientId(o.user_id, o.client_order_id.view()) != nullptr)
            return reject(-2010, "Duplicate order sent.", o);
        o.order_id = next_order_id_++;
        if (o.client_order_id.empty()) o.client_order_id = generateClientOrderId(o.order_id);

        o.transact_time = o.working_time = clock_();
        report(listener, ExecType::New, o);

        if (o.type == OrderType::Limit && o.time_in_force == TimeInForce::FOK && !canFillCompletely(o)) {
            o.status = OrderStatus::Expired;
            report(listener, ExecType::Expired, o);
            return {0, {}, o};
        }

        const bool stopped_early = match(o, listener);

        if (o.status == OrderStatus::Filled || o.status == OrderStatus::ExpiredInMatch) return {0, {}, o};

        if (o.byQuote()) {
            o.status = (stopped_early && o.executed_qty > 0) ? OrderStatus::Filled : OrderStatus::Expired;
            if (o.status == OrderStatus::Expired) report(listener, ExecType::Expired, o);
            return {0, {}, o};
        }

        if (may_rest) {
            rest(o, listener);
            return {0, {}, o};
        }

        o.status = OrderStatus::Expired;
        report(listener, ExecType::Expired, o);
        return {0, {}, o};
    }

    OrderResponse cancel(OrderId order_id, UserId user = kAnyUser) noexcept {
        NullListener none;
        return cancel(order_id, none, user);
    }

    template <typename Listener>
    OrderResponse cancel(OrderId order_id, Listener &listener, UserId user = kAnyUser) noexcept {
        Node **slot = by_id_.find(order_id);
        if (slot == nullptr || (user != kAnyUser && (*slot)->order.user_id != user))
            return reject(-2011, "Unknown order sent.", Order{});
        return cancelNode(*slot, listener);
    }

    template <typename Listener>
    OrderResponse cancelByClientId(UserId user, string_view client_order_id, Listener &listener) noexcept {
        Node *node = findByClientId(user, client_order_id);
        if (node == nullptr) return reject(-2011, "Unknown order sent.", Order{});
        return cancelNode(node, listener);
    }

    optional<Decimal> bestBid() const noexcept {
        return bids_.empty() ? nullopt : optional<Decimal>(bids_.back()->price);
    }

    optional<Decimal> bestAsk() const noexcept {
        return asks_.empty() ? nullopt : optional<Decimal>(asks_.back()->price);
    }

    Decimal volumeAt(Side side, Decimal price) const noexcept {
        const PriceLevel *l = levelAt(side, price);
        return l ? l->total_qty : 0;
    }

    const Order *find(OrderId order_id) const noexcept {
        Node *const *slot = by_id_.find(order_id);
        return slot ? &(*slot)->order : nullptr;
    }

    const Order *findByClientOrderId(UserId user, string_view client_order_id) const noexcept {
        const Node *node = findByClientId(user, client_order_id);
        return node ? &node->order : nullptr;
    }

    vector<OrderId> queueAt(Side side, Decimal price) const {
        vector<OrderId> ids;
        for (const PriceLevel *l : levelsFor(side))
            if (l->price == price)
                for (const Node *n = l->head; n; n = n->next) ids.push_back(n->order.order_id);
        return ids;
    }

    uint32_t openOrders(UserId user) const noexcept {
        const uint32_t *n = open_per_user_.find(user);
        return n ? *n : 0;
    }

    size_t orderCount() const noexcept { return order_pool_.size(); }
    size_t levelCount(Side side) const noexcept { return levelsFor(side).size(); }
    Decimal lastPrice() const noexcept { return last_price_; }
    UpdateId lastUpdateId() const noexcept { return update_id_; }
    const SymbolInfo &info() const noexcept { return info_; }

    string depthJson(size_t limit = 100) const {
        auto side = [&](const vector<PriceLevel *> &levels) {
            string out = "[";
            size_t n = 0;
            for (auto it = levels.rbegin(); it != levels.rend() && n < limit; ++it, ++n) {
                if (n) out += ',';
                out += format(R"(["{}","{}"])", ::toString((*it)->price), ::toString((*it)->total_qty));
            }
            return out + "]";
        };
        return format(R"({{"lastUpdateId":{},"bids":{},"asks":{}}})", update_id_, side(bids_), side(asks_));
    }

    string toString(size_t depth = 10) const {
        const string price_hdr = format("Price({})", info_.quote_asset.view());
        const string qty_hdr = format("Amount({})", info_.base_asset.view());
        const string total_hdr = format("Total({})", info_.quote_asset.view());
        string out = format("{} order book  |  lastUpdateId {}  |  {} open orders\n", info_.symbol.view(),
                            update_id_, orderCount());
        out += format("{:<16}{:>18}{:>20}\n", price_hdr, qty_hdr, total_hdr);

        auto row = [&](const PriceLevel *l, const char *tag) {
            out += format("{:<16}{:>18}{:>20}  {}\n", ::toString(l->price, price_places_),
                          ::toString(l->total_qty, qty_places_),
                          ::toString(notional(l->price, l->total_qty), price_places_), tag);
        };

        const size_t shown_asks = min(depth, asks_.size());
        for (size_t i = asks_.size() - shown_asks; i < asks_.size(); ++i) row(asks_[i], "ask");

        string mid = last_price_ ? ::toString(last_price_, price_places_) : string("-");
        string spread = (!bids_.empty() && !asks_.empty())
                            ? ::toString(asks_.back()->price - bids_.back()->price, price_places_)
                            : string("-");
        out += format("{:<16}  last price      spread {}\n", mid, spread);

        size_t shown = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && shown < depth; ++it, ++shown) row(*it, "bid");
        return out;
    }

    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;

private:
    struct PriceLevel;

    struct Node {
        Order order;
        Node *prev = nullptr;
        Node *next = nullptr;
        PriceLevel *level = nullptr;
        uint64_t client_key = 0;
    };

    struct PriceLevel {
        Decimal price;
        Side side;
        Decimal total_qty = 0;
        Node *head = nullptr;
        Node *tail = nullptr;
    };

    static OrderResponse reject(int code, string_view msg, const Order &o) noexcept {
        OrderResponse r{code, msg, o};
        r.order.status = OrderStatus::Rejected;
        return r;
    }

    static bool crosses(Side taker, Decimal limit, Decimal level_price) noexcept {
        return taker == Side::Buy ? level_price <= limit : level_price >= limit;
    }

    static bool selfTrade(const Order &taker, const Order &maker) noexcept {
        return taker.stp != StpMode::None && taker.user_id == maker.user_id;
    }

    static bool validClientOrderId(string_view s) noexcept {
        for (char c : s) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                            c == '.' || c == ':' || c == '/' || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    static uint64_t clientKey(UserId user, string_view id) noexcept {
        uint64_t h = 1469598103934665603ULL ^ user;
        for (char c : id) {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ULL;
        }
        return h == IdMap<Node *>::kEmpty ? h - 1 : h;
    }

    static ClientOrderId generateClientOrderId(OrderId id) noexcept {
        static constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        char buf[22];
        uint64_t x = id * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
        for (char &c : buf) {
            x ^= x >> 29;
            x *= 0xbf58476d1ce4e5b9ULL;
            c = kAlphabet[x % 62];
        }
        return ClientOrderId(string_view(buf, sizeof(buf)));
    }

    const vector<PriceLevel *> &levelsFor(Side side) const noexcept { return side == Side::Buy ? bids_ : asks_; }
    vector<PriceLevel *> &levelsFor(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }

    size_t insertPos(Side side, Decimal price) const noexcept {
        const auto &levels = levelsFor(side);
        size_t lo = 0, hi = levels.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            const Decimal p = levels[mid]->price;
            const bool better = side == Side::Buy ? p > price : p < price;
            if (better) hi = mid;
            else lo = mid + 1;
        }
        return lo;
    }

    const PriceLevel *levelAt(Side side, Decimal price) const noexcept {
        const size_t pos = insertPos(side, price);
        const auto &levels = levelsFor(side);
        return (pos > 0 && levels[pos - 1]->price == price) ? levels[pos - 1] : nullptr;
    }

    bool levelExists(Side side, Decimal price) const noexcept { return levelAt(side, price) != nullptr; }

    Node *findByClientId(UserId user, string_view client_order_id) const noexcept {
        Node *const *slot = by_client_id_.find(clientKey(user, client_order_id));
        if (slot == nullptr) return nullptr;
        const Order &o = (*slot)->order;
        return (o.user_id == user && o.client_order_id.view() == client_order_id) ? *slot : nullptr;
    }

    optional<OrderResponse> validate(const Order &o) const noexcept {
        if (o.symbol != info_.symbol) return reject(-1121, "Invalid symbol.", o);
        if (!validClientOrderId(o.client_order_id.view()))
            return reject(-1100,
                          "Illegal characters found in parameter 'newClientOrderId'; legal range is "
                          "'^[\\.A-Z\\:/a-z0-9_-]{1,36}$'.",
                          o);

        if (o.type == OrderType::Market) {
            if (o.orig_qty <= 0 && o.orig_quote_qty <= 0)
                return reject(-1102, "Param 'quantity' or 'quoteOrderQty' must be sent, but both were empty/null!", o);
            if (o.orig_qty > 0 && o.orig_quote_qty > 0)
                return reject(-1106, "Parameter 'quoteOrderQty' sent when not required.", o);
            if (o.orig_qty > 0) {
                if (!lotOk(o.orig_qty, info_.min_qty, info_.max_qty, info_.step_size))
                    return reject(-1013, "Filter failure: LOT_SIZE", o);
                if (!lotOk(o.orig_qty, info_.market_min_qty, info_.market_max_qty, info_.market_step_size))
                    return reject(-1013, "Filter failure: MARKET_LOT_SIZE", o);
                if (last_price_ && !notionalOk(notional(last_price_, o.orig_qty), info_.apply_min_to_market,
                                               info_.apply_max_to_market))
                    return reject(-1013, "Filter failure: NOTIONAL", o);
            } else if (!notionalOk(o.orig_quote_qty, info_.apply_min_to_market, info_.apply_max_to_market)) {
                return reject(-1013, "Filter failure: NOTIONAL", o);
            }
            return nullopt;
        }

        if (o.price <= 0)
            return reject(-1102, "Mandatory parameter 'price' was not sent, was empty/null, or malformed.", o);
        if (o.orig_qty <= 0)
            return reject(-1102, "Mandatory parameter 'quantity' was not sent, was empty/null, or malformed.", o);
        if ((info_.min_price && o.price < info_.min_price) || (info_.max_price && o.price > info_.max_price) ||
            (info_.tick_size && o.price % info_.tick_size != 0))
            return reject(-1013, "Filter failure: PRICE_FILTER", o);
        if (!lotOk(o.orig_qty, info_.min_qty, info_.max_qty, info_.step_size))
            return reject(-1013, "Filter failure: LOT_SIZE", o);
        if (!notionalOk(notional(o.price, o.orig_qty), true, true))
            return reject(-1013, "Filter failure: NOTIONAL", o);
        return nullopt;
    }

    static bool lotOk(Decimal qty, Decimal min_q, Decimal max_q, Decimal step) noexcept {
        return !((min_q && qty < min_q) || (max_q && qty > max_q) || (step && qty % step != 0));
    }

    bool notionalOk(Decimal value, bool check_min, bool check_max) const noexcept {
        if (check_min && info_.min_notional && value < info_.min_notional) return false;
        if (check_max && info_.max_notional && value > info_.max_notional) return false;
        return true;
    }

    bool wouldTake(const Order &o) const noexcept {
        const auto &opposite = levelsFor(o.side == Side::Buy ? Side::Sell : Side::Buy);
        return !opposite.empty() && crosses(o.side, o.price, opposite.back()->price);
    }

    bool canFillCompletely(const Order &o) const noexcept {
        const auto &opposite = levelsFor(o.side == Side::Buy ? Side::Sell : Side::Buy);
        Decimal available = 0;
        for (auto it = opposite.rbegin(); it != opposite.rend(); ++it) {
            if (!crosses(o.side, o.price, (*it)->price)) break;
            for (const Node *n = (*it)->head; n; n = n->next) {
                if (selfTrade(o, n->order)) {
                    if (o.stp == StpMode::ExpireMaker) continue;
                    return false;
                }
                available += n->order.leavesQty();
                if (available >= o.orig_qty) return true;
            }
        }
        return false;
    }

    Decimal fillQty(const Order &taker, const Order &maker, Decimal price) const noexcept {
        if (!taker.byQuote()) return min(taker.leavesQty(), maker.leavesQty());
        Decimal q = qtyForQuote(taker.orig_quote_qty - taker.cum_quote_qty, price);
        const Decimal step = info_.market_step_size ? info_.market_step_size : info_.step_size;
        if (step) q -= q % step;
        return min(q, maker.leavesQty());
    }

    template <typename Listener>
    bool match(Order &taker, Listener &listener) noexcept {
        auto &opposite = levelsFor(taker.side == Side::Buy ? Side::Sell : Side::Buy);

        while (!opposite.empty()) {
            PriceLevel *level = opposite.back();
            if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level->price)) return false;

            bool touched = false;
            bool done = false;
            for (Node *n = level->head; n != nullptr;) {
                Node *next = n->next;
                if (selfTrade(taker, n->order)) {
                    if (taker.stp == StpMode::ExpireMaker || taker.stp == StpMode::ExpireBoth) {
                        n->order.status = OrderStatus::ExpiredInMatch;
                        report(listener, ExecType::TradePrevention, n->order);
                        level->total_qty -= n->order.leavesQty();
                        removeNode(n);
                        touched = true;
                    }
                    if (taker.stp == StpMode::ExpireTaker || taker.stp == StpMode::ExpireBoth) {
                        taker.status = OrderStatus::ExpiredInMatch;
                        report(listener, ExecType::TradePrevention, taker);
                        done = true;
                        break;
                    }
                    n = next;
                    continue;
                }

                const Decimal qty = fillQty(taker, n->order, level->price);
                if (qty <= 0) {
                    done = true;
                    break;
                }
                execute(taker, n, qty, listener);
                touched = true;
                if (!taker.byQuote() && taker.leavesQty() == 0) {
                    done = true;
                    break;
                }
                n = next;
            }

            if (touched) publishLevel(level, listener);
            if (level->head == nullptr) {
                opposite.pop_back();
                level_pool_.deallocate(level);
            }
            if (done) return true;
        }
        return false;
    }

    template <typename Listener>
    void execute(Order &taker, Node *maker_node, Decimal qty, Listener &listener) noexcept {
        Order &maker = maker_node->order;
        PriceLevel *level = maker_node->level;
        const Decimal price = level->price;
        const Decimal quote = notional(price, qty);
        const TradeId trade_id = next_trade_id_++;
        const Millis now = clock_();

        maker.executed_qty += qty;
        maker.cum_quote_qty += quote;
        maker.status = maker.leavesQty() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        maker.working_time = now;

        taker.executed_qty += qty;
        taker.cum_quote_qty += quote;
        if (taker.byQuote()) taker.orig_qty = taker.executed_qty;
        taker.status = (!taker.byQuote() && taker.leavesQty() == 0) ? OrderStatus::Filled
                                                                     : OrderStatus::PartiallyFilled;
        level->total_qty -= qty;
        last_price_ = price;

        const bool buyer_is_maker = maker.side == Side::Buy;
        listener.onTrade(TradeEvent{info_.symbol, trade_id, price, qty,
                                    buyer_is_maker ? maker.order_id : taker.order_id,
                                    buyer_is_maker ? taker.order_id : maker.order_id, buyer_is_maker, now});
        listener.onExecutionReport(ExecutionReport{ExecType::Trade, taker, qty, price, trade_id, false, now});
        listener.onExecutionReport(ExecutionReport{ExecType::Trade, maker, qty, price, trade_id, true, now});

        if (maker.leavesQty() == 0) removeNode(maker_node);
    }

    template <typename Listener>
    void rest(Order &o, Listener &listener) noexcept {
        auto &levels = levelsFor(o.side);
        const size_t pos = insertPos(o.side, o.price);
        PriceLevel *level = (pos > 0 && levels[pos - 1]->price == o.price) ? levels[pos - 1] : nullptr;
        if (level == nullptr) {
            level = level_pool_.allocate(PriceLevel{o.price, o.side});
            levels.insert(levels.begin() + static_cast<ptrdiff_t>(pos), level);
        }

        Node *node = order_pool_.allocate();
        node->order = o;
        node->level = level;
        node->prev = level->tail;
        node->next = nullptr;
        if (level->tail) level->tail->next = node;
        else level->head = node;
        level->tail = node;
        level->total_qty += o.leavesQty();

        node->client_key = clientKey(o.user_id, o.client_order_id.view());
        by_id_.insert(o.order_id, node);
        by_client_id_.insert(node->client_key, node);
        if (uint32_t *count = open_per_user_.find(o.user_id)) ++*count;
        else open_per_user_.insert(o.user_id, 1);

        publishLevel(level, listener);
    }

    template <typename Listener>
    OrderResponse cancelNode(Node *node, Listener &listener) noexcept {
        PriceLevel *level = node->level;
        node->order.status = OrderStatus::Canceled;
        node->order.working_time = clock_();
        const Order snapshot = node->order;
        report(listener, ExecType::Canceled, snapshot);

        level->total_qty -= snapshot.leavesQty();
        removeNode(node);
        publishLevel(level, listener);
        if (level->head == nullptr) removeLevel(level);
        return {0, {}, snapshot};
    }

    void removeNode(Node *node) noexcept {
        PriceLevel *level = node->level;
        if (node->prev) node->prev->next = node->next;
        else level->head = node->next;
        if (node->next) node->next->prev = node->prev;
        else level->tail = node->prev;

        by_id_.erase(node->order.order_id);
        by_client_id_.erase(node->client_key);
        if (uint32_t *count = open_per_user_.find(node->order.user_id); count && --*count == 0)
            open_per_user_.erase(node->order.user_id);
        order_pool_.deallocate(node);
    }

    void removeLevel(PriceLevel *level) noexcept {
        auto &levels = levelsFor(level->side);
        const size_t pos = insertPos(level->side, level->price);
        assert(pos > 0 && levels[pos - 1] == level);
        levels.erase(levels.begin() + static_cast<ptrdiff_t>(pos - 1));
        level_pool_.deallocate(level);
    }

    template <typename Listener>
    void publishLevel(const PriceLevel *level, Listener &listener) noexcept {
        listener.onDepthUpdate(DepthUpdate{info_.symbol, ++update_id_, level->side, level->price, level->total_qty});
    }

    template <typename Listener>
    void report(Listener &listener, ExecType type, const Order &o) noexcept {
        listener.onExecutionReport(ExecutionReport{type, o, 0, 0, 0, false, clock_()});
    }

    SymbolInfo info_;
    Clock clock_;
    MemPool<Node> order_pool_;
    MemPool<PriceLevel> level_pool_;
    vector<PriceLevel *> bids_;
    vector<PriceLevel *> asks_;
    IdMap<Node *> by_id_;
    IdMap<Node *> by_client_id_;
    IdMap<uint32_t> open_per_user_;
    int price_places_;
    int qty_places_;
    OrderId next_order_id_ = 1;
    TradeId next_trade_id_ = 1;
    UpdateId update_id_ = 0;
    Decimal last_price_ = 0;
};
