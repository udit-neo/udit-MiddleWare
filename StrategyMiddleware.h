#pragma once
// =============================================================================
// StrategyMiddleware.h  —  MW::Bidding
//
// General-purpose base class for ALL strategies.
//
// Inheritance chain:
//   AnyConcreteStrategy  →  MW::Bidding  →  API2::SGContext
//
// What the middleware does
// ────────────────────────
//  1. Pool management
//       The strategy calls registerPool(key, poolSize) to create a named pool
//       identified by any uint64_t key of its choosing.
//       It then calls addToPool(key, wrapper) for each pre-built wrapper.
//       The middleware owns the pool lifecycle from that point forward.
//
//  2. Order placement
//       placeOrder(key, symbolId, price, qty, triggerPrice, tag)
//         — acquires the next wrapper from pool[key]
//         — calls wrapper->newOrder(...)
//         — allocates an OrderHandle in the OrderStateStore
//         — records orderId* → OrderRecord for O(1) confirmation routing
//         — returns the OrderHandle (INVALID_ORDER_HANDLE on any failure)
//
//       The strategy calls wrapper->newOrder() itself for multi-leg wrappers
//       (IOC spread, basket) via placeDirect() — see below.
//
//  3. Modify / cancel
//       modifyOrder(handle, newPrice, newQty)
//       cancelOrder(handle)
//       Both route through the recorded wrapper and drive the store.
//
//  4. Confirmation routing
//       uTrade on* callbacks arrive here, look up orderId* → OrderRecord,
//       update the OrderStateStore, call the strategy virtual hook, and
//       return the wrapper to its pool when the order is terminal.
//
//  5. Market data routing
//       onMarketDataEvent() deduplicates by timestamp then calls
//       onMarketUpdate(symbolId) — the strategy override.
//
//  6. OrderStateStore
//       Automatic, transparent. Strategies query state via:
//         orderState(handle)   → full OrderState snapshot
//         orderStatus(handle)  → OrderStatus enum
//         isOrderOpen(handle)  → bool
//
// What strategies must do
// ────────────────────────
//   1. Choose a uint64_t pool key per order type (any scheme — enum, hash, …).
//   2. Call registerPool(key, size) once per key.
//   3. Call addToPool(key, wrapper) to seed the pool with pre-built wrappers.
//   4. Call placeOrder() for single-leg orders, OR
//      acquire(key) + wrapper->newOrder(...) + recordPlaced() for multi-leg.
//   5. Override on* virtual hooks to receive order and market-data events.
//   6. Never call reqNewSingleOrder or touch wrappers directly outside step 4.
//
// Pool key scheme
// ────────────────
// The strategy decides what keys mean. Common patterns:
//   • Enum value:      enum class Side { Buy=1, Sell=2 };  key = (uint64_t)Side::Buy
//   • Symbol+mode:     key = (symbolId << 1) | (mode-1)
//   • Arbitrary int:   key = 42   (single pool for the whole strategy)
//   • Spread type:     key = CONVERSION_POOL or REVERSAL_POOL
// The middleware does not interpret keys; it just maps them to pools.
// =============================================================================

#include "OrderDefs.h"
#include "OrderStateStore.h"
#include "CircularOrderPool.h"

#include <../includes/sgContext.h>
#include <../includes/sgApiParameters.h>
#include <../includes/sharedSingleOrder.h>
#include <../includes/apiConstants.h>
#include <../includes/userParamsReader.h>
#include <../includes/baseCommands.h>
#include <../includes/api2UserCommands.h>
#include <../src/common/common.h>
#include <../common/orderWrapper.h>

#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <stdexcept>

namespace MW {

// =============================================================================
// OrderRecord — middleware bookkeeping for one live order
// Strategies never see this; it is internal to MW::Bidding.
// =============================================================================
struct OrderRecord {
    OrderHandle                  handle   = INVALID_ORDER_HANDLE;
    uint64_t                     poolKey  = 0;
    API2::COMMON::OrderWrapper*  wrapper  = nullptr;  // stable pointer into pool
    long                         clOrdId  = 0;        // kept in sync via patchClOrdId
};

// =============================================================================
// MW::Bidding
// =============================================================================
class Bidding : public API2::SGContext {
public:

