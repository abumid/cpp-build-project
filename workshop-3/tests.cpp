// tests.cpp — tests for mem_pool.h, id_map.h, order.h, order_book.h (crypto spot exchange model)
// Build: make test

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "id_map.h"
#include "mem_pool.h"
#include "order.h"
#include "order_book.h"

// Counts every heap allocation so we can prove the hot path never allocates.
static std::atomic<std::size_t> g_allocs{0};
void *operator new(std::size_t n) {
    ++g_allocs;
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define TEST(name) static void name()
#define RUN(name)                                                                       \
    do {                                                                                \
        int before = g_failures;                                                        \
        name();                                                                         \
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", #name);      \
    } while (0)

// Records every event the book emits (allocates — only for tests).
struct Recorder {
    std::vector<ExecutionReport> reports;
    std::vector<TradeEvent> trades;
    std::vector<DepthUpdate> depth;
    void onExecutionReport(const ExecutionReport &r) { reports.push_back(r); }
    void onTrade(const TradeEvent &t) { trades.push_back(t); }
    void onDepthUpdate(const DepthUpdate &d) { depth.push_back(d); }
    void clear() { reports.clear(); trades.clear(); depth.clear(); }
};

static Millis fixedClock() { return 1'700'000'000'000ULL; }

// A permissive symbol for logic tests (no notional / order-count limits).
static SymbolInfo testSymbol() {
    SymbolInfo s = SymbolInfo::btcusdt();
    s.min_notional = 0;
    s.max_notional = 0;
    s.max_num_orders = 0;
    s.default_stp = StpMode::None;
    return s;
}
static OrderBook makeBook(SymbolInfo info = testSymbol()) { return OrderBook(info, 1024, 256, &fixedClock); }

static const Symbol BTC = "BTCUSDT";

// ===========================================================================
// MemPool
// ===========================================================================
TEST(mempool_basics) {
    MemPool<int> pool(4);
    int *a = pool.allocate(10);
    int *b = pool.allocate(20);
    CHECK(a && b && *a == 10 && *b == 20);
    CHECK_EQ(b, a + 1);
    pool.deallocate(a);
    CHECK_EQ(pool.allocate(30), a);  // LIFO reuse of the hot slot
    (void)pool.allocate(1);
    (void)pool.allocate(2);
    CHECK(pool.allocate(3) == nullptr);
}

struct Tracked {
    static inline int alive = 0;
    explicit Tracked(int) { ++alive; }
    ~Tracked() { --alive; }
};

TEST(mempool_lifetimes_and_alignment) {
    Tracked::alive = 0;
    {
        MemPool<Tracked> pool(3);
        CHECK_EQ(Tracked::alive, 0);
        Tracked *t = pool.allocate(1);
        (void)pool.allocate(2);
        CHECK_EQ(Tracked::alive, 2);
        pool.deallocate(t);
        CHECK_EQ(Tracked::alive, 1);
    }
    CHECK_EQ(Tracked::alive, 0);

    struct alignas(64) Line { char c[64]; };
    MemPool<Line> lines(4);
    for (int i = 0; i < 4; ++i) CHECK_EQ(reinterpret_cast<std::uintptr_t>(lines.allocate()) % 64, 0u);
}

// ===========================================================================
// IdMap (open addressing, backward-shift delete)
// ===========================================================================
TEST(idmap_matches_unordered_map) {
    IdMap<int> m(5000);
    std::unordered_map<std::uint64_t, int> ref;
    std::mt19937_64 rng(3);
    int bad = 0;
    for (int i = 0; i < 200'000; ++i) {
        std::uint64_t k = rng() % 6000;
        int op = static_cast<int>(rng() % 3);
        if (op == 0 && ref.size() < 5000) {
            bool a = m.insert(k, i), b = ref.emplace(k, i).second;
            if (a != b) ++bad;
        } else if (op == 1) {
            if (m.erase(k) != (ref.erase(k) == 1)) ++bad;
        } else {
            int *v = m.find(k);
            auto it = ref.find(k);
            if ((v == nullptr) != (it == ref.end()) || (v && *v != it->second)) ++bad;
        }
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(m.size(), ref.size());
}

// ===========================================================================
// Decimal + Order
// ===========================================================================
TEST(decimal_parsing_and_formatting) {
    CHECK_EQ("1"_d, 100'000'000);
    CHECK_EQ("0.01"_d, 1'000'000);
    CHECK_EQ("67250.01"_d, 6'725'001'000'000);
    CHECK_EQ("0.00001"_d, 1'000);
    CHECK_EQ("-1.5"_d, -150'000'000);
    CHECK_EQ(toString("67250.01"_d), std::string("67250.01000000"));
    CHECK_EQ(toString("67250.01"_d, 2), std::string("67250.01"));
    CHECK_EQ(toString("0.5"_d, 5), std::string("0.50000"));
    CHECK_EQ(notional("67250"_d, "0.002"_d), "134.5"_d);
    CHECK_EQ(decimalsOf("0.01"_d), 2);
    CHECK_EQ(decimalsOf("0.00001"_d), 5);
    CHECK_EQ(decimalsOf("1"_d), 0);
}

TEST(order_tostring_is_json) {
    Order o = Order::limit(BTC, 1, Side::Buy, "67250.01"_d, "0.5"_d).withClientOrderId("my-order-1");
    o.order_id = 28;
    const std::string s = o.toString();
    CHECK(s.find(R"("symbol":"BTCUSDT")") != std::string::npos);
    CHECK(s.find(R"("orderId":28)") != std::string::npos);
    CHECK(s.find(R"("clientOrderId":"my-order-1")") != std::string::npos);
    CHECK(s.find(R"("price":"67250.01000000")") != std::string::npos);
    CHECK(s.find(R"("origQty":"0.50000000")") != std::string::npos);
    CHECK(s.find(R"("status":"NEW")") != std::string::npos);
    CHECK(s.find(R"("type":"LIMIT")") != std::string::npos);
    CHECK(s.find(R"("timeInForce":"GTC")") != std::string::npos);
}

// ===========================================================================
// Validation / symbol filters and error codes
// ===========================================================================
TEST(filters_and_error_codes) {
    auto book = makeBook(SymbolInfo::btcusdt());
    auto code = [&](const Order &o) { return book.add(o).code; };

    CHECK_EQ(code(Order::limit("ETHUSDT", 1, Side::Buy, "100"_d, "1"_d)), -1121);
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, 0, "1"_d)), -1102);
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "100"_d, 0)), -1102);
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "100.001"_d, "1"_d)), -1013);   // tick 0.01
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "2000000"_d, "1"_d)), -1013);   // maxPrice
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "100"_d, "0.000001"_d)), -1013);  // step 0.00001
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "100"_d, "0.01"_d)), -1013);    // notional 1 < 5
    CHECK_EQ(code(Order::market(BTC, 1, Side::Buy, 0)), -1102);
    CHECK_EQ(code(Order::market(BTC, 1, Side::Buy, "200"_d)), -1013);             // MARKET_LOT_SIZE
    CHECK_EQ(code(Order::marketByQuote(BTC, 1, Side::Buy, "1"_d)), -1013);         // quote notional 1 < 5
    CHECK_EQ(code(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d).withClientOrderId("bad id!")), -1100);

    OrderResponse r = book.add(Order::limit(BTC, 1, Side::Buy, "100.001"_d, "1"_d));
    CHECK_EQ(r.toString(), std::string(R"({"code":-1013,"msg":"Filter failure: PRICE_FILTER"})"));
    CHECK(r.order.status == OrderStatus::Rejected);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(max_num_orders_filter) {
    SymbolInfo info = testSymbol();
    info.max_num_orders = 2;
    auto book = makeBook(info);
    CHECK(book.add(Order::limit(BTC, 7, Side::Buy, "100"_d, "1"_d)).ok());
    CHECK(book.add(Order::limit(BTC, 7, Side::Buy, "101"_d, "1"_d)).ok());
    OrderResponse r = book.add(Order::limit(BTC, 7, Side::Buy, "102"_d, "1"_d));
    CHECK_EQ(r.code, -1013);
    CHECK_EQ(r.msg, std::string_view("Filter failure: MAX_NUM_ORDERS"));
    CHECK(book.add(Order::limit(BTC, 8, Side::Buy, "102"_d, "1"_d)).ok());  // other user is fine
    CHECK(book.cancel(1).ok());
    CHECK(book.add(Order::limit(BTC, 7, Side::Buy, "102"_d, "1"_d)).ok());  // slot freed
}

