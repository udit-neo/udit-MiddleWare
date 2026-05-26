#pragma once
// =============================================================================
// threelegBidding.h  (SBA::TLS)
//
// Three-leg (Conversion / Reversal) bidding strategy.
//
// Inheritance chain:
//   SBA::TLS  →  MW::Bidding  →  API2::SGContext
//
// All order operations go through MW::Bidding::placeNewOrder /
// modifyOrder / cancelLegOrder.  No uTrade API calls are made here.
//
// Leg layout (flat indices used with wrapper() / placeNewOrder etc.):
//   0 = Future   (group 0, triplet)
//   1 = Call     (group 0, triplet)
//   2 = Put      (group 0, triplet)
// =============================================================================

#ifndef ALGO_H
#define ALGO_H

#include "StrategyMiddleware.h"
#include <../includes/api2Exceptions.h>
#include <../includes/apiDataTypes.h>

#include <iostream>
#include <string>
#include <deque>
#include <chrono>
#include <thread>
#include <unordered_map>

#include <boost/unordered_map.hpp>

// Optional: invoking-API support (set to 1 to enable)
#define INVOKING_API  0
#define SEND_POPUP    0
#define SEND_CUSTOM_DATA 0

#if INVOKING_API
#include <invokingApi.h>
#endif

using namespace API2;

namespace SBA {

// ─────────────────────────────────────────────────────────────────────────────
// Strategy-local front-end parameter block.
// Mirrors MW::StrategyFrontEndParams but keeps legacy field names so that
// existing FILL_PARAMS / dump calls compile without change.
// ─────────────────────────────────────────────────────────────────────────────
struct FrontEndParameters {
    SIGNED_LONG symbolIdFirstLeg   = 0;
    SIGNED_LONG symbolIdSecondLeg  = 0;
    SIGNED_LONG symbolIdThirdLeg   = 0;

    SIGNED_LONG forwardSpread      = 0;
    SIGNED_LONG reverseSpread      = 0;
    SIGNED_LONG CurrentSpread      = 0;
    SIGNED_LONG Difference         = 0;
    SIGNED_LONG SOL                = 0;   // size of lot
    SIGNED_LONG maxLots            = 0;
    SIGNED_LONG AchievedSpread     = 0;
    SIGNED_LONG FilledQty          = 0;
    SIGNED_LONG Timer              = 0;

    bool        opportunity_check  = false;

    char        firstLegOrderMode  = 0;
    char        secondLegOrderMode = 0;
    char        thirdLegOrderMode  = 0;

    API2::AccountDetail account;
    SIGNED_INTEGER      strategyId = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// SBA::TLS – the actual strategy class
// ─────────────────────────────────────────────────────────────────────────────
class TLS : public MW::Bidding {
public:
    // ── Construction / registration ──────────────────────────────────────────
    explicit TLS(API2::StrategyParameters* params);

    // Static driver function passed to reqStartAlgo.
    static void bidDriver(void* params);

    // =========================================================================
    // SGContext callbacks (override MW::Bidding extension points)
    // =========================================================================
    void onTimerEvent()  override;
    void onDefaultEvent() override;
    void onMarketDataEvent(UNSIGNED_LONG symbolId) override;
    void onCMDModifyStrategy(API2::AbstractUserParams* newParams) override;
    void onCMDTerminateStartegy();

