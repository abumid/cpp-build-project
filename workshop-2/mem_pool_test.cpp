// mem_pool_test.cpp — tests for Workshop 2 homework (mem_pool.h)
// Build: make test

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "mem_pool.h"

// Counts heap allocations so we can prove the pool never allocates after construction.
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
#define RUN(name)                                                                  \
    do {                                                                           \
        int before = g_failures;                                                   \
        name();                                                                    \
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", #name); \
    } while (0)

struct Order {
    std::uint64_t id = 0;
    std::int64_t price = 0;
    std::uint32_t qty = 0;
};

TEST(allocate_and_deallocate) {
    MemPool<int> pool(4);
    CHECK_EQ(pool.capacity(), 4u);
    int *a = pool.allocate(10);
    int *b = pool.allocate(20);
    CHECK(a && b && *a == 10 && *b == 20);
    CHECK_EQ(pool.size(), 2u);
    pool.deallocate(a);
    CHECK_EQ(pool.size(), 1u);
    pool.deallocate(nullptr);
    CHECK_EQ(pool.size(), 1u);
}

TEST(contiguous_slots) {
    MemPool<std::uint64_t> pool(8);
    std::uint64_t *prev = pool.allocate(0);
    for (int i = 1; i < 8; ++i) {
        std::uint64_t *p = pool.allocate(i);
        CHECK_EQ(p, prev + 1);  // one contiguous block, handed out in order
        prev = p;
    }
}

TEST(exhaustion_returns_nullptr) {
    MemPool<int> pool(2);
    CHECK(pool.allocate(1) != nullptr);
    CHECK(pool.allocate(2) != nullptr);
    CHECK(pool.allocate(3) == nullptr);
    CHECK_EQ(pool.available(), 0u);
}

TEST(lifo_reuse_of_hot_slot) {
    MemPool<int> pool(4);
    int *a = pool.allocate(1);
    (void)pool.allocate(2);
    pool.deallocate(a);
    CHECK_EQ(pool.allocate(3), a);  // most recently freed (cache-hot) slot first
}

struct Tracked {
    static inline int alive = 0;
    explicit Tracked(int) { ++alive; }
    ~Tracked() { --alive; }
};

TEST(constructors_and_destructors_run) {
    Tracked::alive = 0;
    {
        MemPool<Tracked> pool(3);  // raw storage: nothing constructed yet
        CHECK_EQ(Tracked::alive, 0);
        Tracked *t = pool.allocate(1);
        (void)pool.allocate(2);
        CHECK_EQ(Tracked::alive, 2);
        pool.deallocate(t);
        CHECK_EQ(Tracked::alive, 1);
    }  // pool destructor cleans up what was not returned
    CHECK_EQ(Tracked::alive, 0);
}

struct MoveOnly {
    int v;
    explicit MoveOnly(int x) : v(x) {}
    MoveOnly(MoveOnly &&) = default;
    MoveOnly(const MoveOnly &) = delete;
};
struct TakesMoveOnly {
    int v;
    explicit TakesMoveOnly(MoveOnly &&m) : v(m.v) {}
};

TEST(perfect_forwarding) {
    MemPool<TakesMoveOnly> pool(1);
    TakesMoveOnly *p = pool.allocate(MoveOnly{42});
    CHECK(p && p->v == 42);
}

struct alignas(64) CacheLine {
    char data[64];
};

TEST(alignment_respected) {
    MemPool<CacheLine> pool(4);
    for (int i = 0; i < 4; ++i) CHECK_EQ(reinterpret_cast<std::uintptr_t>(pool.allocate()) % 64, 0u);
}

TEST(owns) {
    MemPool<int> a(2), b(2);
    int *x = a.allocate(1);
    CHECK(a.owns(x));
    CHECK(!b.owns(x));
}

TEST(no_heap_allocation_at_runtime) {
    MemPool<Order> pool(1000);
    std::vector<Order *> ptrs;
    ptrs.reserve(1000);
    const std::size_t before = g_allocs.load();
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 1000; ++i) ptrs.push_back(pool.allocate());
        for (Order *p : ptrs) pool.deallocate(p);
        ptrs.clear();
    }
    CHECK_EQ(g_allocs.load(), before);
}

int main() {
    RUN(allocate_and_deallocate);
    RUN(contiguous_slots);
    RUN(exhaustion_returns_nullptr);
    RUN(lifo_reuse_of_hot_slot);
    RUN(constructors_and_destructors_run);
    RUN(perfect_forwarding);
    RUN(alignment_respected);
    RUN(owns);
    RUN(no_heap_allocation_at_runtime);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
