// order_book_test.cpp — GoogleTest suite for the priority-queue order book (Workshop 4)
// Build and run: make test        Coverage report: make coverage

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <new>
#include <random>

#include "order_book.h"

// Counts heap allocations so a test can prove add/cancel never allocate.
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
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {

const Ticker AAPL = "AAPL";

// Records every trade and order event the book emits.
struct Recorder : OrderBookListener {
    std::vector<Trade> trades;
    std::vector<std::pair<Order, OrderEvent>> events;
    void onTrade(const Trade &t) override { trades.push_back(t); }
    void onOrderEvent(const Order &o, OrderEvent e) override { events.emplace_back(o, e); }
    int count(OrderEvent e) const {
        int n = 0;
        for (auto &[o, ev] : events) n += ev == e;
        return n;
    }
};

class OrderBookTest : public ::testing::Test {
protected:
    OrderBookTest() : book(AAPL, 1024, 256) { book.setListener(&rec); }

    AddResult buy(OrderId id, Price p, Qty q, ClientId c = 1, Millis ttl = 0) {
        return book.add(Order::limit(AAPL, id, c, Side::Buy, p, q, ttl));
    }
    AddResult sell(OrderId id, Price p, Qty q, ClientId c = 2, Millis ttl = 0) {
        return book.add(Order::limit(AAPL, id, c, Side::Sell, p, q, ttl));
    }

    OrderBook book;
    Recorder rec;
};

// ---------------------------------------------------------------------------
// Basics: adding, best price, price-time priority
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, EmptyBookHasNoBestPrices) {
    EXPECT_FALSE(book.bestBid().has_value());
    EXPECT_FALSE(book.bestAsk().has_value());
    EXPECT_EQ(book.orderCount(), 0u);
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 0u);
    EXPECT_EQ(book.find(42), nullptr);
}

TEST_F(OrderBookTest, HeapGivesBestBidAndAsk) {
    buy(1, price(99, 98), 4);
    buy(2, price(100, 1), 2);
    buy(3, price(100, 0), 12);
    sell(4, price(100, 5), 6);
    sell(5, price(100, 2), 3);
    sell(6, price(100, 3), 1);
    ASSERT_TRUE(book.bestBid().has_value());
    EXPECT_EQ(*book.bestBid(), price(100, 1));  // max-heap top
    EXPECT_EQ(*book.bestAsk(), price(100, 2));  // min-heap top
    EXPECT_EQ(book.levelCount(Side::Buy), 3u);
    EXPECT_EQ(book.levelCount(Side::Sell), 3u);
    EXPECT_EQ(book.orderCount(), 6u);
}

TEST_F(OrderBookTest, SamePriceKeepsTimePriority) {
    buy(1, price(100, 0), 5);
    buy(2, price(100, 0), 7);
    buy(3, price(100, 0), 1);
    EXPECT_EQ(book.queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{1, 2, 3}));
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 13u);
    EXPECT_EQ(book.heapSize(Side::Buy), 1u);  // one heap entry per price, not per order
}

// ---------------------------------------------------------------------------
// Matching (the flowchart on the "Matching logic" slide)
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, NonCrossingOrderRests) {
    sell(1, price(100, 1), 8);
    AddResult r = buy(2, price(100, 0), 5);
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(r.rested, 5u);
    EXPECT_TRUE(rec.trades.empty());
}

TEST_F(OrderBookTest, Workshop3SlideTransitions) {
    sell(1, price(100, 1), 8);
    buy(2, price(100, 0), 5);
    buy(3, price(100, 0), 7);
    sell(4, price(100, 2), 3);

    AddResult r = buy(5, price(100, 1), 10);  // fills 8, 2 rest at 100.01
    EXPECT_EQ(r.filled, 8u);
    EXPECT_EQ(r.rested, 2u);
    EXPECT_EQ(*book.bestBid(), price(100, 1));

    rec.trades.clear();
    r = book.add(Order::market(AAPL, 6, 9, Side::Sell, 10));  // 2 + 5 + 3
    EXPECT_EQ(r.filled, 10u);
    ASSERT_EQ(rec.trades.size(), 3u);
    EXPECT_EQ(rec.trades[0].qty, 2u);
    EXPECT_EQ(rec.trades[0].price, price(100, 1));
    EXPECT_EQ(rec.trades[1].qty, 5u);
    EXPECT_EQ(rec.trades[1].buy_order_id, 2u);
    EXPECT_EQ(rec.trades[2].qty, 3u);
    EXPECT_EQ(rec.trades[2].buy_order_id, 3u);
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 4u);
    EXPECT_EQ(*book.bestAsk(), price(100, 2));
}

