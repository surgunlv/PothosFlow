// Copyright (c) 2014-2017 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include "TopologyEval.hpp"
#include "BlockEval.hpp"
#include <Pothos/Framework.hpp>
#include <algorithm> //std::remove

TopologyEval::TopologyEval(void):
    _topology(new Pothos::Topology()),
    _failureState(false),
    _logger(Poco::Logger::get("PothosGui.TopologyEval"))
{
    return;
}

TopologyEval::~TopologyEval(void)
{
    delete _topology;
}

void TopologyEval::acceptConnectionInfo(const ConnectionInfos &info)
{
    _newConnectionInfo = info;
}

void TopologyEval::acceptBlockEvals(const std::map<size_t, std::shared_ptr<BlockEval>> &info)
{
    _newBlockEvals = info;
}

void TopologyEval::update(void)
{
    if (this->isFailureState()) return;

    const auto removedConnections = diffConnectionInfos(_currentConnections, _newConnectionInfo);
    const auto addedConnections = diffConnectionInfos(_newConnectionInfo, _currentConnections);
    if ((removedConnections.size() + addedConnections.size()) == 0) return; //nothing to do

    //remove connections from the topology
    for (const auto &conn : removedConnections)
    {
        //locate the src and dst block evals
        assert(_lastBlockEvals.count(conn.srcBlockUID) != 0);
        assert(_lastBlockEvals.count(conn.dstBlockUID) != 0);
        auto src = _lastBlockEvals.at(conn.srcBlockUID);
        auto dst = _lastBlockEvals.at(conn.dstBlockUID);

        //dont include error or disabled blocks in the active flows
        if (not src->isReady()) continue;
        if (not dst->isReady()) continue;

        //dont include connections to non-existent endpoints
        if (not src->portExists(conn.srcPort, false)) continue;
        if (not dst->portExists(conn.dstPort, true)) continue;

        //attempt to create the connection
        try
        {
            _topology->disconnect(
                src->getProxyBlock(), conn.srcPort.toStdString(),
                dst->getProxyBlock(), conn.dstPort.toStdString());
            std::remove(_currentConnections.begin(), _currentConnections.end(), conn);
        }
        catch (const Pothos::Exception &ex)
        {
            _logger.error("Failed to disconnect: %s", ex.displayText());
            _failureState = true;
            return;
        }
    }

    //create new connections
    for (const auto &conn : addedConnections)
    {
        //locate the src and dst block evals
        assert(_newBlockEvals.count(conn.srcBlockUID) != 0);
        assert(_newBlockEvals.count(conn.dstBlockUID) != 0);
        auto src = _newBlockEvals.at(conn.srcBlockUID);
        auto dst = _newBlockEvals.at(conn.dstBlockUID);

        //dont include error or disabled blocks in the active flows
        if (not src->isReady()) continue;
        if (not dst->isReady()) continue;

        //dont include connections to non-existent endpoints
        if (not src->portExists(conn.srcPort, false)) continue;
        if (not dst->portExists(conn.dstPort, true)) continue;

        //attempt to create the connection
        try
        {
            _topology->connect(
                src->getProxyBlock(), conn.srcPort.toStdString(),
                dst->getProxyBlock(), conn.dstPort.toStdString());
            std::remove(_currentConnections.begin(), _currentConnections.end(), conn);
            _currentConnections.push_back(conn);
        }
        catch (const Pothos::Exception &ex)
        {
            _logger.error("Failed to connect: %s", ex.displayText());
            _failureState = true;
            return;
        }
    }

    //commit after changes
    try
    {
        _topology->commit();
        //stash data for the current state
        _lastBlockEvals = _newBlockEvals;
        _lastConnectionInfo = _newConnectionInfo;
    }
    catch (const Pothos::Exception &ex)
    {
        _logger.error("Failed to commit: %s", ex.displayText());
        _failureState = true;
        return;
    }
}

bool operator==(const ConnectionInfo &lhs, const ConnectionInfo &rhs)
{
    return
        (lhs.srcBlockUID == rhs.srcBlockUID) and
        (lhs.dstBlockUID == rhs.dstBlockUID) and
        (lhs.srcPort == rhs.srcPort) and
        (lhs.dstPort == rhs.dstPort);
}

ConnectionInfos diffConnectionInfos(const ConnectionInfos &in0, const ConnectionInfos &in1)
{
    ConnectionInfos out;
    for (const auto &elem0 : in0)
    {
        for (const auto &elem1 : in1)
        {
            if (elem0 == elem1) goto next_elem0;
        }
        out.push_back(elem0);
        next_elem0: continue;
    }
    return out;
}
