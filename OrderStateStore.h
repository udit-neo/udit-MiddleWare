#pragma once
// =============================================================================
// OrderStateStore.h
//
// Thread-safe store of every order's full lifecycle state.
//
// Responsibilities
// ─────────────────
//  • Allocate a unique OrderHandle per order and initialise its OrderState.
//  • Drive validated state transitions on every confirmation event.
//  • Maintain a clOrdId → OrderHandle reverse index for O(1) lookup in
//    confirmation callbacks.
//  • Support patchClOrdId() so the middleware can replace the synthetic
//    sentinel id (used before the exchange assigns a real one) with the
//    actual clOrdId the first time a confirmation carries it.
//  • Expose read-only snapshot / status queries.
//
// State machine (legal transitions only; all others are logged and thrown)
// ─────────────────────────────────────────────────────────────────────────
//   CREATED
//     └─ recordSent()        → PENDING_NEW
//
//   PENDING_NEW
//     ├─ onNewConfirmed()    → OPEN
//     └─ onNewRejected()     → REJECTED  (terminal)
//
//   OPEN
//     ├─ recordReplaceSent() → PENDING_MODIFY
//     ├─ recordCancelSent()  → PENDING_CANCEL
//     ├─ onPartialFill()     → PARTIALLY_FILLED
//     └─ onFilled()          → FILLED    (terminal)
//
//   PENDING_MODIFY
//     ├─ onReplaceConfirmed()→ OPEN
//     ├─ onReplaceRejected() → OPEN / PARTIALLY_FILLED  (order still live)
//     ├─ onPartialFill()     → PARTIALLY_FILLED          (fill races modify)
//     ├─ onFilled()          → FILLED    (terminal)
//     └─ recordCancelSent()  → PENDING_CANCEL
//
//   PENDING_CANCEL
//     ├─ onCancelled()       → CANCELLED (terminal)
//     ├─ onCancelRejected()  → OPEN / PARTIALLY_FILLED  (order still live)
//     ├─ onPartialFill()     → PARTIALLY_FILLED          (fill races cancel)
//     └─ onFilled()          → FILLED    (terminal)
//
//   PARTIALLY_FILLED
//     ├─ recordReplaceSent() → PENDING_MODIFY
//     ├─ recordCancelSent()  → PENDING_CANCEL
//     ├─ onPartialFill()     → PARTIALLY_FILLED  (additional partial)
//     └─ onFilled()          → FILLED    (terminal)
//
//   FILLED / CANCELLED / REJECTED / FROZEN  — terminal, no outgoing transitions
//
// Thread safety
// ──────────────
//   All public methods are protected by a single mutex.  All confirmation
//   callbacks originate from the uTrade event thread, so in practice the
//   lock is uncontested; it is kept here for safety if the store is ever
//   queried from a secondary thread (e.g. a monitoring thread).
// =============================================================================

#include "OrderDefs.h"

#include <unordered_map>
#include <mutex>
#include <stdexcept>
#include <iostream>
#include <string>
#include <chrono>

namespace MW {

class OrderStateStore {
public:

    // =========================================================================
    // allocate
    //
    // Called by MW::Bidding::placeOrder() BEFORE the wire call.
    // Inserts a new OrderState in CREATED status and returns its handle.
    // The clOrdId is not yet known at this point; recordSent() supplies it.
    // =========================================================================
    OrderHandle allocate(const NewOrderRequest& req) {
        std::lock_guard<std::mutex> lk(_mtx);

        OrderHandle h = ++_counter;

        OrderState s;
        s.handle       = h;
        s.status       = OrderStatus::CREATED;
        s.symbolId     = req.symbolId;
        s.tag          = req.tag;
        s.requestedQty = req.qty;
        s.pendingQty   = req.qty;
        s.orderPrice   = req.price;
        s.createdNs    = _nowNs();
        s.lastUpdateNs = s.createdNs;

        _orders[h] = std::move(s);
        return h;
    }

