#pragma once
// =============================================================================
// OrderDefs.h
//
// Plain-data types shared between MW::Bidding (middleware) and strategies.
// No uTrade / SGContext dependencies — safe to include everywhere.
//
// Sections
// ─────────
//  1. Handle types          (OrderHandle)
//  2. OrderStatus enum      (IDLE through FROZEN)
//  3. Status predicates     (isTerminal, isPending, isLive, statusName)
//  4. Fill                  (one execution report)
//  5. NewOrderRequest       (strategy → store, before wire call)
//  6. OrderState            (full snapshot delivered in callbacks / queries)
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

// SIGNED_LONG and SIGNED_INTEGER are defined by the uTrade SDK headers
// (pulled in transitively via sgContext.h / apiDataTypes.h).
// Do NOT redefine them here — the SDK typedef and a using-declaration for the
// same name are conflicting declarations even when guarded with #ifndef.

namespace MW {

// =============================================================================
// 1. Handle types
// =============================================================================

/// Opaque, monotonically-increasing handle for one placed order.
/// Allocated by OrderStateStore::allocate(); never reused, never 0.
using OrderHandle = uint64_t;
static constexpr OrderHandle INVALID_ORDER_HANDLE = 0;

// =============================================================================
// 2. OrderStatus
// =============================================================================
enum class OrderStatus : uint8_t {
    IDLE             = 0,  // no order placed on this handle yet (pre-allocation)
    CREATED          = 1,  // handle allocated; wire call not yet made
    PENDING_NEW      = 2,  // newOrder sent to exchange; awaiting ack
    OPEN             = 3,  // exchange confirmed the order
    PENDING_MODIFY   = 4,  // replaceOrder sent; awaiting ack
    PENDING_CANCEL   = 5,  // cancelOrder sent; awaiting ack
    PARTIALLY_FILLED = 6,  // one or more fills; still open on exchange
    FILLED           = 7,  // fully filled — terminal
    CANCELLED        = 8,  // cancelled (us or IOC expiry) — terminal
    REJECTED         = 9,  // exchange / risk rejected — terminal
    CANCEL_REJECTED  = 10, // cancel was rejected; order reverts to OPEN/PARTIAL
    REPLACE_REJECTED = 11, // replace was rejected; order reverts to OPEN/PARTIAL
    FROZEN           = 12, // exchange froze the order — terminal
};

// =============================================================================
// 3. Status predicates
// =============================================================================

inline const char* statusName(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::IDLE:             return "IDLE";
        case OrderStatus::CREATED:          return "CREATED";
        case OrderStatus::PENDING_NEW:      return "PENDING_NEW";
        case OrderStatus::OPEN:             return "OPEN";
        case OrderStatus::PENDING_MODIFY:   return "PENDING_MODIFY";
        case OrderStatus::PENDING_CANCEL:   return "PENDING_CANCEL";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
        case OrderStatus::CANCEL_REJECTED:  return "CANCEL_REJECTED";
        case OrderStatus::REPLACE_REJECTED: return "REPLACE_REJECTED";
        case OrderStatus::FROZEN:           return "FROZEN";
        default:                            return "UNKNOWN";
    }
}

/// Order has reached a terminal state and will never change again.
inline bool isTerminal(OrderStatus s) noexcept {
    return s == OrderStatus::FILLED    ||
           s == OrderStatus::CANCELLED ||
           s == OrderStatus::REJECTED  ||
           s == OrderStatus::FROZEN;
}

/// A req* call has been sent to the exchange; we are waiting for its ack.
/// The strategy must not issue further actions on this order during this window.
inline bool isPending(OrderStatus s) noexcept {
    return s == OrderStatus::PENDING_NEW    ||
           s == OrderStatus::PENDING_MODIFY ||
           s == OrderStatus::PENDING_CANCEL;
}

/// Order currently occupies a live slot on the exchange.
/// Used by the middleware to enforce the per-strategy order cap.
inline bool isLive(OrderStatus s) noexcept {
    return s == OrderStatus::PENDING_NEW     ||
           s == OrderStatus::OPEN            ||
           s == OrderStatus::PENDING_MODIFY  ||
           s == OrderStatus::PENDING_CANCEL  ||
           s == OrderStatus::PARTIALLY_FILLED;
    // IDLE / CREATED           — never reached the exchange
    // FILLED / CANCELLED /
    //   REJECTED / FROZEN      — terminal, slot freed
    // CANCEL_REJECTED /
    //   REPLACE_REJECTED       — order reverted to OPEN/PARTIAL (caught above)
}

// =============================================================================
// 4. Fill — one execution report
// =============================================================================
struct Fill {
    uint64_t    fillQty        = 0;
    int64_t     fillPrice      = 0;    // exchange ticks; same unit as order price
    std::string exchangeFillId;        // exchange-assigned fill id (may be empty)
    int64_t     timestampNs    = 0;    // steady_clock nanoseconds at time of fill
};

// =============================================================================
// 5. NewOrderRequest
//
// Built by MW::Bidding::placeOrder() before calling the wire layer.
// Passed to OrderStateStore::allocate() so the store can initialise the
// OrderState without needing to know anything about uTrade internals.
// =============================================================================
struct NewOrderRequest {
    uint64_t    symbolId   = 0;
    int64_t     price      = 0;
    int64_t     stopPrice  = 0;    // trigger / stop price; 0 = same as price
    uint64_t    qty        = 0;
    std::string tag;               // arbitrary label set by strategy ("L1", "cover", …)
};

// =============================================================================
// 6. OrderState
//
// Authoritative snapshot of one order's full lifecycle.
// Delivered to strategy callbacks (onOrderFilled etc.) and available at any
// time via MW::Bidding::orderState(handle).
//
// The middleware updates this automatically on every confirmation event;
// strategies read it but never write to it.
// =============================================================================
struct OrderState {
    // ── Identity ──────────────────────────────────────────────────────────────
    OrderHandle  handle        = INVALID_ORDER_HANDLE;
    OrderStatus  status        = OrderStatus::IDLE;

    // ── Symbol / order description ────────────────────────────────────────────
    uint64_t     symbolId      = 0;
    std::string  tag;              // copied from NewOrderRequest::tag

    // ── Quantities ────────────────────────────────────────────────────────────
    uint64_t     requestedQty  = 0;
    uint64_t     filledQty     = 0;
    uint64_t     pendingQty    = 0;    // requestedQty − filledQty

    // ── Prices ────────────────────────────────────────────────────────────────
    int64_t      orderPrice    = 0;    // last submitted limit price
    int64_t      avgFillPrice  = 0;    // weighted average across all fills

    // ── Exchange identifiers ──────────────────────────────────────────────────
    long         clOrdId       = 0;    // uTrade internal client order id
    std::string  exchangeOrderId;      // exchange-assigned order id (post-confirm)
    int          lastRiskStatus = 0;   // risk status from the last req* call

    // ── Fill history ──────────────────────────────────────────────────────────
    std::vector<Fill> fills;           // one entry per partial/full fill

    // ── Timing ───────────────────────────────────────────────────────────────
    int64_t      createdNs     = 0;    // steady_clock ns when handle was allocated
    int64_t      lastUpdateNs  = 0;    // steady_clock ns of most recent state change
};

} // namespace MW
