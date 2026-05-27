#pragma once
#ifndef ALGO_H
#define ALGO_H
// =============================================================================
// threelegBidding.h  (SBA::TLS)
//
// Three-leg Conversion/Reversal bidding strategy.
// Inherits MW::Bidding — uses its pool, order, and market-data APIs.
//
// Pool scheme (strategy-defined keys):
//   POOL_FUT   — single-leg future wrapper  (passive L1 bids)
//   POOL_CALL  — single-leg call wrapper    (cover leg)
//   POOL_PUT   — single-leg put wrapper     (cover leg)
// =============================================================================

#include "StrategyMiddleware.h"
#include <../includes/api2Exceptions.h>
#include <../includes/apiDataTypes.h>

#include <string>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <boost/unordered_map.hpp>

#define INVOKING_API 0
#if INVOKING_API
#include <invokingApi.h>
#endif

using namespace API2;

namespace SBA {

struct FrontEndParameters {
    SIGNED_LONG symbolIdFirstLeg   = 0;
    SIGNED_LONG symbolIdSecondLeg  = 0;
    SIGNED_LONG symbolIdThirdLeg   = 0;
    SIGNED_LONG forwardSpread      = 0;
    SIGNED_LONG reverseSpread      = 0;
    SIGNED_LONG CurrentSpread      = 0;
    SIGNED_LONG Difference         = 0;
    SIGNED_LONG SOL                = 0;
    SIGNED_LONG maxLots            = 0;
    SIGNED_LONG AchievedSpread     = 0;
    SIGNED_LONG Timer              = 0;
    bool        opportunity_check  = false;
    char        firstLegOrderMode  = 0;
    char        secondLegOrderMode = 0;
    char        thirdLegOrderMode  = 0;
    API2::AccountDetail account;
    SIGNED_INTEGER      strategyId = 0;
};

class TLS : public MW::Bidding {
public:
    // Pool keys — strategy-defined, arbitrary uint64_t constants.
    // Named so call sites read clearly without needing comments.
    static constexpr uint64_t POOL_FUT  = 1;
    static constexpr uint64_t POOL_CALL = 2;
    static constexpr uint64_t POOL_PUT  = 3;

    explicit TLS(API2::StrategyParameters* params);
    static void bidDriver(void* params);

    // ── SGContext callbacks ───────────────────────────────────────────────────
    void onTimerEvent()   override;
    void onDefaultEvent() override;
    void onCMDModifyStrategy(API2::AbstractUserParams* newParams) override;
    void onCMDTerminateStartegy();

    // ── MW::Bidding order hooks ───────────────────────────────────────────────
    void onOrderConfirmed      (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderRejected       (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderReplaced       (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderReplaceRejected(API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderCancelled      (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderCancelRejected (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderPartialFill    (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderFilled         (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;
    void onOrderRmsReject      (API2::OrderConfirmation&, API2::COMMON::OrderId*, MW::OrderHandle) override;

    // ── MW::Bidding market data hook ──────────────────────────────────────────
    void onMarketUpdate(SIGNED_LONG symbolId) override;

private:
    FrontEndParameters _params;
    FrontEndParameters _modParams;

    API2::COMMON::MktData* _mkData[3] = {};

    // Active order handles
    MW::OrderHandle _futHandle  = MW::INVALID_ORDER_HANDLE;
    MW::OrderHandle _callHandle = MW::INVALID_ORDER_HANDLE;
    MW::OrderHandle _putHandle  = MW::INVALID_ORDER_HANDLE;

    // Prices
    SIGNED_LONG _futAskPrice = 0, _futBidPrice = 0;
    SIGNED_LONG _futAskPrice_1 = 0, _futBidPrice_1 = 0;
    SIGNED_LONG _callAskPrice = 0, _callBidPrice = 0;
    SIGNED_LONG _putAskPrice  = 0, _putBidPrice  = 0;
    SIGNED_LONG _currentSpread = 0, _currSpread = 0;
    SIGNED_LONG _secondLegPrice = 0, _thirdLegPrice = 0;
    long        _strikePrice    = 0;

    // Fill accounting
    SIGNED_LONG _traded = 0, _tradedQ = 0;
    SIGNED_LONG _secondTradedQ = 0, _thirdTradedQ = 0;
    SIGNED_LONG _qty = 0, _lastPlaceQty = 0;
    SIGNED_LONG _lastQuantityBidLegL1 = 0;
    SIGNED_LONG _lastQuantitySecondLeg = 0;
    SIGNED_LONG _lastQuantityThirdLeg  = 0;
    SIGNED_LONG _lastTriggerPrice = 0;

    // Achieved-spread accumulators
    SIGNED_LONG _v1Fut = 0, _q1Fut = 0;
    SIGNED_LONG _v2Call = 0, _q2Call = 0;
    SIGNED_LONG _v3Put  = 0, _q3Put  = 0;
    SIGNED_LONG _priceCall = 0, _priceFut = 0, _pricePut = 0;

    // Depth / counter state
    SIGNED_LONG _second_leg_depth = 0, _third_leg_depth = 0;
    SIGNED_LONG _second_leg_counter = 0, _third_leg_counter = 0;

    // Flags
    bool _conFlag = false, _modify = false, _terminateCheck = false;
    bool _placeSecondOrder = true, _placeAggressive = false;
    bool _secondLegTrade = false, _firstLegTrade = false;
    bool _firstLegOrderFlag = false;
    bool _secondLegOrderFlag = false, _thirdLegOrderFlag = false;
    bool _secondLegTimerIdentifier = false, _thirdLegTimerIdentifier = false;

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool        setInternalParameters(API2::UserParams*, FrontEndParameters&);
    void        validateParameters();
    void        buildPools();
    void        mapModifiedParameters();
    void        refreshMarketData();
    void        calculatePrice();
    void        placeL1Order();
    void        placeL1ModOrder();
    void        placeSecondLegOrder();
    void        placeThirdLegOrder();
    void        placeCoverLegOrder();
    void        checkCoverLegOrderStatus();
    void        achievedSpreadCal(API2::OrderConfirmation&);
    void        terminateStrategyComment(API2::DATA_TYPES::StrategyComment);
    void        dump(const FrontEndParameters&);
    std::string getTodayDate();

#if INVOKING_API
    INVOKING::InvokingApi*     invokingApi   = nullptr;
    INVOKING::TwoLegArbitrage* cashFutParams = nullptr;
    int _child1 = 0;
    void runCashFut();
    void printChildPosition(API2::DATA_TYPES::STRATEGY_ID);
#endif
};

} // namespace SBA
#endif // ALGO_H
