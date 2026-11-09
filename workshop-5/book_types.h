#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "order.h"

using namespace std;

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

enum class RejectReason : uint8_t { None, WrongTicker, ZeroQuantity, InvalidPrice, InvalidOrderId, DuplicateOrderId, AlreadyExpired, OutOfRange };

string_view toString(RejectReason r) noexcept;

struct AddResult {
    RejectReason reject = RejectReason::None;
    Qty filled = 0;
    Qty rested = 0;
    Qty cancelled = 0;

    bool accepted() const noexcept { return reject == RejectReason::None; }
};

struct LevelInfo {
    Price price;
    Qty total;
    uint32_t orders;
};
