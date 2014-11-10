// Copyright (c) 2014-2014 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "EvalEngineImpl.hpp"
#include "EvalEngine.hpp"
#include "BlockEval.hpp"
#include "ThreadPoolEval.hpp"
#include "EnvironmentEval.hpp"
#include "TopologyEval.hpp"
#include "GraphObjects/GraphBlock.hpp"
#include <Pothos/Framework/Topology.hpp>
#include <QThread>
#include <QTimer>
#include <QAbstractEventDispatcher>
#include <cassert>

static const int MONITOR_INTERVAL_MS = 1000;

EvalEngineImpl::EvalEngineImpl(void):
    _requireEval(false),
    _monitorTimer(new QTimer(this))
{
    qRegisterMetaType<BlockInfo>("BlockInfo");
    qRegisterMetaType<BlockInfos>("BlockInfos");
    qRegisterMetaType<ConnectionInfo>("ConnectionInfo");
    qRegisterMetaType<ConnectionInfos>("ConnectionInfos");
    qRegisterMetaType<ZoneInfos>("ZoneInfos");

    connect(_monitorTimer, SIGNAL(timeout(void)), this, SLOT(handleMonitorTimeout(void)));
    _monitorTimer->setInterval(MONITOR_INTERVAL_MS);
}

EvalEngineImpl::~EvalEngineImpl(void)
{
    return;
}

void EvalEngineImpl::submitActivateTopology(const bool enable)
{
    //make a new topology evaluator only if enabled and DNE
    if (enable and not _topologyEval)
    {
        _topologyEval.reset(new TopologyEval());
        _requireEval = true;
        this->evaluate();
    }

    //if disabled, clear the current evaluator if present
    if (not enable) _topologyEval.reset();
}

void EvalEngineImpl::submitBlock(const BlockInfo &info)
{
    _blockInfo[info.block.data()] = info;
    _requireEval = true;
    this->evaluate();
}

void EvalEngineImpl::submitTopology(const BlockInfos &blockInfos, const ConnectionInfos &connections)
{
    _blockInfo = blockInfos;
    _connectionInfo = connections;
    _requireEval = true;
    this->evaluate();
}

void EvalEngineImpl::submitZoneInfo(const ZoneInfos &info)
{
    _zoneInfo = info;
    _requireEval = true;
    this->evaluate();
}

std::string EvalEngineImpl::getTopologyDotMarkup(const bool arg)
{
    //have to do this in case this call compressed an eval-worthy event
    this->evaluate();

    if (not _topologyEval) return "";
    return _topologyEval->getTopology()->toDotMarkup(arg);
}

void EvalEngineImpl::handleMonitorTimeout(void)
{
    //cause periodic re-eval to deal with errors
    _requireEval = true;
    this->evaluate();
}

void EvalEngineImpl::evaluate(void)
{
    //Do not evaluate when there are pending events in the queue.
    //Evaluate only after all events received - AKA event compression.
    if (this->thread()->eventDispatcher()->hasPendingEvents()) return;

    //Only evaluate if require evaluate was flagged by a slot
    if (not _requireEval) return;
    _requireEval = false;

    std::map<GraphBlock *, std::shared_ptr<BlockEval>> newBlockEvals;
    std::map<QString, std::shared_ptr<ThreadPoolEval>> newThreadPoolEvals;
    std::map<HostProcPair, std::shared_ptr<EnvironmentEval>> newEnvironmentEvals;

    //merge in the block info
    for (const auto &blockInfoPair : _blockInfo)
    {
        auto blockPtr = blockInfoPair.first;
        const auto &blockInfo = blockInfoPair.second;
        const auto &zone = blockInfo.zone;

        //extract the configuration for this zone
        Poco::JSON::Object::Ptr config;
        {
            auto it = _zoneInfo.find(zone);
            if (it != _zoneInfo.end()) config = it->second;
        }
        const auto hostProcKey = EnvironmentEval::getHostProcFromConfig(zone, config);

        //copy the block eval or make a new one
        auto &blockEval = newBlockEvals[blockPtr];
        if (not blockEval)
        {
            auto it = _blockEvals.find(blockPtr);
            if (it != _blockEvals.end()) blockEval = it->second;
            else blockEval.reset(new BlockEval());
        }

        //copy the thread pool or make a new one
        auto &threadPoolEval = newThreadPoolEvals[zone];
        if (not threadPoolEval)
        {
            auto it = _threadPoolEvals.find(zone);
            if (it != _threadPoolEvals.end() and not it->second->isFailureState()) threadPoolEval = _threadPoolEvals.at(zone);
            else threadPoolEval.reset(new ThreadPoolEval());
        }

        //copy the eval environment or make a new one
        auto &envEval = newEnvironmentEvals[hostProcKey];
        if (not envEval)
        {
            auto it = _environmentEvals.find(hostProcKey);
            if (it != _environmentEvals.end() and not it->second->isFailureState()) envEval = it->second;
            else envEval.reset(new EnvironmentEval());
        }

        //pass config into the environment
        assert(envEval);
        envEval->acceptConfig(zone, config);

        //pass config and env into thread pool
        assert(threadPoolEval);
        threadPoolEval->acceptConfig(config);
        threadPoolEval->acceptEnvironment(envEval);

        //pass info, env, and thread pool into block
        assert(blockEval);
        blockEval->acceptInfo(blockInfo);
        blockEval->acceptThreadPool(threadPoolEval);
        blockEval->acceptEnvironment(envEval);
    }

    //swap in the latest engines that are in-use
    _blockEvals = newBlockEvals;
    _threadPoolEvals = newThreadPoolEvals;
    _environmentEvals = newEnvironmentEvals;

    //1) update all environments in case there were changes
    for (auto &pair : _environmentEvals) pair.second->update();
    //2) update all thread pools in case there were changes
    for (auto &pair : _threadPoolEvals) pair.second->update();
    //3) update all the blocks in case there were changes
    for (auto &pair : _blockEvals) pair.second->update();
    //4) update topology when present (activation mode)
    if (_topologyEval)
    {
        _topologyEval->acceptConnectionInfo(_connectionInfo);
        _topologyEval->acceptBlockEvals(_blockEvals);
        _topologyEval->update();
    }
}