TEST(server_overloaded_when_pool_full) {
    OrderBook book(testSymbol(), 2, 8, &fixedClock);
    CHECK(book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d)).ok());
    CHECK(book.add(Order::limit(BTC, 1, Side::Buy, "101"_d, "1"_d)).ok());
    CHECK_EQ(book.add(Order::limit(BTC, 1, Side::Buy, "102"_d, "1"_d)).code, -1008);
    CHECK(book.add(Order::limit(BTC, 2, Side::Sell, "101"_d, "1"_d, TimeInForce::IOC)).ok());  // IOC never rests, so it still trades
}

// ===========================================================================
// Order types / time in force
// ===========================================================================
TEST(limit_gtc_rests_with_price_time_priority) {
    auto book = makeBook();
    OrderResponse a = book.add(Order::limit(BTC, 1, Side::Buy, "67250"_d, "0.5"_d));
    OrderResponse b = book.add(Order::limit(BTC, 2, Side::Buy, "67250"_d, "0.7"_d));
    book.add(Order::limit(BTC, 3, Side::Buy, "67249.99"_d, "1"_d));
    book.add(Order::limit(BTC, 4, Side::Sell, "67250.01"_d, "0.8"_d));
    CHECK(a.ok() && a.order.order_id == 1 && a.order.status == OrderStatus::New);
    CHECK_EQ(b.order.order_id, 2u);
    CHECK_EQ(*book.bestBid(), "67250"_d);
    CHECK_EQ(*book.bestAsk(), "67250.01"_d);
    CHECK((book.queueAt(Side::Buy, "67250"_d) == std::vector<OrderId>{1, 2}));
    CHECK_EQ(book.volumeAt(Side::Buy, "67250"_d), "1.2"_d);
    CHECK_EQ(book.levelCount(Side::Buy), 2u);
}