    // =========================================================================
    // recordSent
    //
    // Called immediately after wrapper->newOrder() succeeds.
    // Transitions CREATED → PENDING_NEW and registers the clOrdId reverse index.
    //
    // Pass clOrdId = 0 (or a negative sentinel via _syntheticId) if the real
    // exchange id is not yet available; call patchClOrdId() later.
    // =========================================================================
    void recordSent(OrderHandle h, long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getRef(h);
        _validateTransition(s.status, OrderStatus::PENDING_NEW, h);

        s.status       = OrderStatus::PENDING_NEW;
        s.clOrdId      = clOrdId;
        s.lastUpdateNs = _nowNs();

        _byClOrdId[clOrdId] = h;
    }

    // =========================================================================
    // patchClOrdId
    //
    // Replaces oldId with newId in the reverse index and in the OrderState.
    // No-op if oldId == newId or if oldId is not found.
    //
    // Used by MW::Bidding when the first confirmation carries the real clOrdId
    // and the store was seeded with a synthetic sentinel (recordSent(h, 0) or
    // recordSent(h, -h)).
    // =========================================================================
    void patchClOrdId(long oldId, long newId) {
        if (oldId == newId) return;

        std::lock_guard<std::mutex> lk(_mtx);

        auto it = _byClOrdId.find(oldId);
        if (it == _byClOrdId.end()) return;   // already patched or unknown sentinel

        OrderHandle h = it->second;
        _byClOrdId.erase(it);
        _byClOrdId[newId] = h;

        auto os = _orders.find(h);
        if (os != _orders.end())
            os->second.clOrdId = newId;
    }

    // =========================================================================
    // recordReplaceSent
    //
    // Called before wrapper->replaceOrder().
    // Transitions OPEN / PARTIALLY_FILLED → PENDING_MODIFY.
    // Also updates the stored price and qty to the requested new values.
    // =========================================================================
    void recordReplaceSent(OrderHandle h, int64_t newPrice, uint64_t newQty) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getRef(h);
        if (s.status != OrderStatus::OPEN &&
            s.status != OrderStatus::PARTIALLY_FILLED)
        {
            _logIllegal(h, s.status, OrderStatus::PENDING_MODIFY);
            throw std::logic_error(
                "recordReplaceSent: order not in modifiable state ("
                + std::string(statusName(s.status)) + ")");
        }

        s.status       = OrderStatus::PENDING_MODIFY;
        s.orderPrice   = newPrice;
        s.requestedQty = newQty;
        s.pendingQty   = (newQty > s.filledQty) ? (newQty - s.filledQty) : 0;
        s.lastUpdateNs = _nowNs();
    }

    // =========================================================================
    // recordCancelSent
    //
    // Called before wrapper->cancelOrder().
    // Transitions OPEN / PARTIALLY_FILLED / PENDING_MODIFY → PENDING_CANCEL.
    // =========================================================================
    void recordCancelSent(OrderHandle h) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getRef(h);
        if (s.status != OrderStatus::OPEN            &&
            s.status != OrderStatus::PARTIALLY_FILLED &&
            s.status != OrderStatus::PENDING_MODIFY)
        {
            _logIllegal(h, s.status, OrderStatus::PENDING_CANCEL);
            throw std::logic_error(
                "recordCancelSent: order not in cancellable state ("
                + std::string(statusName(s.status)) + ")");
        }