    // ── Order-event extension points (from MW::Bidding) ──────────────────────
    void onOrderNewConfirmed   (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderRejected       (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderReplaced       (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderReplaceRejected(API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderCancelled      (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderCancelRejected (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderPartialFill    (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderFilled         (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;
    void onOrderRmsReject      (API2::OrderConfirmation&, API2::COMMON::OrderId*) override;

private:
    // ── Strategy state ───────────────────────────────────────────────────────
    FrontEndParameters _params;
    FrontEndParameters _modParams;

    // Market-data cache
    API2::COMMON::MktData* _mkData[3] = {};   // indexed by leg: 0=fut,1=call,2=put
    API2::COMMON::MktData* _currMkData = nullptr;

    // Timestamp dedup map (symbolId → last seen timestamp)
    std::unordered_map<SIGNED_LONG, UNSIGNED_LONG> _symbolToMkdataTs;

    // Calculated prices
    SIGNED_LONG _futAskPrice   = 0;
    SIGNED_LONG _futBidPrice   = 0;
    SIGNED_LONG _futAskPrice_1 = 0;  // raw best-ask from market
    SIGNED_LONG _futBidPrice_1 = 0;  // raw best-bid from market
    SIGNED_LONG _callAskPrice  = 0;
    SIGNED_LONG _callBidPrice  = 0;
    SIGNED_LONG _putAskPrice   = 0;
    SIGNED_LONG _putBidPrice   = 0;
    SIGNED_LONG _currentSpread = 0;
    SIGNED_LONG _currSpread    = 0;
    SIGNED_LONG _secondLegPrice = 0;
    SIGNED_LONG _thirdLegPrice  = 0;
    SIGNED_LONG _strikeDifference = 0;
    long        _strikePrice    = 0;

    // Fill accounting
    SIGNED_LONG _traded         = 0;  // first-leg fills outstanding (waiting for 2+3)
    SIGNED_LONG _tradedQ        = 0;  // cumulative fully-hedged qty
    SIGNED_LONG _secondTradedQ  = 0;
    SIGNED_LONG _thirdTradedQ   = 0;
    SIGNED_LONG _qty            = 0;
    SIGNED_LONG _lastPlaceQty   = 0;
    SIGNED_LONG _lastQuantityBidLegL1 = 0;
    SIGNED_LONG _lastQuantitySecondLeg = 0;
    SIGNED_LONG _lastQuantityThirdLeg  = 0;
    SIGNED_LONG _lastPrice       = 0;
    SIGNED_LONG _lastTriggerPrice = 0;

    // Achieved-spread running totals (for avg calculation)
    SIGNED_LONG _v1Fut  = 0, _q1Fut  = 0;
    SIGNED_LONG _v2Call = 0, _q2Call = 0;
    SIGNED_LONG _v3Put  = 0, _q3Put  = 0;
    SIGNED_LONG _priceCall = 0, _priceFut = 0, _pricePut = 0;

    // Depth / counter state for passive cover legs
    SIGNED_LONG _second_leg_depth   = 0;
    SIGNED_LONG _third_leg_depth    = 0;
    SIGNED_LONG _second_leg_counter = 0;
    SIGNED_LONG _third_leg_counter  = 0;

    // Event log counter
    SIGNED_LONG _eventFlowLogID = 0;

    // Control flags
    bool _conFlag                  = false;  // true = conversion, false = reversal
    bool _modify                   = false;
    bool _terminateCheck           = false;
    bool _placeSecondOrder         = true;
    bool _placeAggressive          = false;
    bool _secondLegTrade           = false;
    bool _firstLegTrade            = false;
    bool _firstLegOrderFlag        = false;
    bool _secondLegOrderFlag       = false;
    bool _thirdLegOrderFlag        = false;
    bool _firstLegPartialOrderFlag = false;
    bool _secondLegTimerIdentifier = false;
    bool _thirdLegTimerIdentifier  = false;

    API2::DATA_TYPES::RiskStatus _riskStatus = 0;

#if INVOKING_API
    INVOKING::InvokingApi*      invokingApi   = nullptr;
    INVOKING::TwoLegArbitrage*  cashFutParams = nullptr;
    int _child1 = 0;
#endif

    // =========================================================================
    // Private helpers
    // =========================================================================

    // Initialise from front-end params; returns false on failure.
    bool setInternalParameters(API2::UserParams*   customParams,
                               FrontEndParameters& params);

    // Copy _modParams → _params after a successful modify.
    void mapModifiedParameters();

    // Validate instrument/mode combinations; throws on error.
    void validateParameters();

    // Populate _mkData[0..2] from API; call createNewInstrument for each leg.
    void registerSymbols();

    // Rebuild all MW::Bidding wrapper groups after instruments are ready.
    bool createWrapperGroups();

    // Update all _mkData pointers.
    void refreshMarketData();

    // Strategy main logic.
    void placeOrder();
    void placeL1ModOrder();
    void placeSecondLegOrder();
    void placeThirdLegOrder();
    void placeCoverLegOrder();
    void checkCoverLegOrderStatus();
    void placeL1Aggressive();
    void placeSecondLegOrderAggressive();

    // Spread / price calculation.
    void        calculatePrice();
    bool        calculateLimitPrice();
    void        achievedSpreadCal(API2::OrderConfirmation& conf);
    std::string getTodayDate();

    // Terminal helper.
    void terminateStrategyComment(API2::DATA_TYPES::StrategyComment comment);

    // Debug dump.
    void dump(const FrontEndParameters& params) ;

#if INVOKING_API
    void runCashFut();
    void printChildPosition(API2::DATA_TYPES::STRATEGY_ID childId);
#endif
};

} // namespace SBA

#endif // ALGO_H