TEST_F(OrderBookTest, IncomingSmallerThanRestingPartiallyFillsResting) {
    sell(1, price(50, 0), 10);
    AddResult r = buy(2, price(50, 0), 4);
    EXPECT_EQ(r.filled, 4u);
    ASSERT_NE(book.find(1), nullptr);
    EXPECT_EQ(book.find(1)->leaves(), 6u);
    EXPECT_EQ(book.find(1)->status, OrderStatus::PartiallyFilled);
}

TEST_F(OrderBookTest, TradeHappensAtRestingPrice) {
    sell(1, price(50, 0), 5);
    buy(2, price(55, 0), 5);
    ASSERT_EQ(rec.trades.size(), 1u);
    EXPECT_EQ(rec.trades[0].price, price(50, 0));
    EXPECT_EQ(rec.trades[0].aggressor, Side::Buy);
    EXPECT_EQ(rec.trades[0].seller, 2u);
}

TEST_F(OrderBookTest, LimitSweepsLevelsThenRestsRemainder) {
    sell(1, price(10, 0), 5);
    sell(2, price(10, 1), 5);
    sell(3, price(10, 5), 5);
    AddResult r = buy(4, price(10, 2), 20);
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.rested, 10u);
    EXPECT_EQ(*book.bestBid(), price(10, 2));
    EXPECT_EQ(*book.bestAsk(), price(10, 5));
}

TEST_F(OrderBookTest, SellSideMatchesBids) {
    buy(1, price(20, 0), 3);
    buy(2, price(21, 0), 3);
    AddResult r = sell(3, price(19, 0), 5);
    EXPECT_EQ(r.filled, 5u);
    ASSERT_EQ(rec.trades.size(), 2u);
    EXPECT_EQ(rec.trades[0].price, price(21, 0));  // best bid first
    EXPECT_EQ(rec.trades[1].price, price(20, 0));
}

TEST_F(OrderBookTest, MarketOrderLeftoverIsCancelled) {
    sell(1, price(10, 0), 3);
    AddResult r = book.add(Order::market(AAPL, 2, 1, Side::Buy, 10));
    EXPECT_EQ(r.filled, 3u);
    EXPECT_EQ(r.cancelled, 7u);
    EXPECT_EQ(r.rested, 0u);
    EXPECT_EQ(book.orderCount(), 0u);
}

TEST_F(OrderBookTest, MarketOrderOnEmptyBook) {
    AddResult r = book.add(Order::market(AAPL, 1, 1, Side::Sell, 5));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.cancelled, 5u);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, Rejections) {
    EXPECT_EQ(book.add(Order::limit("MSFT", 1, 1, Side::Buy, 100, 1)).reject, RejectReason::WrongTicker);
    EXPECT_EQ(buy(1, 100, 0).reject, RejectReason::ZeroQuantity);
    EXPECT_EQ(buy(1, 0, 5).reject, RejectReason::InvalidPrice);
    EXPECT_EQ(buy(IdMap<int>::kEmpty, 100, 5).reject, RejectReason::InvalidOrderId);
    EXPECT_TRUE(buy(1, 100, 5).accepted());
    EXPECT_EQ(buy(1, 100, 5).reject, RejectReason::DuplicateOrderId);
    book.setTime(1000);
    EXPECT_EQ(buy(2, 100, 5, 1, 1000).reject, RejectReason::AlreadyExpired);
}

TEST(RejectReasonTest, NamesAreReadable) {
    EXPECT_EQ(toString(RejectReason::None), "None");
    EXPECT_EQ(toString(RejectReason::WrongTicker), "WrongTicker");
    EXPECT_EQ(toString(RejectReason::ZeroQuantity), "ZeroQuantity");
    EXPECT_EQ(toString(RejectReason::InvalidPrice), "InvalidPrice");
    EXPECT_EQ(toString(RejectReason::InvalidOrderId), "InvalidOrderId");
    EXPECT_EQ(toString(RejectReason::DuplicateOrderId), "DuplicateOrderId");
    EXPECT_EQ(toString(RejectReason::AlreadyExpired), "AlreadyExpired");
    EXPECT_EQ(toString(static_cast<RejectReason>(99)), "?");
}

