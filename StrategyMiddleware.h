#pragma once
// =============================================================================
// StrategyMiddleware.h  (MW::Bidding)
//
// Single bridge between every derived strategy and the uTrade / exchange layer.
//
// Design rules
// ─────────────
//  • ALL order operations (new / replace / cancel) must go through the public
//    API here – never call reqNewSingleOrder etc. from a derived class directly.
//  • OrderWrappers are owned and grouped here.  A 5-leg strategy receives
//    group-0 (legs 0-2, "first triplet") and group-1 (legs 3-4, "second pair").
//  • A strategy registers its FrontEndParameters once; the middleware keeps a
//    strategyId → params map so any component can look them up.
//  • Derived classes override the on* extension points and
//    onCMDModifyStrategy / onMarketDataEvent as before.
// =============================================================================

#include <../includes/sgContext.h>
#include <../includes/sgApiParameters.h>
#include <../includes/sharedSingleOrder.h>
#include <../includes/apiConstants.h>
#include <../includes/userParamsReader.h>
#include <../includes/baseCommands.h>
#include <../includes/api2UserCommands.h>
#include <../src/common/common.h>
#include <../common/orderWrapper.h>          // API2::COMMON::OrderWrapper

#include <string>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <functional>

namespace MW {

// ─────────────────────────────────────────────────────────────────────────────
// Leg descriptor – everything the middleware needs to create one OrderWrapper.
// ─────────────────────────────────────────────────────────────────────────────
struct LegSpec {
    SIGNED_LONG          symbolId   = 0;
    char                 orderMode  = 0;   // API2 order-mode enum value
    API2::AccountDetail  account;
};

// ─────────────────────────────────────────────────────────────────────────────
// Front-end parameter block stored per strategy.
// Extend fields as required; the strategy writes this once and the middleware
// stores it.
// ─────────────────────────────────────────────────────────────────────────────
struct StrategyFrontEndParams {
    // ── legs (up to 5 supported; unused legs left at symbolId = 0) ──────────
    SIGNED_LONG symbolId[5]   = {};
    char        orderMode[5]  = {};
    API2::AccountDetail account;

    // ── spread / price parameters ───────────────────────────────────────────
    SIGNED_LONG priceDifference     = 0;
    SIGNED_LONG flipPriceDifference = 0;
    SIGNED_LONG forwardSpread       = 0;
    SIGNED_LONG reverseSpread       = 0;
    SIGNED_LONG Difference          = 0;
    SIGNED_LONG Quantity            = 0;
    SIGNED_LONG OrderSize           = 0;   // SOL
    SIGNED_LONG AchievedPrice       = 0;
    SIGNED_LONG CurrentSpread       = 0;
    SIGNED_LONG FilledQty           = 0;
    SIGNED_LONG Timer               = 0;

    // ── control flags ───────────────────────────────────────────────────────
    bool        opportunity_check   = false;
    bool        L1                  = false;
    bool        L4                  = false;

    SIGNED_INTEGER strategyId       = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Wrapper group – a named collection of OrderWrappers for a set of legs.
// The middleware builds one group per "triplet" or "pair" of legs.
// ─────────────────────────────────────────────────────────────────────────────
struct WrapperGroup {
    std::string                            name;         // e.g. "triplet", "pair"
    std::vector<API2::COMMON::OrderWrapper> wrappers;    // one per leg in this group
    std::vector<SIGNED_LONG>               symbolIds;   // parallel to wrappers

    WrapperGroup() = default;
    explicit WrapperGroup(const std::string& n) : name(n) {}

    std::size_t size()  const { return wrappers.size(); }
    bool        empty() const { return wrappers.empty(); }

