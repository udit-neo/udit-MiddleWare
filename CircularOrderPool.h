#pragma once
#ifndef CIRCULAR_ORDER_POOL_H
#define CIRCULAR_ORDER_POOL_H

// =============================================================================
// CircularOrderPool.h
//
// A pool of pre-built API2::COMMON::OrderWrapper objects keyed by a composite
// symbol+mode identifier.  The middleware creates pools lazily — on the first
// placeOrder() call for a given key — and keeps them alive for the lifetime of
// the strategy.
//
// Pool management rules (matching Oms.cpp behaviour):
//   • acquire()  — takes the front wrapper from the queue; returns nullptr if
//                  empty.  Caller must call reset() on the wrapper before use.
//   • release()  — calls reset() on the wrapper then pushes it to the back so
//                  it is ready for the next order.
//   • Wrappers are stored by value in a std::deque so their addresses are
//     stable while they sit in the pool.  Once acquired, the pointer remains
//     valid until release() is called because the wrapper lives in _inUse.
// =============================================================================

#include <deque>
#include <unordered_map>
#include <cstdint>
#include <../common/orderWrapper.h>

class CircularOrderPool {
public:
    explicit CircularOrderPool(std::size_t poolSize = 600)
        : _poolSize(poolSize) {}

    // ── Population ────────────────────────────────────────────────────────────
    void addWrapper(API2::COMMON::OrderWrapper wrapper) {
        _available.push_back(std::move(wrapper));
    }

    // ── Acquire ───────────────────────────────────────────────────────────────
    // Returns a raw pointer to a wrapper ready for newOrder().
    // The wrapper has been reset(); the caller must NOT reset() it again before
    // calling newOrder() — that is already done here.
    // Returns nullptr if pool is empty.
    API2::COMMON::OrderWrapper* acquire() {
        if (_available.empty()) return nullptr;
        _inUse.push_back(std::move(_available.front()));
        _available.pop_front();
        _inUse.back().reset();
        return &_inUse.back();
    }

    // ── Release ───────────────────────────────────────────────────────────────
    // Called after an order reaches a terminal state (filled/cancelled/rejected).
    // Resets the wrapper and returns it to the back of the available queue.
    void release(API2::COMMON::OrderWrapper* ptr) {
        if (!ptr) return;
        for (auto it = _inUse.begin(); it != _inUse.end(); ++it) {
            if (&(*it) == ptr) {
                it->reset();
                _available.push_back(std::move(*it));
                _inUse.erase(it);
                return;
            }
        }
    }

    // ── Queries ───────────────────────────────────────────────────────────────
    bool        hasAvailable()     const { return !_available.empty(); }
    std::size_t getAvailableCount() const { return _available.size(); }
    std::size_t getInUseCount()    const { return _inUse.size(); }
    std::size_t poolSize()         const { return _poolSize; }

private:
    std::size_t                             _poolSize;
    std::deque<API2::COMMON::OrderWrapper>  _available;  // ready to use
    std::deque<API2::COMMON::OrderWrapper>  _inUse;      // currently live
};

#endif // CIRCULAR_ORDER_POOL_H