// ---------------------------------------------------------------------------
// Cancel and lazy deletion in the heap
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, CancelFromMiddleOfQueue) {
    buy(1, price(100, 0), 5);
    buy(2, price(100, 0), 7);
    buy(3, price(100, 0), 9);
    EXPECT_TRUE(book.cancel(2));
    EXPECT_EQ(book.queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{1, 3}));
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 14u);
    EXPECT_FALSE(book.cancel(2));
    EXPECT_FALSE(book.cancel(999));
    EXPECT_EQ(rec.count(OrderEvent::Canceled), 1);
}

TEST_F(OrderBookTest, CancelledLevelIsLazilyRemovedFromHeap) {
    buy(1, price(101, 0), 1);
    buy(2, price(100, 0), 1);
    ASSERT_TRUE(book.cancel(1));
    EXPECT_EQ(book.levelCount(Side::Buy), 1u);  // live levels
    EXPECT_EQ(book.heapSize(Side::Buy), 2u);    // stale 101.00 still in the heap
    EXPECT_EQ(*book.bestBid(), price(100, 0));  // peek skips and pops the stale top
    EXPECT_EQ(book.heapSize(Side::Buy), 1u);
}

TEST_F(OrderBookTest, EmptyLevelIsReusedWithoutDuplicateHeapEntry) {
    buy(1, price(100, 0), 1);
    buy(2, price(99, 0), 1);
    book.cancel(2);                      // 99.00 empty but still in heap
    buy(3, price(99, 0), 4);             // revives the same level
    EXPECT_EQ(book.heapSize(Side::Buy), 2u);
    EXPECT_EQ(book.levelCount(Side::Buy), 2u);
    EXPECT_EQ(book.volumeAt(Side::Buy, price(99, 0)), 4u);
}

TEST(OrderBookCapacity, FullLevelPoolIsCompacted) {
    OrderBook book(AAPL, 64, 3);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 101, 1));
    book.add(Order::limit(AAPL, 3, 1, Side::Buy, 102, 1));
    book.cancel(1);
    book.cancel(2);                       // two empty levels below the live top still use the pool
    EXPECT_EQ(book.heapSize(Side::Buy), 3u);
    AddResult r = book.add(Order::limit(AAPL, 4, 1, Side::Buy, 103, 1));
    EXPECT_EQ(r.rested, 1u);              // compaction freed them
    EXPECT_EQ(book.heapSize(Side::Buy), 2u);
    EXPECT_EQ(*book.bestBid(), 103);
}

TEST(OrderBookCapacity, CompactionCleansBothSides) {
    OrderBook book(AAPL, 64, 4);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 101, 1));
    book.add(Order::limit(AAPL, 3, 2, Side::Sell, 200, 1));
    book.add(Order::limit(AAPL, 4, 2, Side::Sell, 201, 1));
    book.cancel(1);
    book.cancel(4);  // empty levels sit below each side's live top
    AddResult r = book.add(Order::limit(AAPL, 5, 1, Side::Buy, 102, 1));
    EXPECT_EQ(r.rested, 1u);
    EXPECT_EQ(book.heapSize(Side::Buy), 2u);
    EXPECT_EQ(book.heapSize(Side::Sell), 1u);
}

TEST(OrderBookCapacity, NoLevelLeftCancelsRemainder) {
    OrderBook book(AAPL, 64, 2);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 101, 1));
    AddResult r = book.add(Order::limit(AAPL, 3, 1, Side::Buy, 102, 5));
    EXPECT_TRUE(r.accepted());
    EXPECT_EQ(r.cancelled, 5u);
}

TEST(OrderBookCapacity, NoOrderSlotLeftCancelsRemainder) {
    OrderBook book(AAPL, 2, 16);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 100, 1));
    AddResult r = book.add(Order::limit(AAPL, 3, 1, Side::Buy, 100, 1));
    EXPECT_EQ(r.cancelled, 1u);
    r = book.add(Order::limit(AAPL, 4, 2, Side::Sell, 100, 1));  // crossing still trades
    EXPECT_EQ(r.filled, 1u);
}