        s.status       = OrderStatus::PENDING_CANCEL;
        s.lastUpdateNs = _nowNs();
    }

    // =========================================================================
    // Confirmation callbacks — driven by MW::Bidding from uTrade on* overrides
    // Each returns an OrderState snapshot as it stands after the transition.
    // =========================================================================

    // ── onNewConfirmed: PENDING_NEW → OPEN ────────────────────────────────────
    OrderState onNewConfirmed(long clOrdId,
                              const std::string& exchangeOrderId = "") {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _validateTransition(s.status, OrderStatus::OPEN, s.handle);

        s.status          = OrderStatus::OPEN;
        s.exchangeOrderId = exchangeOrderId;
        s.lastUpdateNs    = _nowNs();
        return s;
    }

    // ── onNewRejected: PENDING_NEW → REJECTED ─────────────────────────────────
    OrderState onNewRejected(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _validateTransition(s.status, OrderStatus::REJECTED, s.handle);

        s.status       = OrderStatus::REJECTED;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onReplaceConfirmed: PENDING_MODIFY → OPEN ─────────────────────────────
    OrderState onReplaceConfirmed(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _validateTransition(s.status, OrderStatus::OPEN, s.handle);

        s.status       = OrderStatus::OPEN;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onReplaceRejected: PENDING_MODIFY → OPEN / PARTIALLY_FILLED ──────────
    OrderState onReplaceRejected(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        // Order reverts to whatever it was before the modify request
        s.status = (s.filledQty > 0)
                   ? OrderStatus::PARTIALLY_FILLED
                   : OrderStatus::OPEN;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onCancelled: PENDING_CANCEL → CANCELLED ───────────────────────────────
    OrderState onCancelled(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _validateTransition(s.status, OrderStatus::CANCELLED, s.handle);

        s.status       = OrderStatus::CANCELLED;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onCancelRejected: PENDING_CANCEL → OPEN / PARTIALLY_FILLED ───────────
    OrderState onCancelRejected(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        s.status = (s.filledQty > 0)
                   ? OrderStatus::PARTIALLY_FILLED
                   : OrderStatus::OPEN;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onPartialFill ─────────────────────────────────────────────────────────
    // Valid from OPEN, PENDING_MODIFY, PENDING_CANCEL (fill can race a cancel).
    OrderState onPartialFill(long clOrdId,
                             uint64_t fillQty,
                             int64_t  fillPrice,
                             const std::string& fillId = "") {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _applyFill(s, fillQty, fillPrice, fillId);
        s.status       = OrderStatus::PARTIALLY_FILLED;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onFilled → FILLED (terminal) ─────────────────────────────────────────
    OrderState onFilled(long clOrdId,
                        uint64_t fillQty,
                        int64_t  fillPrice,
                        const std::string& fillId = "") {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        _applyFill(s, fillQty, fillPrice, fillId);
        s.status       = OrderStatus::FILLED;
        s.pendingQty   = 0;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // ── onFrozen → FROZEN (terminal) ─────────────────────────────────────────
    OrderState onFrozen(long clOrdId) {
        std::lock_guard<std::mutex> lk(_mtx);

        auto& s = _getByClOrdId(clOrdId);
        s.status       = OrderStatus::FROZEN;
        s.lastUpdateNs = _nowNs();
        return s;
    }

    // =========================================================================
    // Auxiliary write
    // =========================================================================

    /// Store the risk status returned by the last req* call.
    void setRiskStatus(OrderHandle h, int rs) {
        std::lock_guard<std::mutex> lk(_mtx);
        _getRef(h).lastRiskStatus = rs;
    }

    // =========================================================================
    // Read-only queries — safe to call from any thread
    // =========================================================================

    /// Full snapshot of an order by handle.
    OrderState snapshot(OrderHandle h) const {
        std::lock_guard<std::mutex> lk(_mtx);
        return _getRefConst(h);
    }

    /// Just the status enum — cheaper than a full snapshot.
    OrderStatus statusOf(OrderHandle h) const {
        std::lock_guard<std::mutex> lk(_mtx);
        return _getRefConst(h).status;
    }

    /// True if a handle has ever been allocated.
    bool exists(OrderHandle h) const {
        std::lock_guard<std::mutex> lk(_mtx);
        return _orders.count(h) > 0;
    }

    /// All non-terminal handles for a given symbolId.
    std::vector<OrderHandle> activeHandlesForSymbol(uint64_t symbolId) const {
        std::lock_guard<std::mutex> lk(_mtx);
        std::vector<OrderHandle> result;
        for (const auto& kv : _orders)
            if (kv.second.symbolId == symbolId &&
                !isTerminal(kv.second.status))
                result.push_back(kv.first);
        return result;
    }

private:
    mutable std::mutex                          _mtx;
    uint64_t                                    _counter = 0;
    std::unordered_map<OrderHandle, OrderState> _orders;
    std::unordered_map<long, OrderHandle>       _byClOrdId;

    // ── Internal accessors (must be called with _mtx held) ───────────────────

    OrderState& _getRef(OrderHandle h) {
        auto it = _orders.find(h);
        if (it == _orders.end())
            throw std::out_of_range(
                "OrderStateStore: unknown handle " + std::to_string(h));
        return it->second;
    }

    const OrderState& _getRefConst(OrderHandle h) const {
        auto it = _orders.find(h);
        if (it == _orders.end())
            throw std::out_of_range(
                "OrderStateStore: unknown handle " + std::to_string(h));
        return it->second;
    }

    OrderState& _getByClOrdId(long clOrdId) {
        auto it = _byClOrdId.find(clOrdId);
        if (it == _byClOrdId.end())
            throw std::out_of_range(
                "OrderStateStore: unknown clOrdId " + std::to_string(clOrdId));
        return _getRef(it->second);
    }

    // ── State machine ─────────────────────────────────────────────────────────

    static bool _isLegalTransition(OrderStatus from, OrderStatus to) noexcept {
        switch (from) {
            case OrderStatus::CREATED:
                return to == OrderStatus::PENDING_NEW;

            case OrderStatus::PENDING_NEW:
                return to == OrderStatus::OPEN      ||
                       to == OrderStatus::REJECTED;

            case OrderStatus::OPEN:
                return to == OrderStatus::PENDING_MODIFY   ||
                       to == OrderStatus::PENDING_CANCEL   ||
                       to == OrderStatus::PARTIALLY_FILLED ||
                       to == OrderStatus::FILLED;

            case OrderStatus::PENDING_MODIFY:
                return to == OrderStatus::OPEN             ||
                       to == OrderStatus::PARTIALLY_FILLED ||  // fill races modify
                       to == OrderStatus::FILLED           ||
                       to == OrderStatus::PENDING_CANCEL;      // cancel after modify

            case OrderStatus::PENDING_CANCEL:
                return to == OrderStatus::CANCELLED        ||
                       to == OrderStatus::OPEN             ||  // cancel rejected
                       to == OrderStatus::PARTIALLY_FILLED ||  // fill races cancel
                       to == OrderStatus::FILLED;              // fill beats cancel

            case OrderStatus::PARTIALLY_FILLED:
                return to == OrderStatus::FILLED           ||
                       to == OrderStatus::PENDING_MODIFY   ||
                       to == OrderStatus::PENDING_CANCEL   ||
                       to == OrderStatus::CANCELLED;

            default:
                return false;   // terminal states have no outgoing transitions
        }
    }

    void _validateTransition(OrderStatus from, OrderStatus to, OrderHandle h) {
        if (!_isLegalTransition(from, to)) {
            _logIllegal(h, from, to);
            throw std::logic_error(
                std::string("OrderStateStore: illegal transition ")
                + statusName(from) + " → " + statusName(to)
                + " for handle " + std::to_string(h));
        }
    }

    static void _logIllegal(OrderHandle h,
                            OrderStatus from, OrderStatus to) noexcept {
        std::cerr << "[MW][Store] ILLEGAL TRANSITION handle=" << h
                  << "  " << statusName(from)
                  << " → " << statusName(to) << "\n";
    }

    // ── Fill accumulation ─────────────────────────────────────────────────────

    static void _applyFill(OrderState& s,
                            uint64_t fillQty, int64_t fillPrice,
                            const std::string& fillId) {
        // Weighted-average fill price across all fills
        int64_t totalValue =
            s.avgFillPrice * static_cast<int64_t>(s.filledQty)
            + fillPrice    * static_cast<int64_t>(fillQty);

        s.filledQty    += fillQty;
        s.avgFillPrice  = (s.filledQty > 0)
                          ? (totalValue / static_cast<int64_t>(s.filledQty))
                          : 0;
        s.pendingQty    = (s.requestedQty > s.filledQty)
                          ? (s.requestedQty - s.filledQty)
                          : 0;

        Fill f;
        f.fillQty        = fillQty;
        f.fillPrice      = fillPrice;
        f.exchangeFillId = fillId;
        f.timestampNs    = _nowNs();
        s.fills.push_back(std::move(f));
    }

    // ── Timing ────────────────────────────────────────────────────────────────

    static int64_t _nowNs() noexcept {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();
    }
};

} // namespace MW
