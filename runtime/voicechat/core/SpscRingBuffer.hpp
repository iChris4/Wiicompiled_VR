// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from MKW-VoiceChat 0.14.8 (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <span>

namespace mkwvc {

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 1);

public:
    std::size_t push(std::span<const T> values) {
        const auto write=write_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        const auto freeSpace=write>=read ? Capacity-(write-read)-1 : read-write-1;
        const auto count=std::min(values.size(),freeSpace);
        for(std::size_t i=0;i<count;++i) data_[(write+i)%Capacity]=values[i];
        write_.store((write+count)%Capacity,std::memory_order_release);
        return count;
    }

    // All of `values` or nothing, so interleaved channels never shift.
    bool pushAll(std::span<const T> values) {
        const auto write=write_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        const auto freeSpace=write>=read ? Capacity-(write-read)-1 : read-write-1;
        if(values.size()>freeSpace) return false;
        for(std::size_t i=0;i<values.size();++i) data_[(write+i)%Capacity]=values[i];
        write_.store((write+values.size())%Capacity,std::memory_order_release);
        return true;
    }

    std::size_t pop(std::span<T> values) {
        const auto read=read_.load(std::memory_order_relaxed);
        const auto write=write_.load(std::memory_order_acquire);
        const auto available=write>=read ? write-read : Capacity-(read-write);
        const auto count=std::min(values.size(),available);
        for(std::size_t i=0;i<count;++i) values[i]=data_[(read+i)%Capacity];
        read_.store((read+count)%Capacity,std::memory_order_release);
        return count;
    }

    std::size_t available() const {
        const auto read=read_.load(std::memory_order_acquire);
        const auto write=write_.load(std::memory_order_acquire);
        return write>=read ? write-read : Capacity-(read-write);
    }

private:
    std::array<T,Capacity> data_{};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
};

}