// ---------------------------------------------------------------------------
// update(orderId): down-size keeps priority, up-size goes to the back
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, DownsizeKeepsQueuePriority) {
    buy(1, price(100, 0), 10);
    buy(2, price(100, 0), 5);
    ASSERT_TRUE(book.update(1, 4));
    EXPECT_EQ(book.queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{1, 2}));
    EXPECT_EQ(book.find(1)->leaves(), 4u);
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 9u);
    EXPECT_EQ(rec.count(OrderEvent::Updated), 1);
}

TEST_F(OrderBookTest, UpsizeMovesToBackOfQueue) {
    buy(1, price(100, 0), 10);
    buy(2, price(100, 0), 5);
    book.setTime(50);
    ASSERT_TRUE(book.update(1, 12));
    EXPECT_EQ(book.queueAt(Side::Buy, price(100, 0)), (std::vector<OrderId>{2, 1}));
    EXPECT_EQ(book.find(1)->leaves(), 12u);
    EXPECT_EQ(book.find(1)->entry_time, 50u);
    EXPECT_EQ(book.volumeAt(Side::Buy, price(100, 0)), 17u);
}

TEST_F(OrderBookTest, UpsizeOfOnlyOrderKeepsLevelLive) {
    buy(1, price(100, 0), 1);
    ASSERT_TRUE(book.update(1, 3));
    EXPECT_EQ(book.levelCount(Side::Buy), 1u);
    EXPECT_EQ(*book.bestBid(), price(100, 0));
}

TEST_F(OrderBookTest, UpdateEdgeCases) {
    buy(1, price(100, 0), 10);
    EXPECT_FALSE(book.update(99, 5));
    EXPECT_TRUE(book.update(1, 10));  // same size: no-op
    EXPECT_EQ(rec.count(OrderEvent::Updated), 0);
    EXPECT_TRUE(book.update(1, 0));   // zero cancels
    EXPECT_EQ(book.find(1), nullptr);
}

TEST_F(OrderBookTest, UpdateOfPartiallyFilledOrderUsesOpenQuantity) {
    buy(1, price(100, 0), 10);
    sell(2, price(100, 0), 4);
    ASSERT_TRUE(book.update(1, 2));
    EXPECT_EQ(book.find(1)->leaves(), 2u);
    EXPECT_EQ(book.find(1)->filled, 4u);
}

// ---------------------------------------------------------------------------
// disconnect(clientId): cancel-on-disconnect
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, DisconnectCancelsAllOrdersOfClient) {
    buy(1, price(100, 0), 1, 7);
    buy(2, price(99, 0), 1, 8);
    sell(3, price(105, 0), 1, 7);
    sell(4, price(106, 0), 1, 7);
    EXPECT_EQ(book.ordersOf(7).size(), 3u);
    EXPECT_EQ(book.disconnect(7), 3u);
    EXPECT_EQ(book.orderCount(), 1u);
    EXPECT_TRUE(book.ordersOf(7).empty());
    EXPECT_EQ(book.disconnect(7), 0u);
    EXPECT_EQ(book.ordersOf(8), (std::vector<OrderId>{2}));
}

TEST_F(OrderBookTest, ClientListSurvivesRemovalFromMiddleAndTail) {
    buy(1, price(100, 0), 1, 7);
    buy(2, price(100, 0), 1, 7);
    buy(3, price(100, 0), 1, 7);
    book.cancel(2);  // middle of the client list
    book.cancel(1);  // tail of the client list
    EXPECT_EQ(book.ordersOf(7), (std::vector<OrderId>{3}));
    book.cancel(3);
    EXPECT_TRUE(book.ordersOf(7).empty());
}

// ---------------------------------------------------------------------------
// TTL (time-to-live)
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, ExpireOrdersRemovesDueOrders) {
    buy(1, price(100, 0), 1, 1, 100);
    buy(2, price(99, 0), 1, 1, 200);
    buy(3, price(98, 0), 1, 1);  // no TTL
    book.setTime(150);
    EXPECT_EQ(book.expireOrders(), 1u);
    EXPECT_EQ(book.find(1), nullptr);
    EXPECT_EQ(*book.bestBid(), price(99, 0));
    book.setTime(500);
    EXPECT_EQ(book.expireOrders(), 1u);
    EXPECT_EQ(book.orderCount(), 1u);
    EXPECT_EQ(rec.count(OrderEvent::Expired), 2);
}