// The Workshop 3 "order book transitions" slide, with BTCUSDT prices.
TEST(slide_transitions) {
    auto book = makeBook();
    Recorder rec;
    book.add(Order::limit(BTC, 1, Side::Sell, "100.01"_d, "8"_d), rec);
    book.add(Order::limit(BTC, 2, Side::Buy, "100.00"_d, "5"_d), rec);
    book.add(Order::limit(BTC, 3, Side::Buy, "100.00"_d, "7"_d), rec);
    book.add(Order::limit(BTC, 4, Side::Sell, "100.02"_d, "3"_d), rec);

    rec.clear();
    OrderResponse r = book.add(Order::limit(BTC, 5, Side::Buy, "100.01"_d, "10"_d), rec);
    CHECK(r.order.status == OrderStatus::PartiallyFilled);
    CHECK_EQ(r.order.executed_qty, "8"_d);
    CHECK_EQ(r.order.cum_quote_qty, "800.08"_d);
    CHECK_EQ(rec.trades.size(), 1u);
    CHECK_EQ(*book.bestBid(), "100.01"_d);
    CHECK_EQ(book.volumeAt(Side::Buy, "100.01"_d), "2"_d);

    rec.clear();
    r = book.add(Order::market(BTC, 6, Side::Sell, "10"_d), rec);
    CHECK(r.order.status == OrderStatus::Filled);
    CHECK_EQ(rec.trades.size(), 3u);
    CHECK(rec.trades[0].qty == "2"_d && rec.trades[0].price == "100.01"_d);
    CHECK(rec.trades[1].qty == "5"_d && rec.trades[1].price == "100.00"_d);
    CHECK(rec.trades[2].qty == "3"_d && rec.trades[2].price == "100.00"_d);
    CHECK(rec.trades[0].is_buyer_maker);  // seller was the taker
    CHECK_EQ(book.volumeAt(Side::Buy, "100.00"_d), "4"_d);
    CHECK_EQ(book.find(3)->leavesQty(), "4"_d);
    CHECK(book.find(3)->status == OrderStatus::PartiallyFilled);
    CHECK_EQ(book.lastPrice(), "100.00"_d);
}

