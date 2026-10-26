#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

using namespace std;

using Decimal  = int64_t;
using OrderId  = uint64_t;
using TradeId  = uint64_t;
using UserId   = uint64_t;
using UpdateId = uint64_t;
using Millis   = uint64_t;

__extension__ typedef __int128 Int128;

inline constexpr int kDecimals = 8;
inline constexpr Decimal kOne = 100'000'000;

constexpr Decimal dec(string_view s) noexcept {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    Decimal whole = 0, frac = 0;
    int digits = 0;
    for (; i < s.size() && s[i] != '.'; ++i) whole = whole * 10 + (s[i] - '0');
    if (i < s.size()) ++i;
    for (; i < s.size() && digits < kDecimals; ++i, ++digits) frac = frac * 10 + (s[i] - '0');
    for (; digits < kDecimals; ++digits) frac *= 10;
    const Decimal v = whole * kOne + frac;
    return neg ? -v : v;
}

constexpr Decimal operator""_d(const char *s, size_t n) noexcept { return dec({s, n}); }

constexpr Decimal notional(Decimal price, Decimal qty) noexcept {
    return static_cast<Decimal>(static_cast<Int128>(price) * qty / kOne);
}

constexpr Decimal qtyForQuote(Decimal quote, Decimal price) noexcept {
    return static_cast<Decimal>(static_cast<Int128>(quote) * kOne / price);
}

constexpr int decimalsOf(Decimal step) noexcept {
    if (step <= 0) return kDecimals;
    int places = kDecimals;
    while (places > 0 && step % 10 == 0) {
        step /= 10;
        --places;
    }
    return places;
}

inline string toString(Decimal v, int places = kDecimals) {
    const Decimal a = v < 0 ? -v : v;
    string s = format("{}{}", v < 0 ? "-" : "", a / kOne);
    if (places > 0) {
        Decimal frac = a % kOne;
        for (int i = places; i < kDecimals; ++i) frac /= 10;
        s += format(".{:0{}}", frac, places);
    }
    return s;
}

template <size_t N>
class FixedString {
public:
    constexpr FixedString() noexcept = default;
    constexpr FixedString(string_view s) noexcept {
        for (size_t i = 0; i < s.size() && i < N; ++i) chars_[i] = s[i];
    }
    constexpr FixedString(const char *s) noexcept : FixedString(string_view(s)) {}

    constexpr string_view view() const noexcept {
        size_t n = 0;
        while (n < N && chars_[n] != '\0') ++n;
        return {chars_.data(), n};
    }
    constexpr bool empty() const noexcept { return chars_[0] == '\0'; }
    friend constexpr bool operator==(const FixedString &, const FixedString &) noexcept = default;

private:
    array<char, N> chars_{};
};

using Symbol = FixedString<20>;
using Asset = FixedString<10>;
using ClientOrderId = FixedString<36>;

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market, LimitMaker };
enum class TimeInForce : uint8_t { GTC, IOC, FOK };
enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Canceled, Rejected, Expired, ExpiredInMatch };
enum class StpMode : uint8_t { Default, None, ExpireTaker, ExpireMaker, ExpireBoth };

constexpr string_view toString(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }

constexpr string_view toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
        case OrderType::LimitMaker: return "LIMIT_MAKER";
    }
    return "?";
}

constexpr string_view toString(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
    }
    return "?";
}

constexpr string_view toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Canceled: return "CANCELED";
        case OrderStatus::Rejected: return "REJECTED";
        case OrderStatus::Expired: return "EXPIRED";
        case OrderStatus::ExpiredInMatch: return "EXPIRED_IN_MATCH";
    }
    return "?";
}

constexpr string_view toString(StpMode m) noexcept {
    switch (m) {
        case StpMode::Default: return "DEFAULT";
        case StpMode::None: return "NONE";
        case StpMode::ExpireTaker: return "EXPIRE_TAKER";
        case StpMode::ExpireMaker: return "EXPIRE_MAKER";
        case StpMode::ExpireBoth: return "EXPIRE_BOTH";
    }
    return "?";
}

struct Order {
    Symbol symbol;
    ClientOrderId client_order_id;
    OrderId order_id = 0;
    UserId user_id = 0;
    Decimal price = 0;
    Decimal orig_qty = 0;
    Decimal orig_quote_qty = 0;
    Decimal executed_qty = 0;
    Decimal cum_quote_qty = 0;
    Millis transact_time = 0;
    Millis working_time = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce time_in_force = TimeInForce::GTC;
    OrderStatus status = OrderStatus::New;
    StpMode stp = StpMode::Default;

    static Order limit(Symbol symbol, UserId user, Side side, Decimal price, Decimal qty,
                       TimeInForce tif = TimeInForce::GTC) noexcept {
        Order o;
        o.symbol = symbol;
        o.user_id = user;
        o.side = side;
        o.type = OrderType::Limit;
        o.time_in_force = tif;
        o.price = price;
        o.orig_qty = qty;
        return o;
    }

    static Order limitMaker(Symbol symbol, UserId user, Side side, Decimal price, Decimal qty) noexcept {
        Order o = limit(symbol, user, side, price, qty);
        o.type = OrderType::LimitMaker;
        return o;
    }

    static Order market(Symbol symbol, UserId user, Side side, Decimal qty) noexcept {
        Order o;
        o.symbol = symbol;
        o.user_id = user;
        o.side = side;
        o.type = OrderType::Market;
        o.orig_qty = qty;
        return o;
    }

    static Order marketByQuote(Symbol symbol, UserId user, Side side, Decimal quote_qty) noexcept {
        Order o = market(symbol, user, side, 0);
        o.orig_quote_qty = quote_qty;
        return o;
    }

    Order &withClientOrderId(string_view id) noexcept {
        client_order_id = id;
        return *this;
    }

    Order &withStp(StpMode mode) noexcept {
        stp = mode;
        return *this;
    }

    bool byQuote() const noexcept { return type == OrderType::Market && orig_quote_qty > 0; }
    Decimal leavesQty() const noexcept { return orig_qty - executed_qty; }
    bool isOpen() const noexcept { return status == OrderStatus::New || status == OrderStatus::PartiallyFilled; }

    string toString() const {
        return format(
            R"({{"symbol":"{}","orderId":{},"orderListId":-1,"clientOrderId":"{}","transactTime":{},)"
            R"("price":"{}","origQty":"{}","executedQty":"{}","origQuoteOrderQty":"{}","cummulativeQuoteQty":"{}",)"
            R"("status":"{}","timeInForce":"{}","type":"{}","side":"{}","workingTime":{},"selfTradePreventionMode":"{}"}})",
            symbol.view(), order_id, client_order_id.view(), transact_time, ::toString(price),
            ::toString(orig_qty), ::toString(executed_qty), ::toString(orig_quote_qty), ::toString(cum_quote_qty),
            ::toString(status), ::toString(time_in_force), ::toString(type), ::toString(side), working_time,
            ::toString(stp));
    }
};

static_assert(is_trivially_copyable_v<Order>);
static_assert(is_trivially_destructible_v<Order>);