TEST_F(OrderBookTest, CancelledTtlEntryIsSkipped) {
    buy(1, price(100, 0), 1, 1, 100);
    book.cancel(1);
    book.setTime(200);
    EXPECT_EQ(book.expireOrders(), 0u);  // stale heap entry popped, nothing expired
    EXPECT_EQ(book.ttlHeapSize(), 0u);
}

TEST_F(OrderBookTest, ExpiredMakerIsSkippedDuringMatching) {
    sell(1, price(100, 0), 5, 2, 100);
    sell(2, price(100, 0), 5, 3);
    book.setTime(150);
    AddResult r = buy(3, price(100, 0), 5);
    EXPECT_EQ(r.filled, 5u);
    ASSERT_EQ(rec.trades.size(), 1u);
    EXPECT_EQ(rec.trades[0].sell_order_id, 2u);  // expired #1 never trades
    EXPECT_EQ(book.find(1), nullptr);
}

TEST_F(OrderBookTest, ExpiredBidIsSkippedBySellTaker) {
    buy(1, price(100, 0), 5, 1, 100);
    buy(2, price(99, 0), 5, 3);
    book.setTime(100);
    AddResult r = sell(3, price(99, 0), 5);
    EXPECT_EQ(r.filled, 5u);
    ASSERT_EQ(rec.trades.size(), 1u);
    EXPECT_EQ(rec.trades[0].buy_order_id, 2u);
}

TEST(Listener, DefaultListenerIgnoresEvents) {
    OrderBook book(AAPL, 16, 8);
    OrderBookListener silent;
    book.setListener(&silent);
    book.add(Order::limit(AAPL, 1, 1, Side::Sell, 100, 1));
    EXPECT_EQ(book.add(Order::limit(AAPL, 2, 2, Side::Buy, 100, 1)).filled, 1u);
}

TEST(OrderBookCapacity, TtlHeapIsCompactedWhenFull) {
    OrderBook book(AAPL, 2, 16);
    book.add(Order::limit(AAPL, 1, 1, Side::Buy, 100, 1, 1000));
    book.add(Order::limit(AAPL, 2, 1, Side::Buy, 101, 1, 1000));
    book.cancel(1);
    book.cancel(2);
    EXPECT_EQ(book.ttlHeapSize(), 2u);  // both entries are stale
    book.add(Order::limit(AAPL, 3, 1, Side::Buy, 102, 1, 2000));
    EXPECT_EQ(book.ttlHeapSize(), 1u);  // stale entries compacted away
}

// ---------------------------------------------------------------------------
// Self-trade prevention: decrement-and-cancel
// ---------------------------------------------------------------------------
TEST(SelfTradePrevention, SmallerTakerIsCancelledLargerMakerKeepsRemainder) {
    OrderBook book(AAPL, 64, 16, StpMode::DecrementAndCancel);
    Recorder rec;
    book.setListener(&rec);
    book.add(Order::limit(AAPL, 1, 7, Side::Sell, 100, 10));
    AddResult r = book.add(Order::limit(AAPL, 2, 7, Side::Buy, 100, 4));
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(r.cancelled, 4u);
    EXPECT_TRUE(rec.trades.empty());
    EXPECT_EQ(book.find(1)->leaves(), 6u);
    EXPECT_EQ(book.volumeAt(Side::Sell, 100), 6u);
    EXPECT_EQ(rec.count(OrderEvent::SelfTradeCanceled), 2);
}

TEST(SelfTradePrevention, SmallerMakerIsCancelledTakerContinues) {
    OrderBook book(AAPL, 64, 16, StpMode::DecrementAndCancel);
    Recorder rec;
    book.setListener(&rec);
    book.add(Order::limit(AAPL, 1, 7, Side::Sell, 100, 3));  // own order first in queue
    book.add(Order::limit(AAPL, 2, 8, Side::Sell, 100, 5));  // someone else
    AddResult r = book.add(Order::limit(AAPL, 3, 7, Side::Buy, 100, 6));
    EXPECT_EQ(r.cancelled, 3u);  // 3 decremented from both
    EXPECT_EQ(r.filled, 3u);     // remaining 3 trade with client 8, still price-time order
    EXPECT_EQ(book.find(1), nullptr);
    ASSERT_EQ(rec.trades.size(), 1u);
    EXPECT_EQ(rec.trades[0].sell_order_id, 2u);
}

