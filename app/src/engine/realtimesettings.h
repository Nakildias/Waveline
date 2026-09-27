// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <type_traits>

namespace waveline {

// Serialized control-side writers, exactly one audio-side reader. Each side
// owns a slot; exchanging the third slot transfers ownership without making
// the audio thread wait, allocate, or retry. Intermediate edits may coalesce.
template<class T> class RealtimeSettings {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
public:
    void set(const T &value) {
        std::lock_guard lock(controlMutex_);
        requested_ = value;
        slots_[write_] = value;
        write_ = middle_.exchange(write_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }

    T get() const {
        std::lock_guard lock(controlMutex_);
        return requested_;
    }

    // Called only by the audio thread (or before that thread starts).
    bool consume(T &value) {
        if (!(middle_.load(std::memory_order_acquire) & kDirty)) return false;
        read_ = middle_.exchange(read_, std::memory_order_acq_rel) & kIndex;
        value = slots_[read_];
        return true;
    }

private:
    static constexpr unsigned kDirty = 4, kIndex = 3;
    std::array<T, 3> slots_{};
    unsigned read_ = 0, write_ = 1;
    std::atomic<unsigned> middle_{2 | kDirty};
    mutable std::mutex controlMutex_;
    T requested_{};
};

} // namespace waveline
