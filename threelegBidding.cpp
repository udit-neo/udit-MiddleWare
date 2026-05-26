// =============================================================================
// threelegBidding.cpp  (SBA::TLS)
//
// ALL uTrade/exchange interaction goes through MW::Bidding:
//   placeNewOrder(legIdx, ...)  →  wrapper(legIdx).newOrder(...)
//   modifyOrder(legIdx, ...)    →  wrapper(legIdx).replaceOrder(...)
//   cancelLegOrder(legIdx, ...) →  wrapper(legIdx).cancelOrder(...)
//
// Confirmation callbacks arrive as onOrder* hooks (MW extension points).
// =============================================================================

#include "threelegBidding.h"
#include "apiConstants.h"
#include <../includes/sgDebugLogDefines.h>

#include <chrono>
#include <thread>
#include <algorithm>
#include <stdexcept>

using namespace MW::LegIndex;   // LEG_FUTURE=0, LEG_CALL=1, LEG_PUT=2

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
SBA::TLS::TLS(API2::StrategyParameters* params)
    : MW::Bidding(params, "ConRevBiddingStrategy")
{
    API2::UserParams* customParams =
        static_cast<API2::UserParams*>(params->getInfo());

    if (!setInternalParameters(customParams, _params)) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Parameters not set from front end");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    // ── Register params in the MW map so any component can retrieve them ─────
    MW::StrategyFrontEndParams mwp;
    mwp.symbolId[0]    = _params.symbolIdFirstLeg;
    mwp.symbolId[1]    = _params.symbolIdSecondLeg;
    mwp.symbolId[2]    = _params.symbolIdThirdLeg;
    mwp.orderMode[0]   = _params.firstLegOrderMode;
    mwp.orderMode[1]   = _params.secondLegOrderMode;
    mwp.orderMode[2]   = _params.thirdLegOrderMode;
    mwp.account        = _params.account;
    mwp.forwardSpread  = _params.forwardSpread;
    mwp.reverseSpread  = _params.reverseSpread;
    mwp.Difference     = _params.Difference;
    mwp.OrderSize      = _params.SOL;
    mwp.Timer          = _params.Timer;
    mwp.opportunity_check = _params.opportunity_check;
    mwp.strategyId     = _params.strategyId;
    registerParams(_params.strategyId, mwp);

    try {
        registerSymbols();
        validateParameters();
    }
    catch (API2::MarketDataSubscriptionFailedException&) {
        DEBUG_MESSAGE(reqQryDebugLog(), "TBT subscription Failed");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    catch (API2::InstrumentNotFoundException&) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Instrument Not Found");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    catch (std::exception& e) {
        DEBUG_MESSAGE(reqQryDebugLog(),
            std::string("Exception in init: ") + e.what());
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    if (_params.forwardSpread) _conFlag = true;

    _strikePrice = instrument(LEG_CALL)->getStaticData()->strikePrice;

    _algoIdSet.insert(instrument(LEG_FUTURE)->getStaticData()->marketId);
    readConfForAlgoid("SAMPLE_BIDDING", "SampleBidding.txt", "_ALGO_ID", true);

    // ── Build MW wrapper groups (legs 0-1-2 → triplet group 0) ───────────────
    if (!createWrapperGroups()) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    // ── Fetch initial market data ─────────────────────────────────────────────
    _mkData[LEG_FUTURE] = reqQryUpdateMarketData(_params.symbolIdFirstLeg);
    _mkData[LEG_CALL]   = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[LEG_PUT]    = reqQryUpdateMarketData(_params.symbolIdThirdLeg);

    _symbolToMkdataTs[_params.symbolIdFirstLeg]  = 0;
    _symbolToMkdataTs[_params.symbolIdSecondLeg] = 0;
    _symbolToMkdataTs[_params.symbolIdThirdLeg]  = 0;

#if INVOKING_API
    runCashFut();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Static driver (entry point for uTrade thread)
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::bidDriver(void* params)
{
    auto* sgParams = static_cast<API2::StrategyParameters*>(params);
    boost::shared_ptr<API2::SGContext> obj(new SBA::TLS(sgParams));
    obj->reqStartAlgo(true, false);
    API2::SGContext::registerStrategy(obj);
    obj->reqTimerEvent(1000000);
}

// ─────────────────────────────────────────────────────────────────────────────
// registerSymbols – create instruments; called once in constructor
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::registerSymbols()
{
    // snapshot=true, TBT=false, live=true
    createNewInstrument(_params.symbolIdFirstLeg,  true, false, true);
    createNewInstrument(_params.symbolIdSecondLeg, true, false, true);
    createNewInstrument(_params.symbolIdThirdLeg,  true, false, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// createWrapperGroups – tell MW::Bidding about the 3 legs
// ─────────────────────────────────────────────────────────────────────────────
bool SBA::TLS::createWrapperGroups()
{
    // Retrieve instruments created by registerSymbols / createNewInstrument
    // Assuming this is inside your strategy constructor or setup function
    API2::COMMON::Instrument* instr[3] = {
        createNewInstrument(_params.symbolIdFirstLeg,  true, false),
        createNewInstrument(_params.symbolIdSecondLeg, true, false),
        createNewInstrument(_params.symbolIdThirdLeg,  true, false)
    };

    for (int i = 0; i < 3; ++i) {
        if (!instr[i]) {
            std::cerr << "[SBA] createWrapperGroups: null instrument at " << i << "\n";
            return false;
        }
    }

    std::vector<MW::LegSpec> specs(3);
    specs[0] = { _params.symbolIdFirstLeg,  _params.firstLegOrderMode,  _params.account };
    specs[1] = { _params.symbolIdSecondLeg, _params.secondLegOrderMode, _params.account };
    specs[2] = { _params.symbolIdThirdLeg,  _params.thirdLegOrderMode,  _params.account };

    std::vector<API2::COMMON::Instrument*> instrs(instr, instr + 3);
    return buildWrapperGroups(specs, instrs);
}

// ─────────────────────────────────────────────────────────────────────────────
// setInternalParameters
// ─────────────────────────────────────────────────────────────────────────────
bool SBA::TLS::setInternalParameters(API2::UserParams*    customParams,
                                     FrontEndParameters&  p)
{
    FILL_PARAMS("SYMBOL LEG1",  p.symbolIdFirstLeg);
    FILL_PARAMS("SYMBOL LEG2",  p.symbolIdSecondLeg);
    FILL_PARAMS("SYMBOL LEG3",  p.symbolIdThirdLeg);
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

// ─────────────────────────────────────────────────────────────────────────────
// dump
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::dump(const FrontEndParameters& p)
{
    DEBUG_VARSHOW(reqQryDebugLog(), "orderMode LEG1",   (int)p.firstLegOrderMode);
    DEBUG_VARSHOW(reqQryDebugLog(), "symbolId  LEG1",   p.symbolIdFirstLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "orderMode LEG2",   (int)p.secondLegOrderMode);
    DEBUG_VARSHOW(reqQryDebugLog(), "symbolId  LEG2",   p.symbolIdSecondLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "orderMode LEG3",   (int)p.thirdLegOrderMode);
    DEBUG_VARSHOW(reqQryDebugLog(), "symbolId  LEG3",   p.symbolIdThirdLeg);
    DEBUG_VARSHOW(reqQryDebugLog(), "forwardSpread",    p.forwardSpread);
    DEBUG_VARSHOW(reqQryDebugLog(), "reverseSpread",    p.reverseSpread);
    DEBUG_VARSHOW(reqQryDebugLog(), "CurrentSpread",    p.CurrentSpread);
    DEBUG_VARSHOW(reqQryDebugLog(), "opportunity_check",p.opportunity_check);
    DEBUG_VARSHOW(reqQryDebugLog(), "Difference",       p.Difference);
    DEBUG_VARSHOW(reqQryDebugLog(), "SOL",              p.SOL);
    DEBUG_VARSHOW(reqQryDebugLog(), "maxLots",          p.maxLots);
    DEBUG_VARSHOW(reqQryDebugLog(), "Timer",            p.Timer);
    DEBUG_VARSHOW(reqQryDebugLog(), "AchievedSpread",   p.AchievedSpread);
}

// ─────────────────────────────────────────────────────────────────────────────
// mapModifiedParameters
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::mapModifiedParameters()
{
    _params = _modParams;

    // Keep MW registry in sync
    MW::StrategyFrontEndParams mwp;
    mwp.symbolId[0]    = _params.symbolIdFirstLeg;
    mwp.symbolId[1]    = _params.symbolIdSecondLeg;
    mwp.symbolId[2]    = _params.symbolIdThirdLeg;
    mwp.orderMode[0]   = _params.firstLegOrderMode;
    mwp.orderMode[1]   = _params.secondLegOrderMode;
    mwp.orderMode[2]   = _params.thirdLegOrderMode;
    mwp.account        = _params.account;
    mwp.forwardSpread  = _params.forwardSpread;
    mwp.reverseSpread  = _params.reverseSpread;
    mwp.Difference     = _params.Difference;
    mwp.OrderSize      = _params.SOL;
    mwp.Timer          = _params.Timer;
    mwp.opportunity_check = _params.opportunity_check;
    mwp.CurrentSpread  = _params.CurrentSpread;
    mwp.AchievedPrice  = _params.AchievedSpread;
    mwp.strategyId     = _params.strategyId;
    updateParams(_params.strategyId, mwp);
}

// ─────────────────────────────────────────────────────────────────────────────
// validateParameters
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::validateParameters()
{
    if (_params.forwardSpread == 0 && _params.reverseSpread == 0)
        throw std::runtime_error("Both spreads can't be zero");

    if (_params.maxLots == 0 || _params.SOL == 0)
        throw std::runtime_error("maxLots / SOL can't be zero");

    if (_params.opportunity_check) {
        if (_params.forwardSpread  && _params.Difference > _params.forwardSpread)
            throw std::runtime_error("opportunity_check(con): Difference > forwardSpread");
        if (_params.reverseSpread  && _params.Difference > _params.reverseSpread)
            throw std::runtime_error("opportunity_check(rev): Difference > reverseSpread");
    }

    enum BuyOrSell  { Buy = 0, Sell = 1 };
    enum OptionType { Put = 0, Call = 1, Future = 2 };

    struct OptionLeg {
        int         optionType;
        int         optionMode;
        double      strikePrice;
        SIGNED_LONG symbolId;
        int         expiryDay;
        int         expiryMon;
        std::string instrument;
    };

    // Build leg descriptors from freshly created instruments
    auto mkLeg = [&](std::size_t idx, SIGNED_LONG symId, char mode) -> OptionLeg {
        auto* instr = MW::Bidding::instrument(idx);
        OptionLeg L;
        L.optionType  = (int)instr->getStaticData()->optionMode;
        L.optionMode  = (int)mode;
        L.strikePrice = instr->getStaticData()->strikePrice;
        L.symbolId    = symId;
        L.expiryDay   = instr->getStaticData()->maturityDay;
        L.expiryMon   = instr->getStaticData()->maturityYearmon;
        L.instrument  = instr->getStaticData()->symbol;
        return L;
    };

    OptionLeg e1 = mkLeg(0, _params.symbolIdFirstLeg,  _params.firstLegOrderMode);
    OptionLeg e2 = mkLeg(1, _params.symbolIdSecondLeg, _params.secondLegOrderMode);
    OptionLeg e3 = mkLeg(2, _params.symbolIdThirdLeg,  _params.thirdLegOrderMode);

    std::vector<OptionLeg> legs = { e1, e2, e3 };
    std::vector<OptionLeg> calls, puts, futures;
    OptionLeg L1, L2, L3;
    for (auto& leg : legs) {
        if      (leg.optionType == OptionType::Future) { L1 = leg; futures.push_back(leg); }
        else if (leg.optionType == OptionType::Call)   { L2 = leg; calls.push_back(leg);   }
        else                                            { L3 = leg; puts.push_back(leg);    }
    }

    if (calls.size() != 1 || puts.size() != 1 || futures.size() != 1)
        throw std::runtime_error("Need exactly 1 future, 1 call, 1 put");

    bool conOk  = (L1.optionMode == Buy  && L2.optionMode == Sell && L3.optionMode == Buy  && _params.forwardSpread);
    bool revOk  = (L1.optionMode == Sell && L2.optionMode == Buy  && L3.optionMode == Sell && _params.reverseSpread);
    if (!conOk && !revOk)
        throw std::runtime_error("Order modes incorrect or wrong spread direction");

    if (L1.instrument != L2.instrument || L2.instrument != L3.instrument)
        throw std::runtime_error("All legs must share the same underlying symbol");

    if (L2.strikePrice != L3.strikePrice)
        throw std::runtime_error("Call and Put strike prices must match");

    if (L1.expiryDay != L2.expiryDay || L2.expiryDay != L3.expiryDay ||
        L1.expiryMon != L2.expiryMon || L2.expiryMon != L3.expiryMon)
        throw std::runtime_error("Expiry dates must be identical across legs");

    // Re-order params so leg indices always map  0=Fut, 1=Call, 2=Put
    _params.symbolIdFirstLeg   = L1.symbolId;
    _params.symbolIdSecondLeg  = L2.symbolId;
    _params.symbolIdThirdLeg   = L3.symbolId;
    _params.firstLegOrderMode  = L1.optionMode;
    _params.secondLegOrderMode = L2.optionMode;
    _params.thirdLegOrderMode  = L3.optionMode;
}

// ─────────────────────────────────────────────────────────────────────────────
// refreshMarketData
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::refreshMarketData()
{
    _mkData[LEG_FUTURE] = reqQryUpdateMarketData(_params.symbolIdFirstLeg);
    _mkData[LEG_CALL]   = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[LEG_PUT]    = reqQryUpdateMarketData(_params.symbolIdThirdLeg);
}

void SBA::TLS::calculatePrice()
{
    refreshMarketData();
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateStrategyComment
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::terminateStrategyComment(API2::DATA_TYPES::StrategyComment comment)
{
    if (_terminateCheck) return;
    _terminateCheck = true;

#if INVOKING_API
    printChildPosition(_child1);
    invokingApi->terminateInvokedStrategy(
        _child1, API2::CONSTANTS::CMD_CommandCategory_TERMINATE_STRATEGY);
#endif

    reqAddStrategyComment(comment);
    reqTerminateStrategy();
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer / default events
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onTimerEvent()
{
    std::string msg;
    msg.reserve(32);
    msg = "CurrentSpread:" + std::to_string(_params.CurrentSpread);
    reqQrySendCustomResponse("", {msg}, 0);

    if (!_terminateCheck) reqTimerEvent(1000000);
}

void SBA::TLS::onDefaultEvent()
{
    if (_terminateCheck) {
        DEBUG_MESSAGE(reqQryDebugLog(), "strategy terminating");
        return;
    }

    calculatePrice();

    // ── If the L1 order is still open, try to modify it ─────────────────────
    if (!_placeAggressive && _placeSecondOrder && wrapper(LEG_FUTURE).isOrderOpen()) {
        if (_modify) {
            _modify = false;
            mapModifiedParameters();
        }
        placeL1ModOrder();
        if (!(_secondTradedQ && _thirdTradedQ))
            _secondLegTrade = true;
        return;
    }

    // ── Place fresh L1 order ─────────────────────────────────────────────────
    if (!_secondLegTrade && !_firstLegTrade) {
        _firstLegTrade = true;
        if (_modify) {
            _modify = false;
            mapModifiedParameters();
        }
        placeOrder();
    }

    // ── Trigger cover-leg placement after L1 fill ─────────────────────────────
    if (!_placeSecondOrder) {
        _placeSecondOrder = true;
        checkCoverLegOrderStatus();
    }

    // ── Aggressive cover-leg replacement ─────────────────────────────────────
    if (_placeAggressive) {
        _placeAggressive   = false;
        placeCoverLegOrder();
        _second_leg_counter = 0;
        _third_leg_counter  = 0;
        _placeSecondOrder   = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// placeL1ModOrder – modify the outstanding first-leg (futures) order
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeL1ModOrder()
{
    if (_tradedQ >= _params.maxLots) {
        DEBUG_VARSHOW(reqQryDebugLog(),
            "Strategy successfully terminated after trades: ", _tradedQ);
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
        return;
    }

    SIGNED_LONG lastQty = _qty;
    _qty = std::min(_params.SOL, _params.maxLots - _tradedQ);

    if (_conFlag) {
        // Conversion: buy future
        _qty = std::min(_qty,
            std::min(_mkData[LEG_CALL]->getBidQty(0),
                     _mkData[LEG_PUT ]->getAskQty(0)));

        _callBidPrice  = _mkData[LEG_CALL]->getBidPrice(0);
        _putAskPrice   = _mkData[LEG_PUT ]->getAskPrice(0);
        _futAskPrice_1 = _mkData[LEG_FUTURE]->getAskPrice(0);

        _currentSpread = instrument(LEG_CALL)->getStaticData()->strikePrice
                         - _futAskPrice_1 + _callBidPrice - _putAskPrice;
        _futAskPrice   = instrument(LEG_CALL)->getStaticData()->strikePrice
                         - _params.forwardSpread + _callBidPrice - _putAskPrice;

        _params.CurrentSpread = _currentSpread;

        if (!_futAskPrice_1 || !_putAskPrice || !_callBidPrice || _futAskPrice <= 0) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeL1ModOrder(con): price is 0");
            return;
        }
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            cancelLegOrder(LEG_FUTURE, _riskStatus);
            _firstLegTrade = false;
            return;
        }

        _qty = std::min(_qty, _lastPlaceQty - _lastQuantityBidLegL1);

        if (!(_lastTriggerPrice == _futAskPrice && _qty == lastQty)) {
            auto t0 = std::chrono::high_resolution_clock::now();
            if (wrapper(LEG_FUTURE).isOrderOpen() &&
                !modifyOrder(LEG_FUTURE, _riskStatus,
                             _futAskPrice, _qty + _lastQuantityBidLegL1, _futAskPrice))
            {
                onDefaultEvent();
                return;
            }
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::high_resolution_clock::now() - t0).count();
            DEBUG_VARSHOW(reqQryDebugLog(),
                "L1 mod conv order ns: ", ns);
            _lastTriggerPrice = _futAskPrice;
        }
    }
    else {
        // Reversal: sell future
        _qty = std::min(_qty,
            std::min(_mkData[LEG_CALL]->getAskQty(0),
                     _mkData[LEG_PUT ]->getBidQty(0)));

        _callAskPrice  = _mkData[LEG_CALL]->getAskPrice(0);
        _putBidPrice   = _mkData[LEG_PUT ]->getBidPrice(0);
        _futBidPrice_1 = _mkData[LEG_FUTURE]->getBidPrice(0);

        _currentSpread = -instrument(LEG_CALL)->getStaticData()->strikePrice
                         + _futBidPrice_1 - _callAskPrice + _putBidPrice;
        _futBidPrice   = instrument(LEG_CALL)->getStaticData()->strikePrice
                         + _params.reverseSpread + _callAskPrice - _putBidPrice;

        _params.CurrentSpread = _currentSpread;

        if (!_putBidPrice || !_callAskPrice || !_futBidPrice_1 || _futBidPrice <= 0) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeL1ModOrder(rev): price is 0");
            return;
        }
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            cancelLegOrder(LEG_FUTURE, _riskStatus);
            _firstLegTrade = false;
            return;
        }

        _qty = std::min(_qty, _lastPlaceQty - _lastQuantityBidLegL1);

        if (!(_lastTriggerPrice == _futBidPrice && _qty == lastQty)) {
            auto t0 = std::chrono::high_resolution_clock::now();
            if (wrapper(LEG_FUTURE).isOrderOpen() &&
                !modifyOrder(LEG_FUTURE, _riskStatus,
                             _futBidPrice, _qty + _lastQuantityBidLegL1, _futBidPrice))
            {
                onDefaultEvent();
                return;
            }
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::high_resolution_clock::now() - t0).count();
            DEBUG_VARSHOW(reqQryDebugLog(),
                "L1 mod rev order ns: ", ns);
            _lastTriggerPrice = _futBidPrice;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// placeOrder – send fresh L1 (first-leg / futures) order
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeOrder()
{
    _lastQuantityBidLegL1 = 0;
    _lastPlaceQty         = 0;

    if (_tradedQ >= _params.maxLots) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);
        return;
    }

    _qty = std::min(_params.SOL, _params.maxLots - _tradedQ);

    if (_conFlag) {
        _qty = std::min(_qty,
            std::min(_mkData[LEG_CALL]->getBidQty(0),
                     _mkData[LEG_PUT ]->getAskQty(0)));

        _callBidPrice  = _mkData[LEG_CALL]->getBidPrice(0);
        _putAskPrice   = _mkData[LEG_PUT ]->getAskPrice(0);
        _futAskPrice_1 = _mkData[LEG_FUTURE]->getAskPrice(0);

        _currentSpread = instrument(LEG_CALL)->getStaticData()->strikePrice
                         - _futAskPrice_1 + _callBidPrice - _putAskPrice;
        _futAskPrice   = instrument(LEG_CALL)->getStaticData()->strikePrice
                         - _params.forwardSpread + _callBidPrice - _putAskPrice;
        _params.CurrentSpread = _currentSpread;

        if (!_futAskPrice_1 || !_putAskPrice || !_callBidPrice || _futAskPrice <= 0) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeOrder(con): price is 0");
            _firstLegTrade = false;
            return;
        }
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            _firstLegTrade = false;
            return;
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        _secondLegTrade = true;

        if (!placeNewOrder(LEG_FUTURE, _riskStatus, _futAskPrice, _qty, _futAskPrice)) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeOrder(con): L1 newOrder failed");
            terminateStrategyComment(
                API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
            return;
        }
        DEBUG_VARSHOW(reqQryDebugLog(), "L1 new conv order ns: ",
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - t0).count());
        _firstLegTrade = false;
        _lastPlaceQty  = _qty;
    }
    else {
        _qty = std::min(_qty,
            std::min(_mkData[LEG_CALL]->getAskQty(0),
                     _mkData[LEG_PUT ]->getBidQty(0)));

        _callAskPrice  = _mkData[LEG_CALL]->getAskPrice(0);
        _putBidPrice   = _mkData[LEG_PUT ]->getBidPrice(0);
        _futBidPrice_1 = _mkData[LEG_FUTURE]->getBidPrice(0);

        _currentSpread = -instrument(LEG_CALL)->getStaticData()->strikePrice
                         + _futBidPrice_1 - _callAskPrice + _putBidPrice;
        _futBidPrice   = instrument(LEG_CALL)->getStaticData()->strikePrice
                         + _params.reverseSpread + _callAskPrice - _putBidPrice;
        _params.CurrentSpread = _currentSpread;

        if (!_putBidPrice || !_callAskPrice || !_futBidPrice_1 || _futBidPrice <= 0) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeOrder(rev): price is 0");
            _firstLegTrade = false;
            return;
        }
        if (_params.opportunity_check && _currentSpread < _params.Difference) {
            _firstLegTrade = false;
            return;
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        _secondLegTrade = true;

        if (!placeNewOrder(LEG_FUTURE, _riskStatus, _futBidPrice, _qty, _futBidPrice)) {
            DEBUG_MESSAGE(reqQryDebugLog(), "placeOrder(rev): L1 newOrder failed");
            terminateStrategyComment(
                API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
            return;
        }
        DEBUG_VARSHOW(reqQryDebugLog(), "L1 new rev order ns: ",
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - t0).count());
        _firstLegTrade = false;
        _lastPlaceQty  = _qty;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// placeSecondLegOrder – modify/place L2 (call) cover order
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeSecondLegOrder()
{
    if (_secondLegTimerIdentifier) { _second_leg_counter = 0; return; }

    _mkData[LEG_CALL] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    SIGNED_LONG prevPrice = _secondLegPrice;

    if (_conFlag) {
        if (_mkData[LEG_CALL]->getBidPrice(_second_leg_depth) == 0) _second_leg_depth = 0;
        _secondLegPrice = _mkData[LEG_CALL]->getBidPrice(_second_leg_depth);
    }
    else {
        if (_mkData[LEG_CALL]->getAskPrice(_second_leg_depth) == 0) _second_leg_depth = 0;
        _secondLegPrice = _mkData[LEG_CALL]->getAskPrice(_second_leg_depth);
    }

    SIGNED_LONG pendingQty = _traded - _secondTradedQ;
    if (_secondLegPrice && !(prevPrice == _secondLegPrice && _lastQuantitySecondLeg == pendingQty)) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!modifyOrder(LEG_CALL, _riskStatus, _secondLegPrice, _traded, _secondLegPrice)) {
            DEBUG_MESSAGE(reqQryDebugLog(),
                "L2 modify failed at " + std::to_string(_secondLegPrice));
        }
        else {
            DEBUG_VARSHOW(reqQryDebugLog(), "L2 mod ns: ",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now() - t0).count());
            _lastQuantitySecondLeg = pendingQty;
        }
    }
    if (_second_leg_counter == 4) _second_leg_depth = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// placeThirdLegOrder – modify/place L3 (put) cover order
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeThirdLegOrder()
{
    if (_thirdLegTimerIdentifier) { _third_leg_counter = 0; return; }

    _mkData[LEG_PUT] = reqQryUpdateMarketData(_params.symbolIdThirdLeg);
    SIGNED_LONG prevPrice = _thirdLegPrice;

    if (_conFlag) {
        if (_mkData[LEG_PUT]->getAskPrice(_third_leg_depth) == 0) _third_leg_depth = 0;
        _thirdLegPrice = _mkData[LEG_PUT]->getAskPrice(_third_leg_depth);
    }
    else {
        if (_mkData[LEG_PUT]->getBidPrice(_third_leg_depth) == 0) _third_leg_depth = 0;
        _thirdLegPrice = _mkData[LEG_PUT]->getBidPrice(_third_leg_depth);
    }

    SIGNED_LONG pendingQty = _traded - _thirdTradedQ;
    if (_thirdLegPrice && !(prevPrice == _thirdLegPrice && _lastQuantityThirdLeg == pendingQty)) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (!modifyOrder(LEG_PUT, _riskStatus, _thirdLegPrice, _traded, _thirdLegPrice)) {
            DEBUG_MESSAGE(reqQryDebugLog(),
                "L3 modify failed at " + std::to_string(_thirdLegPrice));
        }
        else {
            DEBUG_VARSHOW(reqQryDebugLog(), "L3 mod ns: ",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now() - t0).count());
            _lastQuantityThirdLeg = pendingQty;
        }
    }
    if (_third_leg_counter == 4) _third_leg_depth = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// checkCoverLegOrderStatus
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::checkCoverLegOrderStatus()
{
    if (_tradedQ >= _params.maxLots && _traded == 0)
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_COMPLETED_SUCCESSFULLY);

    if (_secondLegTimerIdentifier && _thirdLegTimerIdentifier) {
        if (_traded != _secondTradedQ || _traded != _thirdTradedQ) {
            _secondLegTimerIdentifier = false;
            _thirdLegTimerIdentifier  = false;
        }
        else return;
    }

    _third_leg_counter++;
    _second_leg_counter++;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(_params.Timer * 10));

    placeSecondLegOrder();
    placeThirdLegOrder();

    if (!_secondLegPrice || !_thirdLegPrice)
        _placeSecondOrder = false;
    if (!_secondLegTimerIdentifier || !_thirdLegTimerIdentifier)
        _placeSecondOrder = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// placeCoverLegOrder – place fresh L2+L3 orders aggressively
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::placeCoverLegOrder()
{
    _mkData[LEG_CALL] = reqQryUpdateMarketData(_params.symbolIdSecondLeg);
    _mkData[LEG_PUT]  = reqQryUpdateMarketData(_params.symbolIdThirdLeg);

    _second_leg_depth = 0;
    _third_leg_depth  = 0;

    _secondLegPrice = _conFlag ? _mkData[LEG_CALL]->getBidPrice(0)
                                : _mkData[LEG_CALL]->getAskPrice(0);
    _thirdLegPrice  = _conFlag ? _mkData[LEG_PUT]->getAskPrice(0)
                                : _mkData[LEG_PUT]->getBidPrice(0);

    _secondLegTimerIdentifier = false;
    _thirdLegTimerIdentifier  = false;
    _lastQuantitySecondLeg    = _traded;
    _lastQuantityThirdLeg     = _traded;

    if (!_secondLegPrice || !_thirdLegPrice) {
        DEBUG_MESSAGE(reqQryDebugLog(), "placeCoverLegOrder: price is 0");
        _placeAggressive = true;
        return;
    }

    auto t0 = std::chrono::steady_clock::now();

    // ── L2 (call) ─────────────────────────────────────────────────────────────
    if (!placeNewOrder(LEG_CALL, _riskStatus, _secondLegPrice, _traded, _secondLegPrice)) {
        DEBUG_MESSAGE(reqQryDebugLog(), "placeCoverLegOrder: L2 newOrder failed");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    // ── L3 (put) ──────────────────────────────────────────────────────────────
    if (!placeNewOrder(LEG_PUT, _riskStatus, _thirdLegPrice, _traded, _thirdLegPrice)) {
        DEBUG_MESSAGE(reqQryDebugLog(), "placeCoverLegOrder: L3 newOrder failed");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }

    DEBUG_MESSAGE(reqQryDebugLog(),
        "L2+L3 placed in ns: " + std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count()));
}

