#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>

namespace lob::engine {

/// Fixed-size block allocator over one pre-allocated buffer.
///
/// Used to back node-based standard containers (std::map in design A) so that
/// creating and destroying price levels never calls the heap. Every block is
/// threaded onto the free list in the constructor, which also touches every
/// page up front: page faults happen at construction, not on the hot path.
class BlockArena {
public:
    BlockArena(std::size_t block_size, std::size_t block_align, std::size_t capacity)
        : block_size_(round_up(std::max(block_size, sizeof(FreeBlock)), block_align)),
          block_align_(block_align),
          capacity_(capacity),
          storage_(static_cast<std::byte*>(
                       ::operator new(block_size_ * capacity, std::align_val_t{block_align})),
                   AlignedDelete{block_align}) {
        for (std::size_t i = capacity; i-- > 0;) {
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): placement-new into arena storage
            free_ = ::new (storage_.get() + (i * block_size_)) FreeBlock{free_};
        }
    }

    BlockArena(const BlockArena&) = delete;
    BlockArena& operator=(const BlockArena&) = delete;
    BlockArena(BlockArena&&) = delete;
    BlockArena& operator=(BlockArena&&) = delete;
    ~BlockArena() = default;

    /// Throws std::bad_alloc when exhausted (callers size the arena so that
    /// this cannot happen; it is a guard, not a code path).
    [[nodiscard]] void* allocate() {
        if (free_ == nullptr) {
            throw std::bad_alloc();
        }
        FreeBlock* block = free_;
        free_ = block->next;
        ++in_use_;
        return block;
    }

    void deallocate(void* block) noexcept {
        assert(block != nullptr);
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): placement-new into arena storage
        free_ = ::new (block) FreeBlock{free_};
        --in_use_;
    }

    [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] std::size_t block_align() const noexcept { return block_align_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }

private:
    struct FreeBlock {
        FreeBlock* next;
    };

    struct AlignedDelete {
        std::size_t align;
        void operator()(std::byte* ptr) const noexcept {
            ::operator delete(ptr, std::align_val_t{align});
        }
    };

    [[nodiscard]] static constexpr std::size_t round_up(std::size_t n, std::size_t align) {
        return (n + align - 1) / align * align;
    }

    std::size_t block_size_;
    std::size_t block_align_;
    std::size_t capacity_;
    std::unique_ptr<std::byte, AlignedDelete> storage_;
    FreeBlock* free_{nullptr};
    std::size_t in_use_{0};
};

/// Standard-conforming allocator that serves single objects from a BlockArena.
/// Node-based containers only ever allocate one node at a time; anything else
/// (n != 1, or a node larger than a block) is a configuration error and throws.
template <class T>
class ArenaAllocator {
public:
    using value_type = T;

    explicit ArenaAllocator(BlockArena& arena) noexcept : arena_(&arena) {}

    template <class U>
    // Implicit on purpose: allocator rebinding requires converting construction.
    // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor)
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena()) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n != 1 || sizeof(T) > arena_->block_size() || alignof(T) > arena_->block_align()) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(arena_->allocate());
    }

    void deallocate(T* ptr, std::size_t /*n*/) noexcept { arena_->deallocate(ptr); }

    [[nodiscard]] BlockArena* arena() const noexcept { return arena_; }

    template <class U>
    friend bool operator==(const ArenaAllocator& lhs, const ArenaAllocator<U>& rhs) noexcept {
        return lhs.arena() == rhs.arena();
    }

private:
    BlockArena* arena_;
};

}  // namespace lob::engine
