// order_book_test.cpp — GoogleTest suite for Workshop 5.
// The same behaviour tests run against all three order books (typed tests),
// followed by tests specific to the tick-indexed array.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>

#include "heap_order_book.h"
#include "map_order_book.h"
#include "tick_order_book.h"

// Counts heap allocations so a test can prove the tick book's add/cancel never allocate.
static std::atomic<std::size_t> g_allocs{0};
void *operator new(std::size_t n) {
    ++g_allocs;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    ++g_allocs;
    return std::malloc(n ? n : 1);
}
void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }

namespace {

const Ticker AAPL = "AAPL";

struct Recorder : OrderBookListener {
    std::vector<Trade> trades;
    int cancels = 0;
    void onTrade(const Trade &t) override { trades.push_back(t); }
    void onOrderEvent(const Order &, OrderEvent e) override { cancels += e == OrderEvent::Canceled; }
};

// How each implementation is built for the shared tests: prices $0.01 .. $1000.00, tick $0.01.
template <typename Book> std::unique_ptr<Book> makeBook(std::size_t max_orders = 50'000);
template <> std::unique_ptr<TickOrderBook> makeBook(std::size_t max_orders) {
    return std::make_unique<TickOrderBook>(AAPL, price(0, 1), price(1000, 0), 1, max_orders);
}
template <> std::unique_ptr<HeapOrderBook> makeBook(std::size_t max_orders) {
    return std::make_unique<HeapOrderBook>(AAPL, max_orders, 100'000);
}
template <> std::unique_ptr<MapOrderBook> makeBook(std::size_t max_orders) {
    return std::make_unique<MapOrderBook>(AAPL, max_orders);
}

template <typename Book>
class AnyOrderBookTest : public ::testing::Test {
protected:
    AnyOrderBookTest() : book(makeBook<Book>()) { book->setListener(&rec); }

    AddResult buy(OrderId id, Price p, Qty q, ClientId c = 1) {
        return book->add(Order::limit(AAPL, id, c, Side::Buy, p, q));
    }
    AddResult sell(OrderId id, Price p, Qty q, ClientId c = 2) {
        return book->add(Order::limit(AAPL, id, c, Side::Sell, p, q));
    }

    std::unique_ptr<Book> book;
    Recorder rec;
};

using Books = ::testing::Types<TickOrderBook, HeapOrderBook, MapOrderBook>;
TYPED_TEST_SUITE(AnyOrderBookTest, Books);

// ---------------------------------------------------------------------------
// Shared behaviour: every implementation must pass these
// ---------------------------------------------------------------------------
TYPED_TEST(AnyOrderBookTest, EmptyBook) {
    EXPECT_FALSE(this->book->bestBid().has_value());
    EXPECT_FALSE(this->book->bestAsk().has_value());
    EXPECT_EQ(this->book->orderCount(), 0u);
    EXPECT_EQ(this->book->volumeAt(Side::Buy, price(100, 0)), 0u);
    EXPECT_EQ(this->book->find(1), nullptr);
}

TYPED_TEST(AnyOrderBookTest, BestBidAndAsk) {
    this->buy(1, price(99, 98), 4);
    this->buy(2, price(100, 1), 2);
    this->buy(3, price(100, 0), 12);
    this->sell(4, price(100, 5), 6);
    this->sell(5, price(100, 2), 3);
    EXPECT_EQ(*this->book->bestBid(), price(100, 1));
    EXPECT_EQ(*this->book->bestAsk(), price(100, 2));
    EXPECT_EQ(this->book->levelCount(Side::Buy), 3u);
    EXPECT_EQ(this->book->levelCount(Side::Sell), 2u);
}

TYPED_TEST(AnyOrderBookTest, SamePriceKeepsTimePriority) {
    this->buy(1, price(100, 0), 5);
    this->buy(2, price(100, 0), 7);
    this->buy(3, price(100, 0), 1);
    EXPECT_EQ(this->book->queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(this->book->volumeAt(Side::Buy, price(100, 0)), 13u);
}

TYPED_TEST(AnyOrderBookTest, Workshop3SlideTransitions) {
    this->sell(1, price(100, 1), 8);
    this->buy(2, price(100, 0), 5);
    this->buy(3, price(100, 0), 7);
    this->sell(4, price(100, 2), 3);
    AddResult r = this->buy(5, price(100, 1), 10);
    EXPECT_EQ(r.filled, 8u);
    EXPECT_EQ(r.rested, 2u);
    this->rec.trades.clear();
    r = this->book->add(Order::market(AAPL, 6, 9, Side::Sell, 10));
    EXPECT_EQ(r.filled, 10u);
    ASSERT_EQ(this->rec.trades.size(), 3u);
    EXPECT_EQ(this->rec.trades[0].qty, 2u);
    EXPECT_EQ(this->rec.trades[1].buy_order_id, 2u);
    EXPECT_EQ(this->rec.trades[2].buy_order_id, 3u);
    EXPECT_EQ(this->book->volumeAt(Side::Buy, price(100, 0)), 4u);
    EXPECT_EQ(*this->book->bestAsk(), price(100, 2));
}

TYPED_TEST(AnyOrderBookTest, PartialFillLeavesRemainderInPlace) {
    this->sell(1, price(50, 0), 10);
    this->buy(2, price(50, 0), 4);
    ASSERT_NE(this->book->find(1), nullptr);
    EXPECT_EQ(this->book->find(1)->leaves(), 6u);
    EXPECT_EQ(this->book->find(1)->status, OrderStatus::PartiallyFilled);
}

TYPED_TEST(AnyOrderBookTest, TradesAtRestingPrice) {
    this->sell(1, price(50, 0), 5);
    this->buy(2, price(55, 0), 5);
    ASSERT_EQ(this->rec.trades.size(), 1u);
    EXPECT_EQ(this->rec.trades[0].price, price(50, 0));
}

TYPED_TEST(AnyOrderBookTest, SweepsLevelsThenRests) {
    this->sell(1, price(10, 0), 5);
    this->sell(2, price(10, 1), 5);
    this->sell(3, price(10, 5), 5);
    AddResult r = this->buy(4, price(10, 2), 20);
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.rested, 10u);
    EXPECT_EQ(*this->book->bestBid(), price(10, 2));
    EXPECT_EQ(*this->book->bestAsk(), price(10, 5));
}

TYPED_TEST(AnyOrderBookTest, SellTakesBestBidFirst) {
    this->buy(1, price(20, 0), 3);
    this->buy(2, price(21, 0), 3);
    this->sell(3, price(19, 0), 5);
    ASSERT_EQ(this->rec.trades.size(), 2u);
    EXPECT_EQ(this->rec.trades[0].price, price(21, 0));
    EXPECT_EQ(this->rec.trades[1].price, price(20, 0));
}

TYPED_TEST(AnyOrderBookTest, MarketOrderLeftoverIsCancelled) {
    this->sell(1, price(10, 0), 3);
    AddResult r = this->book->add(Order::market(AAPL, 2, 1, Side::Buy, 10));
    EXPECT_EQ(r.filled, 3u);
    EXPECT_EQ(r.cancelled, 7u);
    r = this->book->add(Order::market(AAPL, 3, 1, Side::Sell, 4));
    EXPECT_EQ(r.cancelled, 4u);
    EXPECT_EQ(this->book->orderCount(), 0u);
}

TYPED_TEST(AnyOrderBookTest, Rejections) {
    EXPECT_EQ(this->book->add(Order::limit("MSFT", 1, 1, Side::Buy, 100, 1)).reject, RejectReason::WrongTicker);
    EXPECT_EQ(this->buy(1, 100, 0).reject, RejectReason::ZeroQuantity);
    EXPECT_EQ(this->buy(1, 0, 5).reject, RejectReason::InvalidPrice);
    EXPECT_TRUE(this->buy(1, 100, 5).accepted());
    EXPECT_EQ(this->buy(1, 100, 5).reject, RejectReason::DuplicateOrderId);
}

TYPED_TEST(AnyOrderBookTest, CancelFromMiddleOfQueue) {
    this->buy(1, price(100, 0), 5);
    this->buy(2, price(100, 0), 7);
    this->buy(3, price(100, 0), 9);
    EXPECT_TRUE(this->book->cancel(2));
    EXPECT_EQ(this->book->queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(this->book->volumeAt(Side::Buy, price(100, 0)), 14u);
    EXPECT_FALSE(this->book->cancel(2));
    EXPECT_FALSE(this->book->cancel(999));
    EXPECT_EQ(this->rec.cancels, 1);
}

TYPED_TEST(AnyOrderBookTest, CancellingBestLevelMovesBestPrice) {
    this->buy(1, price(100, 0), 1);
    this->buy(2, price(99, 50), 1);
    this->sell(3, price(101, 0), 1);
    this->sell(4, price(102, 75), 1);
    this->book->cancel(1);
    this->book->cancel(3);
    EXPECT_EQ(*this->book->bestBid(), price(99, 50));
    EXPECT_EQ(*this->book->bestAsk(), price(102, 75));
    this->book->cancel(2);
    this->book->cancel(4);
    EXPECT_FALSE(this->book->bestBid().has_value());
    EXPECT_FALSE(this->book->bestAsk().has_value());
}

TYPED_TEST(AnyOrderBookTest, DepthListsBestLevelsFirst) {
    this->buy(1, price(100, 0), 5);
    this->buy(2, price(100, 0), 2);
    this->buy(3, price(99, 0), 4);
    this->buy(4, price(98, 0), 1);
    this->sell(5, price(101, 0), 3);
    LevelInfo out[4];
    ASSERT_EQ(this->book->depth(Side::Buy, 2, out), 2u);
    EXPECT_EQ(out[0].price, price(100, 0));
    EXPECT_EQ(out[0].total, 7u);
    EXPECT_EQ(out[0].orders, 2u);
    EXPECT_EQ(out[1].price, price(99, 0));
    ASSERT_EQ(this->book->depth(Side::Sell, 4, out), 1u);
    EXPECT_EQ(out[0].price, price(101, 0));
}

TYPED_TEST(AnyOrderBookTest, ToStringShowsBothSides) {
    this->sell(1, price(100, 1), 8);
    this->buy(2, price(100, 0), 5);
    const std::string s = this->book->toString();
    EXPECT_NE(s.find("100.01"), std::string::npos);
    EXPECT_NE(s.find("spread 0.01"), std::string::npos);
    this->book->cancel(1);
    EXPECT_EQ(this->book->toString().find("spread"), std::string::npos);
}

// Every implementation must make exactly the same trades as the sorted-map book.
TYPED_TEST(AnyOrderBookTest, MatchesSortedMapBookOnRandomFlow) {
    auto reference = makeBook<MapOrderBook>();
    Recorder ref_rec;
    reference->setListener(&ref_rec);
    std::mt19937 rng(2026);
    std::uniform_int_distribution<int> px(9'900, 10'100), qty(1, 100), pct(0, 99);

    for (OrderId id = 1; id <= 30'000; ++id) {
        this->rec.trades.clear();
        ref_rec.trades.clear();
        const int roll = pct(rng);
        if (roll < 30 && id > 1) {
            OrderId victim = std::uniform_int_distribution<OrderId>(1, id - 1)(rng);
            ASSERT_EQ(this->book->cancel(victim), reference->cancel(victim)) << "step " << id;
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Order o = roll < 38 ? Order::market(AAPL, id, 1, s, static_cast<Qty>(qty(rng)))
                                : Order::limit(AAPL, id, 1, s, px(rng), static_cast<Qty>(qty(rng)));
            ASSERT_TRUE(this->book->add(o).accepted());
            reference->add(o);
            ASSERT_EQ(this->rec.trades.size(), ref_rec.trades.size()) << "step " << id;
            for (std::size_t i = 0; i < ref_rec.trades.size(); ++i) {
                ASSERT_EQ(this->rec.trades[i].price, ref_rec.trades[i].price) << "step " << id;
                ASSERT_EQ(this->rec.trades[i].qty, ref_rec.trades[i].qty) << "step " << id;
                ASSERT_EQ(this->rec.trades[i].sell_order_id, ref_rec.trades[i].sell_order_id) << "step " << id;
            }
        }
        ASSERT_EQ(this->book->bestBid(), reference->bestBid()) << "step " << id;
        ASSERT_EQ(this->book->bestAsk(), reference->bestAsk()) << "step " << id;
        ASSERT_EQ(this->book->orderCount(), reference->orderCount()) << "step " << id;
    }
}

// ---------------------------------------------------------------------------
// Tick-indexed array specifics
// ---------------------------------------------------------------------------
TEST(TickOrderBook, LayoutIsOneSlotPerTick) {
    TickOrderBook book(AAPL, price(99, 97), price(100, 5), 1, 16);  // the ladder on the slide
    EXPECT_EQ(book.tickCount(), 9u);
    EXPECT_EQ(book.minPrice(), price(99, 97));
    EXPECT_EQ(book.maxPrice(), price(100, 5));
    EXPECT_EQ(book.tickSize(), 1);
}

TEST(TickOrderBook, RejectsPricesOutsideRangeOrOffTick) {
    TickOrderBook book(AAPL, price(90, 0), price(110, 0), 5, 16);  // tick = $0.05
    EXPECT_EQ(book.add(Order::limit(AAPL, 1, 1, Side::Buy, price(89, 95), 1)).reject, RejectReason::OutOfRange);
    EXPECT_EQ(book.add(Order::limit(AAPL, 2, 1, Side::Buy, price(110, 5), 1)).reject, RejectReason::OutOfRange);
    EXPECT_EQ(book.add(Order::limit(AAPL, 3, 1, Side::Buy, price(100, 3), 1)).reject, RejectReason::OutOfRange);
    EXPECT_EQ(book.add(Order::limit(AAPL, IdMap<int>::kEmpty, 1, Side::Buy, price(100, 0), 1)).reject,
              RejectReason::InvalidOrderId);
    EXPECT_TRUE(book.add(Order::limit(AAPL, 5, 1, Side::Buy, price(100, 5), 1)).accepted());
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 3)), 0u);       // off-tick query
    EXPECT_TRUE(book.queueAt(Side::Buy, price(100, 3)).empty());  // off-tick query
    EXPECT_TRUE(book.queueAt(Side::Sell, price(100, 5)).empty()); // wrong side
}

TEST(TickOrderBook, BestIndexStepsOutwardOverEmptyTicks) {
    TickOrderBook book(AAPL, price(99, 97), price(100, 5), 1, 16);
    book.add(Order::limit(AAPL, 120, 1, Side::Buy, price(99, 98), 4));
    book.add(Order::limit(AAPL, 87, 1, Side::Buy, price(100, 0), 12));
    book.add(Order::limit(AAPL, 102, 1, Side::Buy, price(100, 1), 2));
    book.add(Order::limit(AAPL, 110, 2, Side::Sell, price(100, 2), 3));
    book.add(Order::limit(AAPL, 131, 2, Side::Sell, price(100, 5), 6));
    EXPECT_EQ(*book.bestBid(), price(100, 1));
    EXPECT_EQ(*book.bestAsk(), price(100, 2));

    book.cancel(102);                               // best bid empties → step down to 100.00
    EXPECT_EQ(*book.bestBid(), price(100, 0));
    book.cancel(110);                               // best ask empties → skip 100.03, 100.04
    EXPECT_EQ(*book.bestAsk(), price(100, 5));
    book.cancel(87);                                // skip empty 99.99
    EXPECT_EQ(*book.bestBid(), price(99, 98));
    EXPECT_EQ(book.levelCount(Side::Buy), 1u);
    EXPECT_EQ(book.levelCount(Side::Sell), 1u);
}

TEST(TickOrderBook, NonBestLevelEmptyingDoesNotMoveBest) {
    TickOrderBook book(AAPL, price(1, 0), price(2, 0), 1, 16);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, price(1, 50), 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, price(1, 40), 1));
    book.add(Order::limit(AAPL, 3, 1, Side::Sell, price(1, 60), 1));
    book.add(Order::limit(AAPL, 4, 1, Side::Sell, price(1, 70), 1));
    book.cancel(2);
    book.cancel(4);
    EXPECT_EQ(*book.bestBid(), price(1, 50));
    EXPECT_EQ(*book.bestAsk(), price(1, 60));
}

