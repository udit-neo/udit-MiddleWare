// =============================================================================
// threelegBidding.cpp  (SBA::TLS)
//
// Strategy uses the general MW::Bidding pool API:
//   registerPool(key, size)    — once per pool in buildPools()
//   addToPool(key, wrapper)    — once per wrapper slot in buildPools()
//   placeOrder(key, ...)       — fires single-leg orders
//   modifyOrder(handle, ...)   — modifies an open order
//   cancelOrder(handle)        — cancels an open order
//   orderState(handle)         — queries authoritative state snapshot
//   isOrderOpen(handle)        — bool convenience
//
// Market data: onMarketDataEvent → dedup → onMarketUpdate → onDefaultEvent
// Order events: on* callbacks → store update → virtual hook
// =============================================================================

#include "threelegBidding.h"
#include "apiConstants.h"
#include <../includes/sgDebugLogDefines.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
SBA::TLS::TLS(API2::StrategyParameters* params)
    : MW::Bidding(params, "ConRevBiddingStrategy")
{
    auto* cp = static_cast<API2::UserParams*>(params->getInfo());

    if (!setInternalParameters(cp, _params)) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Parameters not set from front end");
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    try {
        validateParameters();
        buildPools();
    }
    catch (API2::MarketDataSubscriptionFailedException&) {
        DEBUG_MESSAGE(reqQryDebugLog(), "TBT subscription Failed");
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    catch (API2::InstrumentNotFoundException&) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Instrument Not Found");
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    catch (const std::exception& e) {
        DEBUG_MESSAGE(reqQryDebugLog(), std::string("Init error: ") + e.what());
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    _conFlag = (_params.forwardSpread != 0);

    auto* callInstr = createNewInstrument(_params.symbolIdSecondLeg, false, false, false);
    if (callInstr) _strikePrice = callInstr->getStaticData()->strikePrice;

    _algoIdSet.insert(
        createNewInstrument(_params.symbolIdFirstLeg, false, false, false)
            ->getStaticData()->marketId);
    readConfForAlgoid("SAMPLE_BIDDING", "SampleBidding.txt", "_ALGO_ID", true);

    _mkData[0] = reqQryUpdateMarketData(_params.symbolIdFirstLeg);
    _mkData[1] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[2] = reqQryUpdateMarketData(_params.symbolIdThirdLeg);

#if INVOKING_API
    runCashFut();
#endif
}

void SBA::TLS::bidDriver(void* params)
{
    auto* sgParams = static_cast<API2::StrategyParameters*>(params);
    boost::shared_ptr<API2::SGContext> obj(new SBA::TLS(sgParams));
    obj->reqStartAlgo(true, false);
    API2::SGContext::registerStrategy(obj);
    obj->reqTimerEvent(1000000);
}

// ─────────────────────────────────────────────────────────────────────────────
// buildPools
//
// The strategy fully controls wrapper construction here.
// MW::Bidding::registerPool() + addToPool() is all that's required.
// Any wrapper type (1-leg, 2-leg, 3-leg, basket) can be used.
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::buildPools()
{
    // Instruments with live market-data feed
    auto* instrFut  = createNewInstrument(_params.symbolIdFirstLeg,  true, false, true);
    auto* instrCall = createNewInstrument(_params.symbolIdSecondLeg, true, false, true);
    auto* instrPut  = createNewInstrument(_params.symbolIdThirdLeg,  true, false, true);

    if (!instrFut || !instrCall || !instrPut)
        throw API2::InstrumentNotFoundException();

    const int poolSize = 10;

    // ── POOL_FUT: single-leg future (passive L1 bids / mods) ─────────────────
    registerPool(POOL_FUT, poolSize);
    for (int i = 0; i < poolSize; ++i) {
        addToPool(POOL_FUT,
            API2::COMMON::OrderWrapper(instrFut,
                static_cast<int>(_params.firstLegOrderMode),
                this, _params.account));
    }

    // ── POOL_CALL: single-leg call (cover leg L2) ─────────────────────────────
    registerPool(POOL_CALL, poolSize);
    for (int i = 0; i < poolSize; ++i) {
        addToPool(POOL_CALL,
            API2::COMMON::OrderWrapper(instrCall,
                static_cast<int>(_params.secondLegOrderMode),
                this, _params.account));
    }

    // ── POOL_PUT: single-leg put (cover leg L3) ───────────────────────────────
    registerPool(POOL_PUT, poolSize);
    for (int i = 0; i < poolSize; ++i) {
        addToPool(POOL_PUT,
            API2::COMMON::OrderWrapper(instrPut,
                static_cast<int>(_params.thirdLegOrderMode),
                this, _params.account));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameter helpers
// ─────────────────────────────────────────────────────────────────────────────
bool SBA::TLS::setInternalParameters(API2::UserParams* customParams, FrontEndParameters& p)
{
    FILL_PARAMS("SYMBOL LEG1",   p.symbolIdFirstLeg);
    FILL_PARAMS("SYMBOL LEG2",   p.symbolIdSecondLeg);
    FILL_PARAMS("SYMBOL LEG3",   p.symbolIdThirdLeg);
    FILL_PARAMS("forwardSpread", p.forwardSpread);
    FILL_PARAMS("reverseSpread", p.reverseSpread);
    FILL_PARAMS("CurrentSpread", p.CurrentSpread);
    char opp = 0;
    FILL_PARAMS("opportunity_check", opp);
    p.opportunity_check = static_cast<bool>(opp);
    FILL_PARAMS("Difference",    p.Difference);
    FILL_PARAMS("SOL",           p.SOL);
    FILL_PARAMS("maxLots",       p.maxLots);
    FILL_PARAMS("Timer",         p.Timer);
    FILL_PARAMS("AchievedSpread",p.AchievedSpread);
    FILL_PARAMS("Acc Detail 1",  p.account);
    FILL_PARAMS("Order Mode 1",  p.firstLegOrderMode);
    FILL_PARAMS("Order Mode 2",  p.secondLegOrderMode);
    FILL_PARAMS("Order Mode 3",  p.thirdLegOrderMode);
    p.strategyId = customParams->getStrategyId();
    dump(p);
    return true;
}

void SBA::TLS::validateParameters()
{
    if (_params.forwardSpread == 0 && _params.reverseSpread == 0)
        throw std::runtime_error("Both spreads zero");
    if (_params.maxLots == 0 || _params.SOL == 0)
        throw std::runtime_error("maxLots/SOL zero");
    if (_params.opportunity_check) {
        if (_params.forwardSpread && _params.Difference > _params.forwardSpread)
            throw std::runtime_error("Difference > forwardSpread");
        if (_params.reverseSpread  && _params.Difference > _params.reverseSpread)
            throw std::runtime_error("Difference > reverseSpread");
    }
}

void SBA::TLS::mapModifiedParameters() { _params = _modParams; }

// ─────────────────────────────────────────────────────────────────────────────
// Market data
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::refreshMarketData()
{
    _mkData[0] = reqQryUpdateMarketData(_params.symbolIdFirstLeg);
    _mkData[1] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[2] = reqQryUpdateMarketData(_params.symbolIdThirdLeg);
}

void SBA::TLS::calculatePrice() { refreshMarketData(); }

void SBA::TLS::onMarketUpdate(SIGNED_LONG) { onDefaultEvent(); }

// ─────────────────────────────────────────────────────────────────────────────
// onTimerEvent / onDefaultEvent
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onTimerEvent()
{
    reqQrySendCustomResponse("",
        {"CurrentSpread:" + std::to_string(_params.CurrentSpread)}, 0);
    if (!_terminateCheck) reqTimerEvent(1000000);
}

void SBA::TLS::onDefaultEvent()
{
    if (_terminateCheck) return;
    calculatePrice();

    if (!_placeAggressive && _placeSecondOrder && isOrderOpen(_futHandle)) {
        if (_modify) { _modify = false; mapModifiedParameters(); }
        placeL1ModOrder();
        if (!(_secondTradedQ && _thirdTradedQ)) _secondLegTrade = true;
        return;
    }

    if (!_secondLegTrade && !_firstLegTrade) {
        _firstLegTrade = true;
        if (_modify) { _modify = false; mapModifiedParameters(); }
        placeL1Order();
    }

    if (!_placeSecondOrder) {
        _placeSecondOrder = true;
        checkCoverLegOrderStatus();
    }

    if (_placeAggressive) {
        _placeAggressive  = false;
        placeCoverLegOrder();
        _second_leg_counter = 0;
        _third_leg_counter  = 0;
        _placeSecondOrder   = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order placement
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeL1Order()
{
    _lastQuantityBidLegL1 = 0;
    _lastPlaceQty         = 0;

    if (_tradedQ >= _params.maxLots) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
        return;
    }

    _qty = std::min(_params.SOL, _params.maxLots - _tradedQ);
    SIGNED_LONG orderPrice = 0;

    if (_conFlag) {
        _qty = std::min(_qty, std::min(_mkData[1]->getBidQty(0), _mkData[2]->getAskQty(0)));
        _callBidPrice  = _mkData[1]->getBidPrice(0);
        _putAskPrice   = _mkData[2]->getAskPrice(0);
        _futAskPrice_1 = _mkData[0]->getAskPrice(0);
        _currentSpread = _strikePrice - _futAskPrice_1 + _callBidPrice - _putAskPrice;
        _futAskPrice   = _strikePrice - _params.forwardSpread + _callBidPrice - _putAskPrice;
        _params.CurrentSpread = _currentSpread;
        if (!_futAskPrice_1 || !_putAskPrice || !_callBidPrice || _futAskPrice <= 0)
            { _firstLegTrade = false; return; }
        if (_params.opportunity_check && _currentSpread < _params.Difference)
            { _firstLegTrade = false; return; }
        orderPrice = _futAskPrice;
    } else {
        _qty = std::min(_qty, std::min(_mkData[1]->getAskQty(0), _mkData[2]->getBidQty(0)));
        _callAskPrice  = _mkData[1]->getAskPrice(0);
        _putBidPrice   = _mkData[2]->getBidPrice(0);
        _futBidPrice_1 = _mkData[0]->getBidPrice(0);
        _currentSpread = -_strikePrice + _futBidPrice_1 - _callAskPrice + _putBidPrice;
        _futBidPrice   =  _strikePrice + _params.reverseSpread + _callAskPrice - _putBidPrice;
        _params.CurrentSpread = _currentSpread;
        if (!_putBidPrice || !_callAskPrice || !_futBidPrice_1 || _futBidPrice <= 0)
            { _firstLegTrade = false; return; }
        if (_params.opportunity_check && _currentSpread < _params.Difference)
            { _firstLegTrade = false; return; }
        orderPrice = _futBidPrice;
    }

    _secondLegTrade = true;
    // ── MW::Bidding::placeOrder — pool key, symbolId, price, qty ─────────────
    _futHandle = placeOrder(POOL_FUT, _params.symbolIdFirstLeg,
                            orderPrice, _qty, orderPrice, "L1");
    if (_futHandle == MW::INVALID_ORDER_HANDLE) {
        DEBUG_MESSAGE(reqQryDebugLog(), "placeL1Order: FAILED");
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    _firstLegTrade = false;
    _lastPlaceQty  = _qty;
}

void SBA::TLS::placeL1ModOrder()
{
    if (_tradedQ >= _params.maxLots) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
        return;
    }

    SIGNED_LONG lastQty = _qty;
    _qty = std::min(_params.SOL, _params.maxLots - _tradedQ);
    SIGNED_LONG orderPrice = 0;

    if (_conFlag) {
        _qty = std::min(_qty, std::min(_mkData[1]->getBidQty(0), _mkData[2]->getAskQty(0)));
        _callBidPrice  = _mkData[1]->getBidPrice(0);
        _putAskPrice   = _mkData[2]->getAskPrice(0);
        _futAskPrice_1 = _mkData[0]->getAskPrice(0);
        _currentSpread = _strikePrice - _futAskPrice_1 + _callBidPrice - _putAskPrice;
        _futAskPrice   = _strikePrice - _params.forwardSpread + _callBidPrice - _putAskPrice;
        _params.CurrentSpread = _currentSpread;
        if (!_futAskPrice_1 || !_putAskPrice || !_callBidPrice || _futAskPrice <= 0) return;
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            cancelOrder(_futHandle); _firstLegTrade = false; return;
        }
        _qty       = std::min(_qty, _lastPlaceQty - _lastQuantityBidLegL1);
        orderPrice = _futAskPrice;
    } else {
        _qty = std::min(_qty, std::min(_mkData[1]->getAskQty(0), _mkData[2]->getBidQty(0)));
        _callAskPrice  = _mkData[1]->getAskPrice(0);
        _putBidPrice   = _mkData[2]->getBidPrice(0);
        _futBidPrice_1 = _mkData[0]->getBidPrice(0);
        _currentSpread = -_strikePrice + _futBidPrice_1 - _callAskPrice + _putBidPrice;
        _futBidPrice   =  _strikePrice + _params.reverseSpread + _callAskPrice - _putBidPrice;
        _params.CurrentSpread = _currentSpread;
        if (!_putBidPrice || !_callAskPrice || !_futBidPrice_1 || _futBidPrice <= 0) return;
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            cancelOrder(_futHandle); _firstLegTrade = false; return;
        }
        _qty       = std::min(_qty, _lastPlaceQty - _lastQuantityBidLegL1);
        orderPrice = _futBidPrice;
    }

    if (!(_lastTriggerPrice == orderPrice && _qty == lastQty)) {
        if (isOrderOpen(_futHandle))
            modifyOrder(_futHandle, orderPrice, _qty + _lastQuantityBidLegL1, orderPrice);
        _lastTriggerPrice = orderPrice;
    }
}

void SBA::TLS::placeSecondLegOrder()
{
    if (_secondLegTimerIdentifier) { _second_leg_counter = 0; return; }
    _mkData[1] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    SIGNED_LONG prevPrice = _secondLegPrice;
    _secondLegPrice = _conFlag ? _mkData[1]->getBidPrice(_second_leg_depth)
                                : _mkData[1]->getAskPrice(_second_leg_depth);
    if (!_secondLegPrice) { _second_leg_depth = 0; return; }

    SIGNED_LONG pending = _traded - _secondTradedQ;
    if (!(prevPrice == _secondLegPrice && _lastQuantitySecondLeg == pending)) {
        if (isOrderOpen(_callHandle) &&
            modifyOrder(_callHandle, _secondLegPrice, _traded, _secondLegPrice))
            _lastQuantitySecondLeg = pending;
    }
    if (_second_leg_counter == 4) _second_leg_depth = 1;
}

void SBA::TLS::placeThirdLegOrder()
{
    if (_thirdLegTimerIdentifier) { _third_leg_counter = 0; return; }
    _mkData[2] = reqQryUpdateMarketData(_params.symbolIdThirdLeg);
    SIGNED_LONG prevPrice = _thirdLegPrice;
    _thirdLegPrice = _conFlag ? _mkData[2]->getAskPrice(_third_leg_depth)
                               : _mkData[2]->getBidPrice(_third_leg_depth);
    if (!_thirdLegPrice) { _third_leg_depth = 0; return; }

    SIGNED_LONG pending = _traded - _thirdTradedQ;
    if (!(prevPrice == _thirdLegPrice && _lastQuantityThirdLeg == pending)) {
        if (isOrderOpen(_putHandle) &&
            modifyOrder(_putHandle, _thirdLegPrice, _traded, _thirdLegPrice))
            _lastQuantityThirdLeg = pending;
    }
    if (_third_leg_counter == 4) _third_leg_depth = 1;
}

void SBA::TLS::checkCoverLegOrderStatus()
{
    if (_tradedQ >= _params.maxLots && _traded == 0) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
        return;
    }
    if (_secondLegTimerIdentifier && _thirdLegTimerIdentifier) {
        if (_traded == _secondTradedQ && _traded == _thirdTradedQ) return;
        _secondLegTimerIdentifier = false;
        _thirdLegTimerIdentifier  = false;
    }
    _second_leg_counter++;
    _third_leg_counter++;
    std::this_thread::sleep_for(std::chrono::milliseconds(_params.Timer * 10));
    placeSecondLegOrder();
    placeThirdLegOrder();
    if (!_secondLegPrice || !_thirdLegPrice)                 _placeSecondOrder = false;
    if (!_secondLegTimerIdentifier || !_thirdLegTimerIdentifier) _placeSecondOrder = false;
}

void SBA::TLS::placeCoverLegOrder()
{
    _mkData[1] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[2] = reqQryUpdateMarketData(_params.symbolIdThirdLeg);
    _second_leg_depth = 0; _third_leg_depth = 0;
    _secondLegTimerIdentifier = false; _thirdLegTimerIdentifier = false;
    _lastQuantitySecondLeg = _lastQuantityThirdLeg = _traded;

    _secondLegPrice = _conFlag ? _mkData[1]->getBidPrice(0) : _mkData[1]->getAskPrice(0);
    _thirdLegPrice  = _conFlag ? _mkData[2]->getAskPrice(0) : _mkData[2]->getBidPrice(0);
    if (!_secondLegPrice || !_thirdLegPrice) { _placeAggressive = true; return; }

    _callHandle = placeOrder(POOL_CALL, _params.symbolIdSecondLeg,
                             _secondLegPrice, _traded, _secondLegPrice, "cover_call");
    if (_callHandle == MW::INVALID_ORDER_HANDLE) {
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    _putHandle = placeOrder(POOL_PUT, _params.symbolIdThirdLeg,
                            _thirdLegPrice, _traded, _thirdLegPrice, "cover_put");
    if (_putHandle == MW::INVALID_ORDER_HANDLE) {
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// achievedSpreadCal
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::achievedSpreadCal(API2::OrderConfirmation& conf)
{
    uint64_t symId  = conf.getSymbolId();
    double   qty    = conf.getLastFillQuantity();
    double   px     = conf.getLastFillPrice();
    double   val    = qty * px;
    if      (symId == (uint64_t)_params.symbolIdFirstLeg)  { _v1Fut  += val; _q1Fut  += qty; }
    else if (symId == (uint64_t)_params.symbolIdSecondLeg) { _v2Call += val; _q2Call += qty; }
    else                                                    { _v3Put  += val; _q3Put  += qty; }

    if (_q1Fut > 0 && _q1Fut == _q2Call && _q2Call == _q3Put) {
        _priceFut  = _v1Fut / _q1Fut;
        _priceCall = _v2Call / _q2Call;
        _pricePut  = _v3Put  / _q3Put;
        SIGNED_LONG spread = _conFlag
            ? (_strikePrice - _priceFut + _priceCall - _pricePut)
            : (-_strikePrice + _priceFut - _priceCall + _pricePut);
        _params.AchievedSpread = spread;
        reqQrySendCustomResponse("",
            {"AchievedSpread:" + std::to_string(spread)}, 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Order event hooks
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onOrderConfirmed(API2::OrderConfirmation&,
                                 API2::COMMON::OrderId*,
                                 MW::OrderHandle h)
{
    DEBUG_VARSHOW(reqQryDebugLog(), "Confirmed handle:", h);
}

void SBA::TLS::onOrderRejected(API2::OrderConfirmation&,
                                API2::COMMON::OrderId*,
                                MW::OrderHandle)
{
    terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
}

void SBA::TLS::onOrderReplaced(API2::OrderConfirmation&,
                                API2::COMMON::OrderId*,
                                MW::OrderHandle) {}

void SBA::TLS::onOrderReplaceRejected(API2::OrderConfirmation&,
                                       API2::COMMON::OrderId*,
                                       MW::OrderHandle h)
{
    DEBUG_VARSHOW(reqQryDebugLog(), "ReplaceRejected handle:", h);
}

void SBA::TLS::onOrderCancelled(API2::OrderConfirmation&,
                                 API2::COMMON::OrderId*,
                                 MW::OrderHandle handle)
{
    if (handle == _futHandle) {
        _futHandle = MW::INVALID_ORDER_HANDLE;
        _secondLegTrade = false;
        _firstLegTrade  = false;
        onDefaultEvent();
    } else if (handle == _callHandle) {
        _callHandle = MW::INVALID_ORDER_HANDLE;
    } else if (handle == _putHandle) {
        _putHandle = MW::INVALID_ORDER_HANDLE;
    }
}

void SBA::TLS::onOrderCancelRejected(API2::OrderConfirmation&,
                                      API2::COMMON::OrderId*,
                                      MW::OrderHandle h)
{
    DEBUG_VARSHOW(reqQryDebugLog(), "CancelRejected handle:", h);
}

void SBA::TLS::onOrderPartialFill(API2::OrderConfirmation& conf,
                                   API2::COMMON::OrderId*,
                                   MW::OrderHandle handle)
{
    achievedSpreadCal(conf);
    SIGNED_LONG fill = static_cast<SIGNED_LONG>(conf.getLastFillQuantity());

    if (handle == _futHandle) {
        _tradedQ              += fill;
        _traded               += fill;
        _lastQuantityBidLegL1 += fill;
        if (!isOrderOpen(_callHandle) && !isOrderOpen(_putHandle)) {
            _placeAggressive = true;
            onDefaultEvent();
        }
    } else if (handle == _callHandle) {
        _secondTradedQ    += fill;
        _second_leg_depth  = 0;
        _second_leg_counter = 1;
    } else {
        _thirdTradedQ     += fill;
        _third_leg_depth   = 0;
        _third_leg_counter = 1;
    }
}

void SBA::TLS::onOrderFilled(API2::OrderConfirmation& conf,
                              API2::COMMON::OrderId*,
                              MW::OrderHandle handle)
{
    achievedSpreadCal(conf);
    SIGNED_LONG fill = static_cast<SIGNED_LONG>(conf.getLastFillQuantity());

    MW::OrderState st = orderState(handle);
    DEBUG_VARSHOW(reqQryDebugLog(), "Filled handle:", handle);
    DEBUG_VARSHOW(reqQryDebugLog(), "  filledQty:",   st.filledQty);
    DEBUG_VARSHOW(reqQryDebugLog(), "  avgFillPx:",   st.avgFillPrice);

    if (handle == _futHandle) {
        _firstLegOrderFlag    = true;
        _traded               += fill;
        _tradedQ              += fill;
        _lastQuantityBidLegL1 += fill;
        _futHandle = MW::INVALID_ORDER_HANDLE;
    } else if (handle == _callHandle) {
        _secondLegOrderFlag       = true;
        _secondLegTimerIdentifier = true;
        _second_leg_depth         = 0;
        _secondTradedQ           += fill;
        _callHandle = MW::INVALID_ORDER_HANDLE;
    } else {
        _thirdLegOrderFlag        = true;
        _thirdLegTimerIdentifier  = true;
        _third_leg_depth          = 0;
        _thirdTradedQ            += fill;
        _putHandle = MW::INVALID_ORDER_HANDLE;
    }

    if (_secondLegOrderFlag && _thirdLegOrderFlag) {
        _secondLegOrderFlag = _thirdLegOrderFlag = _firstLegOrderFlag = false;
        _second_leg_counter = _third_leg_counter = 0;

        if (_traded != _secondTradedQ) {
            _traded        -= _secondTradedQ;
            _secondTradedQ  = 0;
            _thirdTradedQ   = 0;
            _placeAggressive = true;
            onDefaultEvent();
            return;
        }
        _traded = _secondTradedQ = _thirdTradedQ = 0;
        _placeSecondOrder = true;
        _secondLegTrade   = false;
        _firstLegTrade    = false;
        reqQrySendCustomResponse("",
            {"FilledQty:" + std::to_string(_tradedQ * 100)}, 0);
        onDefaultEvent();
    } else if (_secondLegTrade && _firstLegOrderFlag) {
        if (!isOrderOpen(_callHandle) && !isOrderOpen(_putHandle)) {
            _firstLegOrderFlag = false;
            _placeAggressive   = true;
        }
        onDefaultEvent();
    }
}

void SBA::TLS::onOrderRmsReject(API2::OrderConfirmation&,
                                 API2::COMMON::OrderId*,
                                 MW::OrderHandle)
{
    terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Control
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onCMDModifyStrategy(API2::AbstractUserParams* newParams)
{
    auto* cp = static_cast<API2::UserParams*>(newParams);
    if (!setInternalParameters(cp, _modParams)) {
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.maxLots == 0 || _modParams.SOL == 0 ||
        (_modParams.forwardSpread == 0 && _modParams.reverseSpread == 0) ||
        (_modParams.forwardSpread != 0 && _modParams.reverseSpread != 0))
    {
        terminateStrategyComment(API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_terminateCheck) return;
    _modify = true;
    onDefaultEvent();
    reqSendStrategyResponse(
        API2::CONSTANTS::RSP_ResponseType_STRATEGY_RUNNING,
        API2::CONSTANTS::RSP_RiskStatus_SUCCESS,
        API2::CONSTANTS::RSP_StrategyComment_USER_INPUT);
}

void SBA::TLS::onCMDTerminateStartegy()
{
    terminateStrategyComment(
        API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
}

void SBA::TLS::terminateStrategyComment(API2::DATA_TYPES::StrategyComment c)
{
    if (_terminateCheck) return;
    _terminateCheck = true;
    reqAddStrategyComment(c);
    reqTerminateStrategy();
}

void SBA::TLS::dump(const FrontEndParameters& p)
{
    DEBUG_VARSHOW(reqQryDebugLog(), "sym LEG1", p.symbolIdFirstLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "sym LEG2", p.symbolIdSecondLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "sym LEG3", p.symbolIdThirdLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "fwdSpread", p.forwardSpread);
    DEBUG_VARSHOW(reqQryDebugLog(), "revSpread", p.reverseSpread);
    DEBUG_VARSHOW(reqQryDebugLog(), "SOL",       p.SOL);
    DEBUG_VARSHOW(reqQryDebugLog(), "maxLots",   p.maxLots);
}

std::string SBA::TLS::getTodayDate()
{
    time_t t = time(nullptr);
    tm* n = localtime(&t);
    char buf[9];
    strftime(buf, sizeof(buf), "%d%m%Y", n);
    return std::string(buf);
}
