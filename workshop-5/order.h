#pragma once

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

using namespace std;

using OrderId  = uint64_t;
using ClientId = uint32_t;
using Price    = int64_t;
using Qty      = uint32_t;
using Millis   = uint64_t;

inline constexpr Price kPriceScale = 100;

constexpr Price price(int64_t dollars, int64_t cents) noexcept { return dollars * kPriceScale + cents; }

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market };
enum class OrderStatus : uint8_t { New, PartiallyFilled, Filled, Canceled, Expired, Rejected };

constexpr string_view toString(Side s) noexcept { return s == Side::Buy ? "BUY" : "SELL"; }
constexpr string_view toString(OrderType t) noexcept { return t == OrderType::Limit ? "LIMIT" : "MARKET"; }

constexpr string_view toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Canceled: return "CANCELED";
        case OrderStatus::Expired: return "EXPIRED";
        case OrderStatus::Rejected: return "REJECTED";
    }
    return "?";
}

inline string priceToString(Price p) {
    const Price a = p < 0 ? -p : p;
    return format("{}{}.{:02}", p < 0 ? "-" : "", a / kPriceScale, a % kPriceScale);
}

class Ticker {
public:
    static constexpr size_t kMaxLen = 8;

    constexpr Ticker() noexcept = default;
    constexpr Ticker(string_view s) noexcept {
        for (size_t i = 0; i < s.size() && i < kMaxLen; ++i) chars_[i] = s[i];
    }
    constexpr Ticker(const char *s) noexcept : Ticker(string_view(s)) {}

    constexpr string_view view() const noexcept {
        size_t n = 0;
        while (n < kMaxLen && chars_[n] != '\0') ++n;
        return {chars_.data(), n};
    }
    friend constexpr bool operator==(const Ticker &, const Ticker &) noexcept = default;

private:
    array<char, kMaxLen> chars_{};
};

struct Order {
    Ticker ticker;
    OrderId order_id = 0;
    Price price = 0;
    Millis expire_at = 0;
    Millis entry_time = 0;
    ClientId client_id = 0;
    Qty qty = 0;
    Qty filled = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    OrderStatus status = OrderStatus::New;

    static Order limit(Ticker t, OrderId id, ClientId c, Side s, Price p, Qty q, Millis expire_at = 0) noexcept {
        Order o;
        o.ticker = t;
        o.order_id = id;
        o.client_id = c;
        o.side = s;
        o.type = OrderType::Limit;
        o.price = p;
        o.qty = q;
        o.expire_at = expire_at;
        return o;
    }

    static Order market(Ticker t, OrderId id, ClientId c, Side s, Qty q) noexcept {
        Order o = limit(t, id, c, s, 0, q);
        o.type = OrderType::Market;
        return o;
    }

    Qty leaves() const noexcept { return qty - filled; }
    bool hasTtl() const noexcept { return expire_at != 0; }

    string toString() const {
        const string px = type == OrderType::Market ? string("MKT") : priceToString(price);
        string s = format("Order[id:{} client:{} {} {} {} qty:{} filled:{} @ {} {}", order_id, client_id, ticker.view(),
                          ::toString(type), ::toString(side), qty, filled, px, ::toString(status));
        if (hasTtl()) s += format(" ttl:{}", expire_at);
        return s + "]";
    }
};

static_assert(is_trivially_copyable_v<Order>);