TEST(event_sequence_is_correct) {
    auto book = makeBook();
    Recorder rec;
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d), rec);
    rec.clear();
    book.add(Order::limit(BTC, 2, Side::Buy, "100"_d, "1"_d), rec);
    // NEW for the taker, then a trade with TRADE reports for both sides
    CHECK_EQ(rec.reports.size(), 3u);
    CHECK(rec.reports[0].exec_type == ExecType::New);
    CHECK(rec.reports[1].exec_type == ExecType::Trade && !rec.reports[1].is_maker);
    CHECK(rec.reports[2].exec_type == ExecType::Trade && rec.reports[2].is_maker);
    CHECK(rec.reports[1].order.status == OrderStatus::Filled);
    CHECK(rec.reports[2].order.status == OrderStatus::Filled);
    CHECK_EQ(rec.trades.size(), 1u);
    CHECK_EQ(rec.trades[0].trade_id, 1u);
    CHECK_EQ(rec.trades[0].buyer_order_id, 2u);
    CHECK_EQ(rec.trades[0].seller_order_id, 1u);
    CHECK_EQ(rec.depth.size(), 1u);
    CHECK_EQ(rec.depth[0].qty, 0);  // level removed
    CHECK(rec.reports[1].toString().find(R"("x":"TRADE","X":"FILLED")") != std::string::npos);
    CHECK(rec.trades[0].toString().find(R"("e":"trade")") != std::string::npos);
}

TEST(ioc_expires_remainder) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    OrderResponse r = book.add(Order::limit(BTC, 2, Side::Buy, "100"_d, "3"_d, TimeInForce::IOC));
    CHECK(r.order.status == OrderStatus::Expired);
    CHECK_EQ(r.order.executed_qty, "1"_d);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(fok_all_or_nothing) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    book.add(Order::limit(BTC, 1, Side::Sell, "101"_d, "1"_d));
    OrderResponse r = book.add(Order::limit(BTC, 2, Side::Buy, "101"_d, "3"_d, TimeInForce::FOK));
    CHECK(r.order.status == OrderStatus::Expired);
    CHECK_EQ(r.order.executed_qty, 0);
    CHECK_EQ(book.orderCount(), 2u);  // nothing touched
    r = book.add(Order::limit(BTC, 2, Side::Buy, "101"_d, "2"_d, TimeInForce::FOK));
    CHECK(r.order.status == OrderStatus::Filled);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(market_order_without_liquidity_expires) {
    auto book = makeBook();
    OrderResponse r = book.add(Order::market(BTC, 1, Side::Buy, "1"_d));
    CHECK(r.ok());
    CHECK(r.order.status == OrderStatus::Expired);
}

TEST(market_by_quote_qty) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "50000"_d, "0.01"_d));
    book.add(Order::limit(BTC, 1, Side::Sell, "60000"_d, "1"_d));
    // spend 1100 USDT: 0.01 @ 50000 = 500, then 600 / 60000 = 0.01 @ 60000
    OrderResponse r = book.add(Order::marketByQuote(BTC, 2, Side::Buy, "1100"_d));
    CHECK(r.order.status == OrderStatus::Filled);
    CHECK_EQ(r.order.executed_qty, "0.02"_d);
    CHECK_EQ(r.order.cum_quote_qty, "1100"_d);
    CHECK_EQ(r.order.orig_qty, "0.02"_d);
    CHECK_EQ(book.volumeAt(Side::Sell, "60000"_d), "0.99"_d);

    auto empty = makeBook();
    empty.add(Order::limit(BTC, 1, Side::Sell, "50000"_d, "0.01"_d));
    r = empty.add(Order::marketByQuote(BTC, 2, Side::Buy, "1000"_d));  // runs out of book
    CHECK(r.order.status == OrderStatus::Expired);
    CHECK_EQ(r.order.executed_qty, "0.01"_d);
}