// ─────────────────────────────────────────────────────────────────────────────
// achievedSpreadCal
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::achievedSpreadCal(API2::OrderConfirmation& conf)
{
    const uint64_t symId    = conf.getSymbolId();
    const double   fillQty  = conf.getLastFillQuantity();
    const double   fillPx   = conf.getLastFillPrice();
    const double   val      = fillQty * fillPx;

    if      (symId == (uint64_t)_params.symbolIdFirstLeg)  { _v1Fut  += val; _q1Fut  += fillQty; }
    else if (symId == (uint64_t)_params.symbolIdSecondLeg) { _v2Call += val; _q2Call += fillQty; }
    else                                                    { _v3Put  += val; _q3Put  += fillQty; }

    if (_q1Fut > 0 && _q1Fut == _q2Call && _q2Call == _q3Put) {
        _priceFut  = _v1Fut  / _q1Fut;
        _priceCall = _v2Call / _q2Call;
        _pricePut  = _v3Put  / _q3Put;

        _currSpread = _conFlag
            ? (_strikePrice - _priceFut + _priceCall - _pricePut)
            : (-_strikePrice + _priceFut - _priceCall + _pricePut);

        _params.AchievedSpread = _currSpread;
        reqQrySendCustomResponse("",
            { "AchievedSpread:" + std::to_string(_params.AchievedSpread) }, 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// onMarketDataEvent
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onMarketDataEvent(UNSIGNED_LONG symbolId)
{
    _currMkData = reqQryUpdateMarketData(symbolId);
    UNSIGNED_LONG ts = _currMkData->getTimeStamp();
    if (ts != _symbolToMkdataTs[symbolId]) {
        _symbolToMkdataTs[symbolId] = ts;
        onDefaultEvent();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// onCMDModifyStrategy
// ─────────────────────────────────────────────────────────────────────────────
void SBA::TLS::onCMDModifyStrategy(API2::AbstractUserParams* newParams)
{
    auto* customParams = static_cast<API2::UserParams*>(newParams);
    DEBUG_MESSAGE(reqQryDebugLog(), "Modifying strategy");

    if (!setInternalParameters(customParams, _modParams)) {
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.maxLots == 0 || _modParams.SOL == 0) {
        DEBUG_MESSAGE(reqQryDebugLog(), "maxLots/SOL zero on modify");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.Timer < 0) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Timer negative on modify");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.forwardSpread == 0 && _modParams.reverseSpread == 0) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Both spreads zero on modify");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.forwardSpread != 0 && _modParams.reverseSpread != 0) {
        DEBUG_MESSAGE(reqQryDebugLog(), "Both spreads non-zero on modify");
        terminateStrategyComment(
            API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
        return;
    }
    if (_modParams.opportunity_check) {
        if (_modParams.forwardSpread && _modParams.Difference > _modParams.forwardSpread) {
            terminateStrategyComment(
                API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
            return;
        }
        if (_modParams.reverseSpread && _modParams.Difference > _modParams.reverseSpread) {
            terminateStrategyComment(
                API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
            return;
        }
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

// ─────────────────────────────────────────────────────────────────────────────
// Order-event extension-point overrides
// ─────────────────────────────────────────────────────────────────────────────

void SBA::TLS::onOrderNewConfirmed(API2::OrderConfirmation& conf,
                                   API2::COMMON::OrderId*   orderId)
{
    // Nothing extra needed beyond what MW::Bidding already did (routeConfirmation)
}

void SBA::TLS::onOrderRejected(API2::OrderConfirmation& conf,
                                API2::COMMON::OrderId*   orderId)
{
    DEBUG_MESSAGE(reqQryDebugLog(),
        "onOrderRejected: terminating strategy");
    terminateStrategyComment(
        API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
}

void SBA::TLS::onOrderReplaced(API2::OrderConfirmation& conf,
                                API2::COMMON::OrderId*   orderId)
{
    // No extra action needed
}

void SBA::TLS::onOrderReplaceRejected(API2::OrderConfirmation& conf,
                                       API2::COMMON::OrderId*   orderId)
{
    DEBUG_MESSAGE(reqQryDebugLog(), "onOrderReplaceRejected");
}

void SBA::TLS::onOrderCancelled(API2::OrderConfirmation& conf,
                                 API2::COMMON::OrderId*   orderId)
{
    // If L1 was cancelled, reset trade flags and re-evaluate
    if (wrapper(LEG_FUTURE)._orderId == orderId) {
        _secondLegTrade = false;
        _firstLegTrade  = false;
        onDefaultEvent();
    }
}

void SBA::TLS::onOrderCancelRejected(API2::OrderConfirmation& conf,
                                      API2::COMMON::OrderId*   orderId)
{
    DEBUG_MESSAGE(reqQryDebugLog(), "onOrderCancelRejected");
}

void SBA::TLS::onOrderPartialFill(API2::OrderConfirmation& conf,
                                   API2::COMMON::OrderId*   orderId)
{
    achievedSpreadCal(conf);

    if (wrapper(LEG_FUTURE)._orderId == orderId) {
        _tradedQ              += conf.getLastFillQuantity();
        _traded               += conf.getLastFillQuantity();
        _lastQuantityBidLegL1 += conf.getLastFillQuantity();
        // If cover legs aren't open yet, trigger aggressive placement
        if (!(wrapper(LEG_CALL).isOrderOpen() || wrapper(LEG_PUT).isOrderOpen())) {
            _placeAggressive = true;
            onDefaultEvent();
        }
    }
    else if (wrapper(LEG_CALL)._orderId == orderId) {
        _secondTradedQ     += conf.getLastFillQuantity();
        _second_leg_depth   = 0;
        _second_leg_counter = 1;
    }
    else {
        _thirdTradedQ      += conf.getLastFillQuantity();
        _third_leg_depth    = 0;
        _third_leg_counter  = 1;
    }
}

void SBA::TLS::onOrderFilled(API2::OrderConfirmation& conf,
                              API2::COMMON::OrderId*   orderId)
{
    achievedSpreadCal(conf);

    if (wrapper(LEG_FUTURE)._orderId == orderId) {
        _firstLegOrderFlag    = true;
        _traded               += conf.getLastFillQuantity();
        _tradedQ              += conf.getLastFillQuantity();
        _lastQuantityBidLegL1 += conf.getLastFillQuantity();
    }
    else if (wrapper(LEG_CALL)._orderId == orderId) {
        _secondLegOrderFlag    = true;
        _secondLegTimerIdentifier = true;
        _second_leg_depth      = 0;
        _secondTradedQ        += conf.getLastFillQuantity();
    }
    else {
        _thirdLegOrderFlag     = true;
        _thirdLegTimerIdentifier = true;
        _third_leg_depth       = 0;
        _thirdTradedQ         += conf.getLastFillQuantity();
    }

    // ── Both cover legs fully filled ─────────────────────────────────────────
    if (_secondLegOrderFlag && _thirdLegOrderFlag) {
        _secondLegOrderFlag = _thirdLegOrderFlag = _firstLegOrderFlag = false;
        _second_leg_counter = _third_leg_counter = 0;

        if (_traded != _secondTradedQ) {
            // Residual first-leg qty: place aggressively
            _traded        -= _secondTradedQ;
            _secondTradedQ  = 0;
            _thirdTradedQ   = 0;
            _placeAggressive = true;
            onDefaultEvent();
            return;
        }

        _traded = _secondTradedQ = _thirdTradedQ = 0;
        _placeSecondOrder  = true;
        _secondLegTrade    = false;
        _firstLegTrade     = false;

        reqQrySendCustomResponse("",
            { "FilledQty:" + std::to_string(_tradedQ * 100) }, 0);
        onDefaultEvent();
    }
    else if (_secondLegTrade && _firstLegOrderFlag) {
        // L1 filled but cover legs not open yet
        if (!(wrapper(LEG_CALL).isOrderOpen() || wrapper(LEG_PUT).isOrderOpen())) {
            _firstLegOrderFlag = false;
            _placeAggressive   = true;
        }
        onDefaultEvent();
    }
}

void SBA::TLS::onOrderRmsReject(API2::OrderConfirmation& conf,
                                 API2::COMMON::OrderId*   orderId)
{
    DEBUG_MESSAGE(reqQryDebugLog(), "onOrderRmsReject: terminating");
    terminateStrategyComment(
        API2::CONSTANTS::RSP_StrategyComment_STRATEGY_ERROR_STATE);
}

// ─────────────────────────────────────────────────────────────────────────────
// getTodayDate
// ─────────────────────────────────────────────────────────────────────────────
std::string SBA::TLS::getTodayDate()
{
    time_t t = time(nullptr);
    tm*    n = localtime(&t);
    char   buf[9];
    strftime(buf, sizeof(buf), "%d%m%Y", n);
    return std::string(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// INVOKING API (optional)
// ─────────────────────────────────────────────────────────────────────────────
#if INVOKING_API
void SBA::TLS::runCashFut()
{
    invokingApi   = new INVOKING::InvokingApi(reqQryDebugLog());
    cashFutParams = new INVOKING::TwoLegArbitrage();

    API2::AccountDetail acc1, acc2;
    acc1.setPrimaryClientCode("AAA");
    acc2.setPrimaryClientCode("AW2");

    cashFutParams->setSymbolIdFirstLeg(15047177);
    cashFutParams->setSymbolIdSecondLeg(1503499);
    cashFutParams->setFirstLegAccountDetails(acc1);
    cashFutParams->setSecondLegAccountDetails(acc2);
    cashFutParams->setTransactionType(API2::CONSTANTS::CMD_TransactionType_NEW);
    cashFutParams->setQuantityFirstLeg(4);
    cashFutParams->setClientId(17);
    cashFutParams->setFirstLegMode(API2::CONSTANTS::CMD_OrderMode_BUY);
    cashFutParams->setParentStrategyId(_params.strategyId);
    cashFutParams->setThresholdQuantity_2(100);
    cashFutParams->setBidDifference(100);
    cashFutParams->setOpTicks(2);
    cashFutParams->setSqOff(3);
    cashFutParams->setMarketOrderPercent(200);
    cashFutParams->setPriceDifference(-305);

    if (!invokingApi->runStrategy(INVOKING::StrategyType_Cash_Fut, cashFutParams, &_child1))
        DEBUG_MESSAGE(reqQryDebugLog(), "Unable to run cash fut");
    else
        DEBUG_VARSHOW(reqQryDebugLog(), "Cash Fut child id: ", _child1);
}

void SBA::TLS::printChildPosition(API2::DATA_TYPES::STRATEGY_ID childId)
{
    if (reqQryStrategyInfo(childId) != nullptr) {
        DEBUG_VARSHOW(reqQryDebugLog(), "Child traded qty: ",
            reqQryStrategyInfo(childId)->getPosition(15047177, 0)->getTradedQty());
        DEBUG_VARSHOW(reqQryDebugLog(), "Child avg price: ",
            reqQryStrategyInfo(childId)->getPosition(15047177, 0)->getAvgTradedPrice());
    }
    else {
        DEBUG_VARSHOW(reqQryDebugLog(), "Child strategy not found: ", childId);
    }
}
#endif
