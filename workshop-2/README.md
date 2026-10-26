# Workshop 2: Memory Pool

Homework for the Open Avenues **"C++ Systems for Finance: Build a Trading Exchange from Scratch"** workshops. It follows *Building Low Latency Applications with C++* (Sourav Ghosh, 2023).

**Homework:** write `mem_pool.h`:
- Eliminate heap allocation at runtime (pre-allocate everything up front)
- Keep hot objects cache-local in a contiguous vector
- Avoid any per-allocation overhead

```cpp
MemPool<Order> pool(1024);           // the only allocation happens here
Order *o = pool.allocate(args...);   // O(1), constructs in place (placement new)
pool.deallocate(o);                  // O(1), runs the destructor and frees the slot
```

## Design
It uses the "two vectors" layout from the slides (structure of arrays):

| Member | Role |
|---|---|
| `vector<Slot> storage_` | raw, correctly aligned slots for `T`, in one contiguous block |
| `vector<uint8_t> in_use_` | one flag per slot, used for double-free and bad-pointer asserts |
| `vector<uint32_t> free_stack_` | indices of the free slots |

- **O(1) every time.** The book's version scans for the next free flag, which slows down as the pool fills. A stack of free indices never scans.
- **LIFO reuse.** The slot freed most recently is the first one handed out again, and it is probably still in L1/L2.
- **No objects until allocate().** `std::construct_at` and `std::destroy_at` mean `T` doesn't need a default constructor, and nothing is built on top of a live object.
- **Not copyable or movable.** Copying or moving the pool is deleted, using C++26 `= delete("reason")` when the compiler supports it.

## Run
```bash
make test       # 29 checks
make sanitize
```
