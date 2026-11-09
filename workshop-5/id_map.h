#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace std;

template <typename V>
class IdMap final {
public:
    static constexpr uint64_t kEmpty = ~uint64_t{0};

    explicit IdMap(size_t max_entries) {
        size_t cap = 16;
        while (cap < max_entries * 2) cap <<= 1;
        slots_.assign(cap, Slot{kEmpty, V{}});
        mask_ = cap - 1;
    }

    V *find(uint64_t key) noexcept {
        for (size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return &slots_[i].value;
            if (slots_[i].key == kEmpty) return nullptr;
        }
    }

    const V *find(uint64_t key) const noexcept { return const_cast<IdMap *>(this)->find(key); }

    bool insert(uint64_t key, V value) noexcept {
        assert(key != kEmpty);
        if (size_ * 2 >= slots_.size()) return false;
        size_t i = home(key);
        while (slots_[i].key != kEmpty) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{key, value};
        ++size_;
        return true;
    }

    bool erase(uint64_t key) noexcept {
        size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        size_t j = i;
        while (true) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kEmpty) break;
            const size_t k = home(slots_[j].key);
            const bool movable = (i <= j) ? (k <= i || k > j) : (k <= i && k > j);
            if (movable) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = kEmpty;
        --size_;
        return true;
    }

    size_t size() const noexcept { return size_; }

private:
    struct Slot {
        uint64_t key;
        V value;
    };

    size_t home(uint64_t key) const noexcept {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        return static_cast<size_t>(key) & mask_;
    }

    vector<Slot> slots_;
    size_t mask_ = 0;
    size_t size_ = 0;
};