    // =========================================================================
    // Construction
    // =========================================================================
    explicit Bidding(API2::StrategyParameters* params,
                     const std::string&        sgName)
        : API2::SGContext(params, sgName)
    {}

    virtual ~Bidding() = default;

    // =========================================================================
    // Pool registration
    //
    // registerPool(key, size)
    //   Creates an empty pool identified by `key` with capacity hint `size`.
    //   Idempotent: calling again for an existing key with wrappers is a no-op.
    //   Must be called before addToPool() or placeOrder() for that key.
    //
    // addToPool(key, wrapper)
    //   Hands a pre-built, fully-constructed OrderWrapper to the pool.
    //   The middleware takes ownership from this point.
    //   The strategy builds the wrapper with whatever constructor it needs
    //   (single-leg, two-leg, three-leg IOC, etc.) — the middleware does not
    //   know or care about the wrapper's internal structure.
    //
    // Example (3-leg bidding strategy):
    //   registerPool(CONV_KEY, 10);
    //   for (int i = 0; i < 10; ++i) {
    //       API2::COMMON::OrderWrapper w(this,
    //           instrFut,  CMD_OrderMode_BUY,  account,
    //           instrCall, CMD_OrderMode_SELL, account,
    //           instrPut,  CMD_OrderMode_BUY,  account);
    //       addToPool(CONV_KEY, std::move(w));
    //   }
    //
    // Example (single-leg market-making strategy):
    //   registerPool(BUY_KEY, 20);
    //   for (int i = 0; i < 20; ++i) {
    //       API2::COMMON::OrderWrapper w(instrFut, CMD_OrderMode_BUY, this, acct);
    //       addToPool(BUY_KEY, std::move(w));
    //   }
    // =========================================================================
    void registerPool(uint64_t key, std::size_t size = 10) {
        if (_pools.count(key) && _pools[key].hasAvailable()) return;
        _pools.emplace(key, CircularOrderPool(size));
    }

    void addToPool(uint64_t key, API2::COMMON::OrderWrapper wrapper) {
        auto it = _pools.find(key);
        if (it == _pools.end()) {
            std::cerr << "[MW] addToPool: pool key " << key
                      << " not registered. Call registerPool() first.\n";
            return;
        }
        it->second.addWrapper(std::move(wrapper));
    }

    // =========================================================================
    // placeOrder  — single-leg (and single-fire multi-leg) path
    //
    // Acquires a wrapper from pool[key], calls wrapper->newOrder(price, qty,
    // trigPx), allocates an OrderHandle, and returns it.
    //
    // Use this for any wrapper whose order is fully described by
    // (price, qty, triggerPrice) — covers single-leg limit/stop orders AND
    // uTrade basket/spread wrappers that accept a single price set.
    //
    // For multi-leg IOC wrappers that need separate prices per leg, use
    // acquire() + manual newOrder() + recordPlaced().
    //
    // Returns INVALID_ORDER_HANDLE on:
    //   • pool key not found
    //   • pool exhausted (all wrappers in use)
    //   • wrapper->newOrder() returns false
    // =========================================================================
    OrderHandle placeOrder(uint64_t           key,
                           SIGNED_LONG        primarySymbolId,
                           SIGNED_LONG        price,
                           SIGNED_LONG        qty,
                           SIGNED_LONG        triggerPrice = 0,
                           const std::string& tag = "")
    {
        auto* wrapper = _acquireWrapper(key);
        if (!wrapper) return INVALID_ORDER_HANDLE;

        OrderHandle h = _allocateHandle(primarySymbolId, price, triggerPrice, qty, tag);

        API2::DATA_TYPES::RiskStatus rs = 0;
        SIGNED_LONG trigPx = triggerPrice ? triggerPrice : price;
        bool ok = wrapper->newOrder(rs, price, qty, trigPx);

        return _finalise(key, wrapper, h, rs, ok);
    }

    //PlaceMultiOrder
    