TEST(SelfTradePrevention, EqualSizesCancelBoth) {
    OrderBook book(AAPL, 64, 16, StpMode::DecrementAndCancel);
    book.add(Order::limit(AAPL, 1, 7, Side::Buy, 100, 5));
    AddResult r = book.add(Order::limit(AAPL, 2, 7, Side::Sell, 100, 5));
    EXPECT_EQ(r.cancelled, 5u);
    EXPECT_EQ(book.orderCount(), 0u);
    EXPECT_FALSE(book.bestBid().has_value());
}

TEST(SelfTradePrevention, OffByDefault) {
    OrderBook book(AAPL, 64, 16);
    book.add(Order::limit(AAPL, 1, 7, Side::Buy, 100, 5));
    AddResult r = book.add(Order::limit(AAPL, 2, 7, Side::Sell, 100, 5));
    EXPECT_EQ(r.filled, 5u);
}

// ---------------------------------------------------------------------------
// toString / formatting
// ---------------------------------------------------------------------------
TEST_F(OrderBookTest, ToStringLadder) {
    sell(1, price(100, 2), 3);
    sell(2, price(100, 1), 8);
    buy(3, price(100, 0), 5);
    buy(4, price(100, 0), 7);
    buy(5, price(99, 0), 1);
    const std::string s = book.toString(1);
    std::cout << s;
    EXPECT_EQ(s.find("100.02"), std::string::npos);  // depth 1 shows only the best ask
    EXPECT_NE(s.find("100.01"), std::string::npos);
    EXPECT_NE(s.find("spread 0.01"), std::string::npos);
    EXPECT_NE(s.find("[#3 c1 5] [#4 c1 7]"), std::string::npos);
    EXPECT_EQ(s.find("99.00"), std::string::npos);
}

TEST_F(OrderBookTest, ToStringOneSidedAndEmpty) {
    EXPECT_NE(book.toString().find("0 orders"), std::string::npos);
    buy(1, price(100, 0), 1);
    buy(2, price(101, 0), 1);
    book.cancel(2);  // stale level is not printed
    const std::string s = book.toString();
    EXPECT_EQ(s.find("101.00"), std::string::npos);
    EXPECT_EQ(s.find("spread"), std::string::npos);
}

TEST(Formatting, OrderAndTradeToString) {
    Order o = Order::limit(AAPL, 7, 3, Side::Buy, price(100, 5), 10, 500);
    EXPECT_EQ(o.toString(), "Order[id:7 client:3 AAPL LIMIT BUY qty:10 filled:0 @ 100.05 NEW ttl:500]");
    Order m = Order::market(AAPL, 8, 3, Side::Sell, 4);
    EXPECT_EQ(m.toString(), "Order[id:8 client:3 AAPL MARKET SELL qty:4 filled:0 @ MKT NEW]");
    Trade t{1, 2, 3, 4, Side::Sell, price(9, 99), 5};
    EXPECT_EQ(t.toString(), "Trade[SELL 5 @ 9.99 buy:#1 sell:#2]");
    EXPECT_EQ(priceToString(-price(1, 5)), "-1.05");
    EXPECT_EQ(toString(OrderStatus::PartiallyFilled), "PARTIALLY_FILLED");
    EXPECT_EQ(toString(OrderStatus::Expired), "EXPIRED");
    EXPECT_EQ(toString(OrderStatus::Rejected), "REJECTED");
    EXPECT_EQ(toString(OrderStatus::Canceled), "CANCELED");
    EXPECT_EQ(toString(OrderStatus::Filled), "FILLED");
    EXPECT_EQ(toString(static_cast<OrderStatus>(99)), "?");
}