TEST(TickOrderBook, FullPoolCancelsRemainder) {
    TickOrderBook book(AAPL, 1, 1000, 1, 2);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 101, 1));
    AddResult r = book.add(Order::limit(AAPL, 3, 1, Side::Buy, 102, 5));
    EXPECT_EQ(r.cancelled, 5u);
    r = book.add(Order::limit(AAPL, 4, 2, Side::Sell, 101, 1));
    EXPECT_EQ(r.filled, 1u);
}

TEST(TickOrderBook, AddAndCancelNeverAllocate) {
    TickOrderBook book(AAPL, price(90, 0), price(110, 0), 1, 8192);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> px(9'950, 10'050), qty(1, 50), pct(0, 99);
    std::vector<OrderId> ids;
    ids.reserve(60'000);
    const std::size_t before = g_allocs.load();
    for (OrderId id = 1; id <= 50'000; ++id) {
        if (pct(rng) < 30 && !ids.empty()) {
            book.cancel(ids[rng() % ids.size()]);
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            book.add(Order::limit(AAPL, id, 1, s, px(rng), qty(rng)));
            ids.push_back(id);
        }
    }
    EXPECT_EQ(g_allocs.load(), before);
}

TEST(BookTypes, Formatting) {
    EXPECT_EQ(toString(RejectReason::OutOfRange), "OutOfRange");
    EXPECT_EQ(toString(RejectReason::WrongTicker), "WrongTicker");
    EXPECT_EQ(toString(RejectReason::None), "None");
    EXPECT_EQ(toString(RejectReason::ZeroQuantity), "ZeroQuantity");
    EXPECT_EQ(toString(RejectReason::InvalidPrice), "InvalidPrice");
    EXPECT_EQ(toString(RejectReason::InvalidOrderId), "InvalidOrderId");
    EXPECT_EQ(toString(RejectReason::DuplicateOrderId), "DuplicateOrderId");
    EXPECT_EQ(toString(RejectReason::AlreadyExpired), "AlreadyExpired");
    EXPECT_EQ(toString(static_cast<RejectReason>(99)), "?");
    Trade t{1, 2, 3, 4, Side::Buy, price(9, 99), 5};
    EXPECT_EQ(t.toString(), "Trade[BUY 5 @ 9.99 buy:#1 sell:#2]");
}

}  // namespace