    // =========================================================================
    // acquire / recordPlaced  — manual path for multi-leg IOC wrappers
    //
    // Use when the wrapper needs per-leg prices/qtys that don't fit the
    // single-price placeOrder() signature:
    //
    //   auto* w = acquire(IOC_KEY);
    //   if (!w) { /* handle pool exhaustion */ return; }
    //   bool ok = w->newOrder(_rs,
    //       futPrice, qty,
    //       callPrice, qty,
    //       putPrice, qty);
    //   OrderHandle h = recordPlaced(IOC_KEY, w, symbolId, futPrice, qty, ok);
    //
    // acquire()      — takes wrapper from front of pool; returns nullptr if empty.
    // recordPlaced() — registers the wrapper in the reverse map, drives the store,
    //                  returns the handle (INVALID_ORDER_HANDLE if ok==false).
    // =========================================================================
    API2::COMMON::OrderWrapper* acquire(uint64_t key) {
        return _acquireWrapper(key);
    }

    OrderHandle recordPlaced(uint64_t                    key,
                             API2::COMMON::OrderWrapper* wrapper,
                             SIGNED_LONG                 primarySymbolId,
                             SIGNED_LONG                 price,
                             SIGNED_LONG                 qty,
                             bool                        wireCallSucceeded,
                             const std::string&          tag = "")
    {
        OrderHandle h = _allocateHandle(primarySymbolId, price, 0, qty, tag);
        API2::DATA_TYPES::RiskStatus rs = 0;   // caller already called newOrder
        return _finalise(key, wrapper, h, rs, wireCallSucceeded);
    }

    // =========================================================================
    // modifyOrder
    // =========================================================================
    bool modifyOrder(OrderHandle handle,
                     SIGNED_LONG newPrice,
                     SIGNED_LONG newQty,
                     SIGNED_LONG newTriggerPrice = 0)
    {
        auto* rec = _findByHandle(handle);
        if (!rec) {
            std::cerr << "[MW] modifyOrder: unknown handle " << handle << "\n";
            return false;
        }
        if (!rec->wrapper->isOrderOpen()) {
            std::cerr << "[MW] modifyOrder: handle " << handle << " not open\n";
            return false;
        }

        try { _store.recordReplaceSent(handle, newPrice,
                                       static_cast<uint64_t>(newQty)); }
        catch (const std::exception& e) {
            std::cerr << "[MW] modifyOrder: " << e.what() << "\n";
        }

        API2::DATA_TYPES::RiskStatus rs = 0;
        SIGNED_LONG trigPx = newTriggerPrice ? newTriggerPrice : newPrice;
        bool ok = rec->wrapper->replaceOrder(rs, newPrice, newQty, trigPx);

        if (!ok) {
            try { _store.onReplaceRejected(rec->clOrdId); } catch (...) {}
            std::cerr << "[MW] modifyOrder: replaceOrder FAILED\n";
        }
        return ok;
    }

    // =========================================================================
    // cancelOrder
    // =========================================================================
    bool cancelOrder(OrderHandle handle)
    {
        auto* rec = _findByHandle(handle);
        if (!rec) {
            std::cerr << "[MW] cancelOrder: unknown handle " << handle << "\n";
            return false;
        }
        if (!rec->wrapper->isOrderOpen()) {
            std::cerr << "[MW] cancelOrder: handle " << handle << " not open\n";
            return false;
        }

        try { _store.recordCancelSent(handle); }
        catch (const std::exception& e) {
            std::cerr << "[MW] cancelOrder: " << e.what() << "\n";
        }

        API2::DATA_TYPES::RiskStatus rs = 0;
        bool ok = rec->wrapper->cancelOrder(rs);

        if (!ok) {
            try { _store.onCancelRejected(rec->clOrdId); } catch (...) {}
            std::cerr << "[MW] cancelOrder: cancelOrder FAILED\n";
        }
        return ok;
    }

    // =========================================================================
    // State queries
    // =========================================================================
    OrderState  orderState(OrderHandle h)  const { return _store.snapshot(h);  }
    OrderStatus orderStatus(OrderHandle h) const { return _store.statusOf(h);  }
    bool        isOrderOpen(OrderHandle h) const { return isLive(orderStatus(h)); }

