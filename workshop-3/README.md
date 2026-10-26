# Workshop 3: Exchange Order Book (Part 1)

Homework for the Open Avenues **"C++ Systems for Finance: Build a Trading Exchange from Scratch"** workshops.

**Homework:**
- `order.h`: an `Order` class with `toString()`
- `order_book.h`: an `OrderBook` with `add(order)`, `cancel(orderId)` and `toString()`
- Use `MemPool` to allocate orders, pick the data structures, and test the implementation

| File | Contents |
|---|---|
| `mem_pool.h` | Same pool as Workshop 2 |
| `id_map.h` | Fixed-capacity open-addressing hash map (linear probing, backward-shift delete) |
| `order.h` | 8-decimal fixed-point `Decimal`, exchange enums, `Order` with a JSON `toString()` |
| `order_book.h` | `SymbolInfo` filters, `OrderBook`, execution reports, trades, depth updates |
| `tests.cpp` | 26 tests / 156 checks |
| `bench.cpp` | Benchmarks |

## Data structures
| Need | Choice | Why |
|---|---|---|
| Orders at one price | intrusive doubly linked FIFO of `MemPool` nodes | time priority, O(1) cancel, nodes packed together |
| Price levels per side | sorted `vector<PriceLevel*>`, best price at the back, binary-search insert | matching touches only `back()`, contiguous, never reallocates |
| Order ID → order | `IdMap<Node*>` | O(1); `std::unordered_map` would allocate a node per insert |
| (account, clientOrderId) → order | `IdMap<Node*>` | duplicate check and cancel by client ID |
| Open orders per account | `IdMap<uint32_t>` | `MAX_NUM_ORDERS` filter |

## Exchange behaviour
- **Order types:** `LIMIT`, `MARKET` (by `quantity` or `quoteOrderQty`), `LIMIT_MAKER` (post-only)
- **Time in force:** `GTC`, `IOC`, `FOK`
- **Statuses:** `NEW`, `PARTIALLY_FILLED`, `FILLED`, `CANCELED`, `REJECTED`, `EXPIRED`, `EXPIRED_IN_MATCH`
- **Filters:** `PRICE_FILTER`, `LOT_SIZE`, `MARKET_LOT_SIZE`, `NOTIONAL`, `MAX_NUM_ORDERS`
- **Self-trade prevention:** `NONE`, `EXPIRE_TAKER`, `EXPIRE_MAKER`, `EXPIRE_BOTH`, with a per-symbol default
- **Errors:** `-1013`, `-1100`, `-1102`, `-1106`, `-1121`, `-2010`, `-2011`, `-1008`
- **Events:** `executionReport`, `trade`, `depthUpdate`, plus a depth snapshot with `lastUpdateId`

```cpp
OrderBook book(SymbolInfo::btcusdt(), 100'000, 4'096);
auto r = book.add(Order::limit("BTCUSDT", /*user*/ 1, Side::Buy, "67250.01"_d, "0.5"_d));
book.cancel(r.order.order_id);
std::cout << book.toString();
```

## Run
```bash
make test       # GCC or Clang, C++20, -Werror
make sanitize
make bench
```

Not modelled: balances and fees, stop orders, iceberg orders, OCO, amend/cancel-replace, and the DECREMENT/TRANSFER STP modes.