    // Convenience: is any wrapper in this group currently open?
    bool anyOpen() const {
        for (API2::COMMON::OrderWrapper w : wrappers)
            if (w.isOrderOpen()) return true;
        return false;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// MW::Bidding
// ─────────────────────────────────────────────────────────────────────────────
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
    // FrontEndParameters registry
    // Derived strategy calls registerParams() once (usually in constructor).
    // Any component can call getParams() by strategyId.
    // =========================================================================
    void registerParams(SIGNED_INTEGER strategyId,
                        const StrategyFrontEndParams& p)
    {
        std::lock_guard<std::mutex> lk(_paramsMtx);
        _paramsMap[strategyId] = p;
    }

    // Returns nullptr if not found.
    const StrategyFrontEndParams* getParams(SIGNED_INTEGER strategyId) const {
        std::lock_guard<std::mutex> lk(_paramsMtx);
        auto it = _paramsMap.find(strategyId);
        return (it != _paramsMap.end()) ? &it->second : nullptr;
    }

    // Update a single field without replacing the whole struct.
    void updateParams(SIGNED_INTEGER strategyId,
                      const StrategyFrontEndParams& updated)
    {
        std::lock_guard<std::mutex> lk(_paramsMtx);
        _paramsMap[strategyId] = updated;
    }

    // =========================================================================
    // Instrument / wrapper registration
    //
    // Call buildWrapperGroups() once during strategy initialisation AFTER all
    // instruments have been created via createNewInstrument().
    //
    // legs[0..2] → group index 0  ("triplet")
    // legs[3..4] → group index 1  ("pair")      – only if numLegs == 5
    //
    // Returns false if any instrument could not be found.
    // =========================================================================
    bool buildWrapperGroups(const std::vector<LegSpec>& legs,
                            const std::vector<API2::COMMON::Instrument*>& instruments)
    {
        if (legs.size() != instruments.size()) {
            std::cerr << "[MW] buildWrapperGroups: legs/instruments size mismatch\n";
            return false;
        }
        std::size_t n = legs.size();
        if (n != 3 && n != 5) {
            std::cerr << "[MW] buildWrapperGroups: only 3 or 5 legs supported, got " << n << "\n";
            return false;
        }

        _wrapperGroups.clear();
        _instruments = instruments;   // take ownership of the pointers

        // ── group 0: first three legs ("triplet") ────────────────────────────
        {
            WrapperGroup g("triplet");
            for (std::size_t i = 0; i < 3; ++i) {
                if (!instruments[i]) {
                    std::cerr << "[MW] buildWrapperGroups: null instrument at index " << i << "\n";
                    return false;
                }
                g.wrappers.emplace_back(
                    instruments[i],
                    static_cast<int>(legs[i].orderMode),
                    this,
                    legs[i].account
                );
                g.symbolIds.push_back(legs[i].symbolId);
            }
            _wrapperGroups.push_back(std::move(g));
        }

        // ── group 1: remaining two legs ("pair") ─────────────────────────────
        if (n == 5) {
            WrapperGroup g("pair");
            for (std::size_t i = 3; i < 5; ++i) {
                if (!instruments[i]) {
                    std::cerr << "[MW] buildWrapperGroups: null instrument at index " << i << "\n";
                    return false;
                }
                g.wrappers.emplace_back(
                    instruments[i],
                    static_cast<int>(legs[i].orderMode),
                    this,
                    legs[i].account
                );
                g.symbolIds.push_back(legs[i].symbolId);
            }
            _wrapperGroups.push_back(std::move(g));
        }

        return true;
    }

    // =========================================================================
    // Accessors for wrapper groups / individual wrappers
    // =========================================================================

    // Number of groups (1 for 3-leg, 2 for 5-leg).
    std::size_t groupCount() const { return _wrapperGroups.size(); }

    // Access a whole group (bounds-checked).
    WrapperGroup& group(std::size_t idx) {
        if (idx >= _wrapperGroups.size())
            throw std::out_of_range("[MW] group index out of range");
        return _wrapperGroups[idx];
    }
    const WrapperGroup& group(std::size_t idx) const {
        if (idx >= _wrapperGroups.size())
            throw std::out_of_range("[MW] group index out of range");
        return _wrapperGroups[idx];
    }

    // Flat access: wrapper(0)=leg0, wrapper(1)=leg1, … wrapper(4)=leg4.
    API2::COMMON::OrderWrapper& wrapper(std::size_t legIdx) {
        return _flatWrapper(legIdx);
    }
    const API2::COMMON::OrderWrapper& wrapper(std::size_t legIdx) const {
        return _flatWrapperConst(legIdx);
    }

    // Instrument pointer for a given leg index.
    API2::COMMON::Instrument* instrument(std::size_t legIdx) const {
        if (legIdx >= _instruments.size()) return nullptr;
        return _instruments[legIdx];
    }

    // Total number of legs registered.
    std::size_t legCount() const { return _instruments.size(); }

    // =========================================================================
    // Order operations (all go through the OrderWrapper layer)
    // =========================================================================

    // ── new order ─────────────────────────────────────────────────────────────
    bool placeNewOrder(std::size_t            legIdx,
                       API2::DATA_TYPES::RiskStatus& riskStatus,
                       SIGNED_LONG            price,
                       SIGNED_LONG            qty,
                       SIGNED_LONG            triggerPrice = 0)
    {
        auto& w = _flatWrapper(legIdx);
        w.reset();
        bool ok = w.newOrder(riskStatus, price, qty,
                             triggerPrice ? triggerPrice : price);
        if (!ok)
            std::cerr << "[MW] placeNewOrder: leg " << legIdx
                      << " newOrder failed, riskStatus=" << riskStatus << "\n";
        return ok;
    }

    // ── replace order ─────────────────────────────────────────────────────────
    bool modifyOrder(std::size_t            legIdx,
                     API2::DATA_TYPES::RiskStatus& riskStatus,
                     SIGNED_LONG            newPrice,
                     SIGNED_LONG            newQty,
                     SIGNED_LONG            newTriggerPrice = 0)
    {
        auto& w = _flatWrapper(legIdx);
        if (!w.isOrderOpen()) {
            std::cerr << "[MW] modifyOrder: leg " << legIdx << " order not open\n";
            return false;
        }
        bool ok = w.replaceOrder(riskStatus, newPrice, newQty,
                                 newTriggerPrice ? newTriggerPrice : newPrice);
        if (!ok)
            std::cerr << "[MW] modifyOrder: leg " << legIdx
                      << " replaceOrder failed, riskStatus=" << riskStatus << "\n";
        return ok;
    }

    // ── cancel order ──────────────────────────────────────────────────────────
    bool cancelLegOrder(std::size_t            legIdx,
                        API2::DATA_TYPES::RiskStatus& riskStatus)
    {
        auto& w = _flatWrapper(legIdx);
        if (!w.isOrderOpen()) {
            std::cerr << "[MW] cancelLegOrder: leg " << legIdx << " order not open\n";
            return false;
        }
        bool ok = w.cancelOrder(riskStatus);
        if (!ok)
            std::cerr << "[MW] cancelLegOrder: leg " << legIdx
                      << " cancelOrder failed, riskStatus=" << riskStatus << "\n";
        return ok;
    }

    // ── convenience: cancel every open order in a group ───────────────────────
    void cancelGroup(std::size_t                  groupIdx,
                     API2::DATA_TYPES::RiskStatus& riskStatus)
    {
        if (groupIdx >= _wrapperGroups.size()) return;
        auto& g = _wrapperGroups[groupIdx];
        for (auto& w : g.wrappers)
            if (w.isOrderOpen()) w.cancelOrder(riskStatus);
    }

    // ── process a confirmation through the correct wrapper ────────────────────
    // Returns true if the wrapper accepted the confirmation.
    bool routeConfirmation(API2::OrderConfirmation& conf,
                           API2::COMMON::OrderId*   orderId)
    {
        reqQryDebugLog()->saveConfirmation(conf);
        for (auto& g : _wrapperGroups)
            for (auto& w : g.wrappers)
                if (w._orderId == orderId)
                    return w.processConfirmation(conf);
        return false;
    }

    // ── find which leg index owns an orderId ─────────────────────────────────
    // Returns -1 if not found.
    int legIndexOf(API2::COMMON::OrderId* orderId) const {
        std::size_t flat = 0;
        for (const auto& g : _wrapperGroups)
            for (std::size_t j = 0; j < g.wrappers.size(); ++j, ++flat)
                if (g.wrappers[j]._orderId == orderId)
                    return static_cast<int>(flat);
        return -1;
    }

    // ── find which group index owns an orderId ────────────────────────────────
    // Returns -1 if not found.
    int groupIndexOf(API2::COMMON::OrderId* orderId) const {
        for (std::size_t gi = 0; gi < _wrapperGroups.size(); ++gi)
            for (const auto& w : _wrapperGroups[gi].wrappers)
                if (w._orderId == orderId)
                    return static_cast<int>(gi);
        return -1;
    }

    // =========================================================================
    // uTrade confirmation callbacks — route to wrapper, then call strategy hook
    // =========================================================================

    void onProcessOrderConfirmation(API2::OrderConfirmation& conf) override {
        // Optional raw-log hook; override in derived class if needed.
    }

    void onConfirmed(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderNewConfirmed(conf, orderId);
    }

    void onNewReject(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderRejected(conf, orderId);
    }

    void onReplaced(API2::OrderConfirmation& conf,
                    API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderReplaced(conf, orderId);
    }

    void onReplaceRejected(API2::OrderConfirmation& conf,
                           API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderReplaceRejected(conf, orderId);
    }

    void onCanceled(API2::OrderConfirmation& conf,
                    API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderCancelled(conf, orderId);
    }

    void onIOCCanceled(API2::OrderConfirmation& conf,
                       API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderCancelled(conf, orderId);
    }

    void onCancelRejected(API2::OrderConfirmation& conf,
                          API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderCancelRejected(conf, orderId);
    }

    void onPartialFill(API2::OrderConfirmation& conf,
                       API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderPartialFill(conf, orderId);
    }

    void onFilled(API2::OrderConfirmation& conf,
                  API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderFilled(conf, orderId);
    }

    void onFrozen(API2::OrderConfirmation& conf,
                  API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderFrozen(conf, orderId);
    }

    void onMarketToLimit(API2::OrderConfirmation& conf,
                         API2::COMMON::OrderId*   orderId) override
    {
        routeConfirmation(conf, orderId);
        onOrderMarketToLimit(conf, orderId);
    }

    void onRmsReject(API2::OrderConfirmation& conf,
                     API2::COMMON::OrderId*   orderId) override
    {
        reqQryDebugLog()->saveConfirmation(conf);
        // Route to correct wrapper manually (rmsReject doesn't go via normal processConfirmation)
        for (auto& g : _wrapperGroups)
            for (auto& w : g.wrappers)
                if (w._orderId == orderId) { w.processConfirmation(conf); break; }
        onOrderRmsReject(conf, orderId);
    }

    // =========================================================================
    // Extension points – override in derived strategy class.
    // Each receives the raw confirmation + orderId so the strategy can
    // still call legIndexOf() / groupIndexOf() to know which leg fired.
    // =========================================================================
    virtual void onOrderNewConfirmed   (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderRejected       (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderReplaced       (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderReplaceRejected(API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderCancelled      (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderCancelRejected (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderPartialFill    (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderFilled         (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderFrozen         (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderMarketToLimit  (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}
    virtual void onOrderRmsReject      (API2::OrderConfirmation&, API2::COMMON::OrderId*) {}

protected:
    // Instruments parallel to all wrappers across all groups (flat order).
    std::vector<API2::COMMON::Instrument*> _instruments;

    // Wrapper groups: index 0 = triplet (legs 0-2), index 1 = pair (legs 3-4).
    std::vector<WrapperGroup> _wrapperGroups;

private:
    mutable std::mutex                                       _paramsMtx;
    std::unordered_map<SIGNED_INTEGER, StrategyFrontEndParams> _paramsMap;

    // ── flat wrapper access helpers ──────────────────────────────────────────
    API2::COMMON::OrderWrapper& _flatWrapper(std::size_t legIdx) {
        std::size_t idx = legIdx;
        for (auto& g : _wrapperGroups) {
            if (idx < g.wrappers.size()) return g.wrappers[idx];
            idx -= g.wrappers.size();
        }
        throw std::out_of_range("[MW] leg index " + std::to_string(legIdx) + " out of range");
    }
    const API2::COMMON::OrderWrapper& _flatWrapperConst(std::size_t legIdx) const {
        std::size_t idx = legIdx;
        for (const auto& g : _wrapperGroups) {
            if (idx < g.wrappers.size()) return g.wrappers[idx];
            idx -= g.wrappers.size();
        }
        throw std::out_of_range("[MW] leg index " + std::to_string(legIdx) + " out of range");
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Convenience leg-index constants for 3-leg and 5-leg strategies
// ─────────────────────────────────────────────────────────────────────────────
namespace LegIndex {
    // 3-leg (group 0)
    constexpr std::size_t LEG_FUTURE  = 0;
    constexpr std::size_t LEG_CALL    = 1;
    constexpr std::size_t LEG_PUT     = 2;
    // 5-leg (group 1, flat indices 3-4)
    constexpr std::size_t LEG_EXTRA_1 = 3;
    constexpr std::size_t LEG_EXTRA_2 = 4;
}

} // namespace MW