TEST(limit_maker_is_post_only) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    OrderResponse r = book.add(Order::limitMaker(BTC, 2, Side::Buy, "100"_d, "1"_d));
    CHECK_EQ(r.code, -2010);
    CHECK_EQ(r.msg, std::string_view("Order would immediately match and take."));
    r = book.add(Order::limitMaker(BTC, 2, Side::Buy, "99.99"_d, "1"_d));
    CHECK(r.ok() && r.order.status == OrderStatus::New);
    CHECK_EQ(*book.bestBid(), "99.99"_d);
}

// ===========================================================================
// Self-trade prevention
// ===========================================================================
TEST(stp_expire_taker) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    Recorder rec;
    OrderResponse r = book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d).withStp(StpMode::ExpireTaker), rec);
    CHECK(r.order.status == OrderStatus::ExpiredInMatch);
    CHECK(rec.trades.empty());
    CHECK(rec.reports.back().exec_type == ExecType::TradePrevention);
    CHECK_EQ(book.volumeAt(Side::Sell, "100"_d), "1"_d);  // maker untouched
}

TEST(stp_expire_maker) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));   // user 1 (own)
    book.add(Order::limit(BTC, 2, Side::Sell, "100"_d, "1"_d));   // user 2
    OrderResponse r = book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d).withStp(StpMode::ExpireMaker));
    CHECK(r.order.status == OrderStatus::Filled);  // own maker expired, filled against user 2
    CHECK(book.find(1) == nullptr);
    CHECK(book.find(2) == nullptr);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(stp_expire_both_and_symbol_default) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    OrderResponse r = book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d).withStp(StpMode::ExpireBoth));
    CHECK(r.order.status == OrderStatus::ExpiredInMatch);
    CHECK_EQ(book.orderCount(), 0u);

    auto real = makeBook(SymbolInfo::btcusdt());  // symbol default STP is EXPIRE_MAKER
    r = real.add(Order::limit(BTC, 1, Side::Sell, "100"_d, "1"_d));
    CHECK(r.order.stp == StpMode::ExpireMaker);
}

// ===========================================================================
// Cancel
// ===========================================================================
TEST(cancel_by_id_and_client_id) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "5"_d));
    book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "7"_d).withClientOrderId("bot-42"));
    book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "9"_d));

    OrderResponse r = book.cancel(2);
    CHECK(r.ok() && r.order.status == OrderStatus::Canceled);
    CHECK((book.queueAt(Side::Buy, "100"_d) == std::vector<OrderId>{1, 3}));

    r = book.cancel(2);
    CHECK_EQ(r.code, -2011);
    CHECK_EQ(r.toString(), std::string(R"({"code":-2011,"msg":"Unknown order sent."})"));
    CHECK_EQ(book.cancel(1, /*user*/ 99).code, -2011);  // not your order

    book.add(Order::limit(BTC, 5, Side::Sell, "200"_d, "1"_d).withClientOrderId("bot-42"));
    NullListener none;
    CHECK(book.cancelByClientId(5, "bot-42", none).ok());
    CHECK_EQ(book.cancelByClientId(5, "bot-42", none).code, -2011);

    CHECK(book.cancel(1).ok());
    CHECK(book.cancel(3).ok());
    CHECK(!book.bestBid().has_value());
    CHECK_EQ(book.levelCount(Side::Buy), 0u);
}

TEST(duplicate_client_order_id) {
    auto book = makeBook();
    CHECK(book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d).withClientOrderId("abc")).ok());
    CHECK_EQ(book.add(Order::limit(BTC, 1, Side::Buy, "99"_d, "1"_d).withClientOrderId("abc")).code, -2010);
    CHECK(book.add(Order::limit(BTC, 2, Side::Buy, "99"_d, "1"_d).withClientOrderId("abc")).ok());  // other user
    OrderResponse auto_id = book.add(Order::limit(BTC, 1, Side::Buy, "98"_d, "1"_d));
    CHECK_EQ(auto_id.order.client_order_id.view().size(), 22u);  // auto-generated 22-char id
}

// ===========================================================================
// Market data
// ===========================================================================
TEST(depth_snapshot_json) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Buy, "4"_d, "431"_d));
    book.add(Order::limit(BTC, 1, Side::Sell, "4.02"_d, "12"_d));
    CHECK_EQ(book.depthJson(),
             std::string(R"({"lastUpdateId":2,"bids":[["4.00000000","431.00000000"]],"asks":[["4.02000000","12.00000000"]]})"));
}