    // Pool diagnostics
    std::size_t poolAvailable(uint64_t key) const {
        auto it = _pools.find(key);
        return it != _pools.end() ? it->second.getAvailableCount() : 0;
    }
    std::size_t poolInUse(uint64_t key) const {
        auto it = _pools.find(key);
        return it != _pools.end() ? it->second.getInUseCount() : 0;
    }

    // =========================================================================
    // uTrade SGContext confirmation callbacks
    //
    // Flow for every callback:
    //   1. _lookup(orderId*)         → find OrderRecord (O(1))
    //   2. wrapper->processConfirmation()
    //   3. drive OrderStateStore
    //   4. call strategy virtual hook
    //   5. terminal → _release(orderId*)  → wrapper back to pool
    // =========================================================================

    void onProcessOrderConfirmation(API2::OrderConfirmation&) override {}

    void onConfirmed(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        _patchAndConfirm(conf, rec->handle, orderId);
        onOrderConfirmed(conf, orderId, rec->handle);
    }

    void onNewReject(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        _patchAndReject(conf, rec->handle, orderId);
        onOrderRejected(conf, orderId, rec->handle);
        _release(orderId);
    }

    void onReplaced(API2::OrderConfirmation& conf,
                    API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onReplaceConfirmed(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onReplaced: " << e.what() << "\n"; }
        onOrderReplaced(conf, orderId, rec->handle);
    }

    void onReplaceRejected(API2::OrderConfirmation& conf,
                           API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onReplaceRejected(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onReplaceRejected: " << e.what() << "\n"; }
        onOrderReplaceRejected(conf, orderId, rec->handle);
    }

    void onCanceled(API2::OrderConfirmation& conf,
                    API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onCancelled(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onCancelled: " << e.what() << "\n"; }
        onOrderCancelled(conf, orderId, rec->handle);
        _release(orderId);
    }

    void onIOCCanceled(API2::OrderConfirmation& conf,
                       API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onCancelled(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onIOCCancelled: " << e.what() << "\n"; }
        onOrderCancelled(conf, orderId, rec->handle);
        _release(orderId);
    }

    void onCancelRejected(API2::OrderConfirmation& conf,
                          API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onCancelRejected(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onCancelRejected: " << e.what() << "\n"; }
        onOrderCancelRejected(conf, orderId, rec->handle);
        // Order still live — do NOT release wrapper
    }

    void onPartialFill(API2::OrderConfirmation& conf,
                       API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try {
            _store.onPartialFill(_confClOrdId(conf),
                static_cast<uint64_t>(conf.getLastFillQuantity()),
                static_cast<int64_t>(conf.getLastFillPrice()));
        } catch (const std::exception& e) {
            std::cerr << "[MW][Store] onPartialFill: " << e.what() << "\n"; }
        onOrderPartialFill(conf, orderId, rec->handle);
        // Still open — do NOT release
    }

    void onFilled(API2::OrderConfirmation& conf,
                  API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try {
            _store.onFilled(_confClOrdId(conf),
                static_cast<uint64_t>(conf.getLastFillQuantity()),
                static_cast<int64_t>(conf.getLastFillPrice()));
        } catch (const std::exception& e) {
            std::cerr << "[MW][Store] onFilled: " << e.what() << "\n"; }
        onOrderFilled(conf, orderId, rec->handle);
        _release(orderId);   // terminal — wrapper back to pool
    }

    void onFrozen(API2::OrderConfirmation& conf,
                  API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        try { _store.onFrozen(_confClOrdId(conf)); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onFrozen: " << e.what() << "\n"; }
        onOrderFrozen(conf, orderId, rec->handle);
        _release(orderId);
    }

    void onMarketToLimit(API2::OrderConfirmation& conf,
                         API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        // Status unchanged — order still OPEN at a new price; no store transition
        onOrderMarketToLimit(conf, orderId, rec->handle);
    }

    void onRmsReject(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        auto* rec = _lookup(orderId);
        if (!rec) return;
        reqQryDebugLog()->saveConfirmation(conf);
        rec->wrapper->processConfirmation(conf);
        _patchAndReject(conf, rec->handle, orderId);
        onOrderRmsReject(conf, orderId, rec->handle);
        _release(orderId);
    }

    // ── Market data ───────────────────────────────────────────────────────────
    // Deduplicates by timestamp; calls onMarketUpdate() for new ticks only.
    void onMarketDataEvent(UNSIGNED_LONG symbolId) override
    {
        auto* mkd = reqQryUpdateMarketData(symbolId);
        if (!mkd) return;
        UNSIGNED_LONG ts   = mkd->getTimeStamp();
        auto&         prev = _symbolTs[static_cast<SIGNED_LONG>(symbolId)];
        if (ts == prev) return;
        prev = ts;
        onMarketUpdate(static_cast<SIGNED_LONG>(symbolId));
    }

    // =========================================================================
    // Virtual hooks — override in derived strategy
    //
    // Every order hook receives:
    //   conf    — raw uTrade confirmation (for fill price, qty, reject reason …)
    //   orderId — uTrade order pointer (rarely needed; handle is preferred)
    //   handle  — MW::OrderHandle for orderState(handle) queries
    // =========================================================================
    virtual void onOrderConfirmed      (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderRejected       (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderReplaced       (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderReplaceRejected(API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderCancelled      (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderCancelRejected (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderPartialFill    (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderFilled         (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderFrozen         (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderMarketToLimit  (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}
    virtual void onOrderRmsReject      (API2::OrderConfirmation&, API2::COMMON::OrderId*, OrderHandle) {}

    // Called after timestamp dedup for each new market data tick.
    virtual void onMarketUpdate(SIGNED_LONG /*symbolId*/) {}

protected:
    // Accessible to derived strategies for advanced use (e.g. snapshot all
    // active orders, audit fills, etc.)
    OrderStateStore _store;

    // Pool map: strategy-defined key → CircularOrderPool
    std::unordered_map<uint64_t, CircularOrderPool> _pools;

    // symbolId → last-seen timestamp (market-data dedup)
    std::unordered_map<SIGNED_LONG, UNSIGNED_LONG> _symbolTs;

private:
    // orderId* → OrderRecord  (primary reverse map for confirmation routing)
    std::unordered_map<API2::COMMON::OrderId*, OrderRecord> _orderIdToRecord;

    // ── Internal helpers ──────────────────────────────────────────────────────

    // Acquire next wrapper from a pool; returns nullptr and logs on failure.
    API2::COMMON::OrderWrapper* _acquireWrapper(uint64_t key) {
        auto it = _pools.find(key);
        if (it == _pools.end()) {
            std::cerr << "[MW] pool key " << key
                      << " not found. Call registerPool() first.\n";
            return nullptr;
        }
        auto* w = it->second.acquire();
        if (!w)
            std::cerr << "[MW] pool key " << key << " exhausted.\n";
        return w;
    }

    // Allocate a store entry before the wire call.
    OrderHandle _allocateHandle(SIGNED_LONG symbolId,
                                SIGNED_LONG price,
                                SIGNED_LONG triggerPrice,
                                SIGNED_LONG qty,
                                const std::string& tag)
    {
        NewOrderRequest req;
        req.symbolId  = static_cast<uint64_t>(symbolId);
        req.price     = price;
        req.stopPrice = triggerPrice ? triggerPrice : price;
        req.qty       = static_cast<uint64_t>(qty);
        req.tag       = tag;
        return _store.allocate(req);
    }

    // Common tail of placeOrder() and recordPlaced():
    // drives the store, builds reverse maps, returns handle.
    OrderHandle _finalise(uint64_t                     key,
                          API2::COMMON::OrderWrapper*  wrapper,
                          OrderHandle                  h,
                          API2::DATA_TYPES::RiskStatus rs,
                          bool                         ok)
    {
        if (!ok) {
            std::cerr << "[MW] order FAILED (riskStatus=" << rs << ")\n";
            _store.setRiskStatus(h, static_cast<int>(rs));
            // Return wrapper immediately — it was acquired but never sent
            auto it = _pools.find(_poolKeyOf(wrapper));
            if (it != _pools.end()) it->second.release(wrapper);
            return INVALID_ORDER_HANDLE;
        }

        // Wire call succeeded → CREATED → PENDING_NEW
        // Use a synthetic sentinel (negative of handle) because the real
        // clOrdId has not arrived yet; patchClOrdId() replaces it on confirm.
        long syntheticId = _syntheticId(h);
        try { _store.recordSent(h, syntheticId); }
        catch (const std::exception& e) {
            std::cerr << "[MW] recordSent: " << e.what() << "\n";
        }
        _store.setRiskStatus(h, static_cast<int>(rs));

        // Register in reverse map; seed clOrdId with the synthetic sentinel
        // so modifyOrder/cancelOrder rollbacks can use rec->clOrdId directly
        // without touching the incomplete API2::COMMON::OrderId type.
        OrderRecord rec;
        rec.handle  = h;
        rec.poolKey = key;
        rec.wrapper = wrapper;
        rec.clOrdId = syntheticId;   // updated to real id by _patchAndConfirm/Reject
        _orderIdToRecord[wrapper->_orderId] = rec;

        return h;
    }

    // Find record by orderId* — O(1).
    OrderRecord* _lookup(API2::COMMON::OrderId* orderId) {
        auto it = _orderIdToRecord.find(orderId);
        return (it != _orderIdToRecord.end()) ? &it->second : nullptr;
    }

    // Find record by handle — O(n) but only used in modify/cancel.
    OrderRecord* _findByHandle(OrderHandle h) {
        for (auto& kv : _orderIdToRecord)
            if (kv.second.handle == h) return &kv.second;
        return nullptr;
    }

    // Release wrapper back to its pool and erase from reverse map.
    void _release(API2::COMMON::OrderId* orderId) {
        auto it = _orderIdToRecord.find(orderId);
        if (it == _orderIdToRecord.end()) return;

        auto poolIt = _pools.find(it->second.poolKey);
        if (poolIt != _pools.end())
            poolIt->second.release(it->second.wrapper);

        _orderIdToRecord.erase(it);
    }

    // Find which pool a wrapper belongs to (used in _finalise on failure).
    uint64_t _poolKeyOf(API2::COMMON::OrderWrapper* wrapper) const {
        for (const auto& kv : _orderIdToRecord)
            if (kv.second.wrapper == wrapper) return kv.second.poolKey;
        return 0;
    }

    // Extract clOrdId from confirmation.
    static long _confClOrdId(const API2::OrderConfirmation& conf) {
        return static_cast<long>(conf.getClOrderId());
    }

    // Negative of handle — unique sentinel used before exchange assigns clOrdId.
    static long _syntheticId(OrderHandle h) {
        return -static_cast<long>(h);
    }

    // Patch the clOrdId sentinel → real id, then drive onNewConfirmed.
    // Also updates rec.clOrdId so modifyOrder/cancelOrder rollbacks stay in sync.
    void _patchAndConfirm(API2::OrderConfirmation& conf, OrderHandle h,
                          API2::COMMON::OrderId* orderId) {
        long realId = _confClOrdId(conf);
        _store.patchClOrdId(_syntheticId(h), realId);
        // Keep the record's cached clOrdId up to date
        auto it = _orderIdToRecord.find(orderId);
        if (it != _orderIdToRecord.end()) it->second.clOrdId = realId;
        try { _store.onNewConfirmed(realId, conf.getExchangeOrderId()); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onNewConfirmed: " << e.what() << "\n"; }
    }

    // Patch sentinel → real id, then drive onNewRejected.
    void _patchAndReject(API2::OrderConfirmation& conf, OrderHandle h,
                         API2::COMMON::OrderId* orderId) {
        long realId = _confClOrdId(conf);
        _store.patchClOrdId(_syntheticId(h), realId);
        auto it = _orderIdToRecord.find(orderId);
        if (it != _orderIdToRecord.end()) it->second.clOrdId = realId;
        try { _store.onNewRejected(realId); }
        catch (const std::exception& e) {
            std::cerr << "[MW][Store] onNewRejected: " << e.what() << "\n"; }
    }
};

} // namespace MW
