#include "book_types.h"

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
        case RejectReason::OutOfRange: return "OutOfRange";
    }
    return "?";
}