TEST(depth_updates_track_levels) {
    auto book = makeBook();
    Recorder rec;
    book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "1"_d), rec);
    book.add(Order::limit(BTC, 1, Side::Buy, "100"_d, "2"_d), rec);
    book.cancel(1, rec);
    CHECK_EQ(rec.depth.size(), 3u);
    CHECK(rec.depth[0].qty == "1"_d && rec.depth[1].qty == "3"_d && rec.depth[2].qty == "2"_d);
    CHECK_EQ(rec.depth[2].update_id, 3u);
    CHECK_EQ(book.lastUpdateId(), 3u);
}

TEST(tostring_ladder) {
    auto book = makeBook();
    book.add(Order::limit(BTC, 1, Side::Sell, "67251.50"_d, "0.12"_d));
    book.add(Order::limit(BTC, 1, Side::Sell, "67250.01"_d, "0.5"_d));
    book.add(Order::limit(BTC, 2, Side::Buy, "67250"_d, "1.25"_d));
    book.add(Order::limit(BTC, 2, Side::Buy, "67249.10"_d, "0.3"_d));
    const std::string s = book.toString();
    std::cout << s;
    CHECK(s.find("Price(USDT)") != std::string::npos);
    CHECK(s.find("Amount(BTC)") != std::string::npos);
    CHECK(s.find("67251.50") < s.find("67250.01"));
    CHECK(s.find("67250.01") < s.find("spread 0.01"));
    CHECK(s.find("spread") < s.find("67250.00"));
    CHECK(s.find("1.25000") != std::string::npos);
}