// ---------------------------------------------------------------------------
// Performance property: no heap allocation on the hot path
// ---------------------------------------------------------------------------
TEST(Performance, AddAndCancelNeverAllocate) {
    OrderBook book(AAPL, 4096, 1024);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> px(9'990, 10'010), qty(1, 50), pct(0, 99);
    std::vector<OrderId> ids;
    ids.reserve(60'000);
    const std::size_t before = g_allocs.load();
    for (OrderId id = 1; id <= 50'000; ++id) {
        if (pct(rng) < 30 && !ids.empty()) {
            book.cancel(ids[rng() % ids.size()]);
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            book.add(Order::limit(AAPL, id, rng() % 20, s, px(rng), qty(rng)));
            ids.push_back(id);
        }
    }
    EXPECT_EQ(g_allocs.load(), before);
}

// ---------------------------------------------------------------------------
// Randomised comparison with a simple std::map reference book
// ---------------------------------------------------------------------------
struct RefBook {
    struct E { OrderId id; Qty qty; };
    std::map<Price, std::deque<E>, std::greater<>> bids;
    std::map<Price, std::deque<E>> asks;
    std::map<OrderId, std::pair<Side, Price>> where;

    template <typename L>
    Qty take(L &levels, Side side, bool market, Price limit, Qty q, std::vector<Trade> &out) {
        while (q > 0 && !levels.empty()) {
            auto it = levels.begin();
            if (!market && (side == Side::Buy ? it->first > limit : it->first < limit)) break;
            auto &dq = it->second;
            while (q > 0 && !dq.empty()) {
                Qty f = std::min(q, dq.front().qty);
                out.push_back(Trade{0, 0, 0, 0, side, it->first, f});
                q -= f;
                dq.front().qty -= f;
                if (dq.front().qty == 0) { where.erase(dq.front().id); dq.pop_front(); }
            }
            if (dq.empty()) levels.erase(it);
        }
        return q;
    }
    void add(const Order &o, std::vector<Trade> &out) {
        bool mkt = o.type == OrderType::Market;
        Qty rem = o.side == Side::Buy ? take(asks, o.side, mkt, o.price, o.qty, out)
                                      : take(bids, o.side, mkt, o.price, o.qty, out);
        if (rem > 0 && !mkt) {
            if (o.side == Side::Buy) bids[o.price].push_back({o.order_id, rem});
            else asks[o.price].push_back({o.order_id, rem});
            where[o.order_id] = {o.side, o.price};
        }
    }
    bool cancel(OrderId id) {
        auto w = where.find(id);
        if (w == where.end()) return false;
        auto erase = [&](auto &levels) {
            auto &q = levels[w->second.second];
            for (auto it = q.begin(); it != q.end(); ++it)
                if (it->id == id) { q.erase(it); break; }
            if (q.empty()) levels.erase(w->second.second);
        };
        if (w->second.first == Side::Buy) erase(bids); else erase(asks);
        where.erase(w);
        return true;
    }
};

TEST(Randomised, MatchesReferenceBook) {
    OrderBook book(AAPL, 50'000, 4'000);
    Recorder rec;
    book.setListener(&rec);
    RefBook ref;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> px(9'900, 10'100), qty(1, 100), pct(0, 99);
    std::vector<Trade> want;

    for (OrderId id = 1; id <= 50'000; ++id) {
        rec.trades.clear();
        want.clear();
        const int roll = pct(rng);
        if (roll < 30 && id > 1) {
            OrderId victim = std::uniform_int_distribution<OrderId>(1, id - 1)(rng);
            ASSERT_EQ(book.cancel(victim), ref.cancel(victim)) << "step " << id;
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Order o = roll < 35 ? Order::market(AAPL, id, 1, s, static_cast<Qty>(qty(rng)))
                                : Order::limit(AAPL, id, 1, s, px(rng), static_cast<Qty>(qty(rng)));
            ASSERT_TRUE(book.add(o).accepted());
            ref.add(o, want);
            ASSERT_EQ(rec.trades.size(), want.size()) << "step " << id;
            for (std::size_t i = 0; i < want.size(); ++i) {
                ASSERT_EQ(rec.trades[i].price, want[i].price) << "step " << id;
                ASSERT_EQ(rec.trades[i].qty, want[i].qty) << "step " << id;
            }
        }
        auto rb = ref.bids.empty() ? std::optional<Price>() : std::optional<Price>(ref.bids.begin()->first);
        auto ra = ref.asks.empty() ? std::optional<Price>() : std::optional<Price>(ref.asks.begin()->first);
        ASSERT_EQ(book.bestBid(), rb) << "step " << id;
        ASSERT_EQ(book.bestAsk(), ra) << "step " << id;
        ASSERT_EQ(book.orderCount(), ref.where.size()) << "step " << id;
    }
    for (auto &[p, q] : ref.bids) {
        Qty total = 0;
        for (auto &e : q) total += e.qty;
        EXPECT_EQ(book.volumeAt(Side::Buy, p), total);
    }
}

}  // namespace