// ===========================================================================
// Performance properties
// ===========================================================================
TEST(hot_path_does_not_allocate) {
    OrderBook book(testSymbol(), 8192, 1024, &fixedClock);
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> px(9'990, 10'010), qty(1, 50), pct(0, 99);
    std::vector<OrderId> ids;
    ids.reserve(100'000);
    NullListener none;
    const std::size_t before = g_allocs.load();
    for (int i = 0; i < 50'000; ++i) {
        if (pct(rng) < 30 && !ids.empty()) {
            book.cancel(ids[static_cast<std::size_t>(rng()) % ids.size()], none);
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Order o = pct(rng) < 10 ? Order::market(BTC, rng() % 50, s, qty(rng) * "0.001"_d)
                                    : Order::limit(BTC, rng() % 50, s, px(rng) * "0.01"_d, qty(rng) * "0.001"_d);
            OrderResponse r = book.add(o, none);
            if (r.ok()) ids.push_back(r.order.order_id);
        }
    }
    CHECK_EQ(g_allocs.load(), before);
}

// ---------------------------------------------------------------------------
// Differential test vs a simple std::map reference (STP off, limit + market).
// ---------------------------------------------------------------------------
struct RefBook {
    struct E { OrderId id; Decimal qty; };
    std::map<Decimal, std::deque<E>, std::greater<>> bids;
    std::map<Decimal, std::deque<E>> asks;
    std::map<OrderId, std::pair<Side, Decimal>> where;

    template <typename L>
    Decimal take(L &levels, Side side, bool market, Decimal limit, Decimal q, std::vector<TradeEvent> &out) {
        while (q > 0 && !levels.empty()) {
            auto it = levels.begin();
            if (!market && (side == Side::Buy ? it->first > limit : it->first < limit)) break;
            auto &dq = it->second;
            while (q > 0 && !dq.empty()) {
                Decimal f = std::min(q, dq.front().qty);
                out.push_back(TradeEvent{BTC, 0, it->first, f, 0, 0, false, 0});
                q -= f;
                dq.front().qty -= f;
                if (dq.front().qty == 0) { where.erase(dq.front().id); dq.pop_front(); }
            }
            if (dq.empty()) levels.erase(it);
        }
        return q;
    }
    void add(OrderId id, const Order &o, std::vector<TradeEvent> &out) {
        bool mkt = o.type == OrderType::Market;
        Decimal rem = o.side == Side::Buy ? take(asks, o.side, mkt, o.price, o.orig_qty, out)
                                          : take(bids, o.side, mkt, o.price, o.orig_qty, out);
        if (rem > 0 && !mkt) {
            if (o.side == Side::Buy) bids[o.price].push_back({id, rem});
            else asks[o.price].push_back({id, rem});
            where[id] = {o.side, o.price};
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

struct TradeCollector {
    std::vector<TradeEvent> trades;
    void onExecutionReport(const ExecutionReport &) {}
    void onTrade(const TradeEvent &t) { trades.push_back(t); }
    void onDepthUpdate(const DepthUpdate &) {}
};

TEST(matches_reference_model_randomised) {
    OrderBook book(testSymbol(), 50'000, 4'000, &fixedClock);
    RefBook ref;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> px(9'900, 10'100), qty(1, 100), pct(0, 99);
    TradeCollector got;
    std::vector<TradeEvent> want;
    OrderId issued = 0;
    int mismatches = 0;

    for (int step = 0; step < 100'000 && mismatches == 0; ++step) {
        got.trades.clear();
        want.clear();
        const int roll = pct(rng);
        if (roll < 30 && issued > 0) {
            OrderId id = std::uniform_int_distribution<OrderId>(1, issued)(rng);
            if (book.cancel(id, got).ok() != ref.cancel(id)) ++mismatches;
        } else {
            Side s = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Order o = roll < 35 ? Order::market(BTC, 1, s, qty(rng) * "0.001"_d)
                                : Order::limit(BTC, 1, s, px(rng) * "0.01"_d, qty(rng) * "0.001"_d);
            OrderResponse r = book.add(o, got);
            if (!r.ok()) { ++mismatches; continue; }
            issued = r.order.order_id;
            ref.add(issued, o, want);
            if (got.trades.size() != want.size()) ++mismatches;
            for (std::size_t i = 0; mismatches == 0 && i < want.size(); ++i)
                if (got.trades[i].price != want[i].price || got.trades[i].qty != want[i].qty) ++mismatches;
        }
        auto rb = ref.bids.empty() ? std::optional<Decimal>() : std::optional<Decimal>(ref.bids.begin()->first);
        auto ra = ref.asks.empty() ? std::optional<Decimal>() : std::optional<Decimal>(ref.asks.begin()->first);
        if (book.bestBid() != rb || book.bestAsk() != ra || book.orderCount() != ref.where.size()) ++mismatches;
        if (book.bestBid() && book.bestAsk() && *book.bestBid() >= *book.bestAsk()) ++mismatches;
    }
    for (auto &[p, q] : ref.bids) {
        Decimal t = 0;
        for (auto &e : q) t += e.qty;
        if (book.volumeAt(Side::Buy, p) != t) ++mismatches;
    }
    for (auto &[p, q] : ref.asks) {
        Decimal t = 0;
        for (auto &e : q) t += e.qty;
        if (book.volumeAt(Side::Sell, p) != t) ++mismatches;
    }
    CHECK_EQ(mismatches, 0);
}

int main() {
    RUN(mempool_basics);
    RUN(mempool_lifetimes_and_alignment);
    RUN(idmap_matches_unordered_map);
    RUN(decimal_parsing_and_formatting);
    RUN(order_tostring_is_json);
    RUN(filters_and_error_codes);
    RUN(max_num_orders_filter);
    RUN(server_overloaded_when_pool_full);
    RUN(limit_gtc_rests_with_price_time_priority);
    RUN(slide_transitions);
    RUN(event_sequence_is_correct);
    RUN(ioc_expires_remainder);
    RUN(fok_all_or_nothing);
    RUN(market_order_without_liquidity_expires);
    RUN(market_by_quote_qty);
    RUN(limit_maker_is_post_only);
    RUN(stp_expire_taker);
    RUN(stp_expire_maker);
    RUN(stp_expire_both_and_symbol_default);
    RUN(cancel_by_id_and_client_id);
    RUN(duplicate_client_order_id);
    RUN(depth_snapshot_json);
    RUN(depth_updates_track_levels);
    RUN(tostring_ladder);
    RUN(hot_path_does_not_allocate);
    RUN(matches_reference_model_randomised);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
