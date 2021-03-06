/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include "configuration.h"
#include "dcp/consumer.h"
#include "dcp/producer.h"
#include "dcpconnmap.h"
#include "ep_engine.h"

const uint32_t DcpConnMap::dbFileMem = 10 * 1024;
const uint16_t DcpConnMap::numBackfillsThreshold = 4096;
const uint8_t DcpConnMap::numBackfillsMemThreshold = 1;

class DcpConnMap::DcpConfigChangeListener : public ValueChangedListener {
public:
    DcpConfigChangeListener(DcpConnMap& connMap);
    virtual ~DcpConfigChangeListener() {
    }
    virtual void sizeValueChanged(const std::string& key, size_t value);

private:
    DcpConnMap& myConnMap;
};

DcpConnMap::DcpConnMap(EventuallyPersistentEngine &e)
    : ConnMap(e),
      aggrDcpConsumerBufferSize(0) {
    backfills.numActiveSnoozing = 0;
    updateMaxActiveSnoozingBackfills(engine.getEpStats().getMaxDataSize());
    minCompressionRatioForProducer.store(
                    engine.getConfiguration().getDcpMinCompressionRatio());

    // Note: these allocations are deleted by ~Configuration
    engine.getConfiguration().
        addValueChangedListener("dcp_consumer_process_buffered_messages_yield_limit",
                                new DcpConfigChangeListener(*this));
    engine.getConfiguration().
        addValueChangedListener("dcp_consumer_process_buffered_messages_batch_size",
                                new DcpConfigChangeListener(*this));
}

DcpConsumer *DcpConnMap::newConsumer(const void* cookie,
                                     const std::string &name)
{
    LockHolder lh(connsLock);

    std::string conn_name("eq_dcpq:");
    conn_name.append(name);

    const auto& iter = map_.find(cookie);
    if (iter != map_.end()) {
        iter->second->setDisconnect();
        LOG(EXTENSION_LOG_NOTICE,
            "Failed to create Dcp Consumer because connection "
            "(%p) already exists.", cookie);
        return nullptr;
    }

    /*
     *  If we request a connection of the same name then
     *  mark the existing connection as "want to disconnect".
     */
    for (const auto cookieToConn : map_) {
        if (cookieToConn.second->getName() == conn_name) {
            LOG(EXTENSION_LOG_NOTICE,
                "%s Disconnecting existing Dcp Consumer %p as it has the same "
                "name as a new connection %p",
                cookieToConn.second->logHeader(), cookieToConn.first, cookie);
            cookieToConn.second->setDisconnect();
        }
    }

    DcpConsumer *dcp = new DcpConsumer(engine, cookie, conn_name);
    connection_t dc(dcp);
    LOG(EXTENSION_LOG_INFO, "%s Connection created", dc->logHeader());
    map_[cookie] = dc;
    return dcp;

}

bool DcpConnMap::isPassiveStreamConnected_UNLOCKED(uint16_t vbucket) {
    for (const auto cookieToConn : map_) {
        DcpConsumer* dcpConsumer = dynamic_cast<DcpConsumer*>(
                cookieToConn.second.get());
        if (dcpConsumer && dcpConsumer->isStreamPresent(vbucket)) {
            LOG(EXTENSION_LOG_DEBUG, "(vb %d) A DCP passive stream "
                "is already exists for the vbucket in connection: %s",
                vbucket, dcpConsumer->logHeader());
            return true;
        }
    }
    return false;
}

ENGINE_ERROR_CODE DcpConnMap::addPassiveStream(ConnHandler& conn,
                                               uint32_t opaque,
                                               uint16_t vbucket,
                                               uint32_t flags)
{
    LockHolder lh(connsLock);
    /* Check if a stream (passive) for the vbucket is already present */
    if (isPassiveStreamConnected_UNLOCKED(vbucket)) {
        LOG(EXTENSION_LOG_WARNING, "%s (vb %d) Failing to add passive stream, "
            "as one already exists for the vbucket!",
            conn.logHeader(), vbucket);
        return ENGINE_KEY_EEXISTS;
    }

    return conn.addStream(opaque, vbucket, flags);
}

DcpProducer* DcpConnMap::newProducer(const void* cookie,
                                     const std::string& name,
                                     uint32_t flags,
                                     cb::const_byte_buffer jsonExtra) {
    LockHolder lh(connsLock);

    std::string conn_name("eq_dcpq:");
    conn_name.append(name);

    const auto& iter = map_.find(cookie);
    if (iter != map_.end()) {
        iter->second->setDisconnect();
        LOG(EXTENSION_LOG_NOTICE,
            "Failed to create Dcp Producer because connection "
            "(%p) already exists.", cookie);
        return nullptr;
    }

    /*
     *  If we request a connection of the same name then
     *  mark the existing connection as "want to disconnect".
     */
    for (const auto cookieToConn : map_) {
        if (cookieToConn.second->getName() == conn_name) {
            LOG(EXTENSION_LOG_NOTICE,
                "%s Disconnecting existing Dcp Producer %p as it has the same "
                "name as a new connection %p",
                cookieToConn.second->logHeader(), cookieToConn.first, cookie);
            cookieToConn.second->setDisconnect();
        }
    }

    DcpProducer* dcp = new DcpProducer(
            engine, cookie, conn_name, flags, jsonExtra, true /*startTask*/);
    LOG(EXTENSION_LOG_INFO, "%s Connection created", dcp->logHeader());
    map_[cookie] = dcp;

    return dcp;
}

void DcpConnMap::shutdownAllConnections() {
    LOG(EXTENSION_LOG_NOTICE, "Shutting down dcp connections!");

    if (connNotifier_ != NULL) {
        connNotifier_->stop();
        manageConnections();
    }

    // Take a copy of the connection map (under lock), then using the
    // copy iterate across closing all streams and cancelling any
    // tasks.
    // We do this so we don't hold the connsLock when calling
    // notifyPaused() on producer streams, as that would create a lock
    // cycle between connLock, worker thread lock and releaseLock.
    CookieToConnectionMap mapCopy;
    {
        LockHolder lh(connsLock);
        mapCopy = map_;
    }

    closeStreams(mapCopy);
    cancelTasks(mapCopy);
}

void DcpConnMap::vbucketStateChanged(uint16_t vbucket, vbucket_state_t state,
                                     bool closeInboundStreams) {
    LockHolder lh(connsLock);
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr->second.get());
        if (producer) {
            producer->closeStreamDueToVbStateChange(vbucket, state);
        } else if (closeInboundStreams) {
            static_cast<DcpConsumer *>(itr->second.get())->closeStreamDueToVbStateChange(
                    vbucket, state);
        }
    }
}

void DcpConnMap::closeStreamsDueToRollback(uint16_t vbucket) {
    LockHolder lh(connsLock);
    for (auto& pair : map_) {
        DcpProducer* producer = dynamic_cast<DcpProducer*>(pair.second.get());
        if (producer) {
            producer->closeStreamDueToRollback(vbucket);
        }
    }
}

bool DcpConnMap::handleSlowStream(uint16_t vbid,
                                  const std::string &name) {
    size_t lock_num = vbid % vbConnLockNum;
    std::lock_guard<SpinLock> lh(vbConnLocks[lock_num]);
    std::list<connection_t> &vb_conns = vbConns[vbid];

    std::list<connection_t>::iterator itr = vb_conns.begin();
    for (; itr != vb_conns.end(); ++itr) {
        DcpProducer* producer = static_cast<DcpProducer*> ((*itr).get());
        if (producer && producer->handleSlowStream(vbid, name)) {
            return true;
        }
    }
    return false;
}

void DcpConnMap::closeStreams(CookieToConnectionMap& map) {
    for (auto itr : map) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr.second.get());
        if (producer) {
            producer->closeAllStreams();
            producer->clearCheckpointProcessorTaskQueues();
            // The producer may be in EWOULDBLOCK (if it's idle), therefore
            // notify him to ensure the front-end connection can close the TCP
            // connection.
            producer->notifyPaused(/*schedule*/false);
        } else {
            auto* consumer = dynamic_cast<DcpConsumer*>(itr.second.get());
            if (consumer) {
                consumer->closeAllStreams();
                // The consumer may be in EWOULDBLOCK (if it's idle), therefore
                // notify him to ensure the front-end connection can close the
                // TCP connection.
                consumer->notifyPaused(/*schedule*/false);
            }
        }
    }
}

void DcpConnMap::cancelTasks(CookieToConnectionMap& map) {
    for (auto itr : map) {
        DcpConsumer* consumer = dynamic_cast<DcpConsumer*> (itr.second.get());
        if (consumer) {
            consumer->cancelTask();
        }
    }
}

void DcpConnMap::disconnect(const void *cookie) {
    // Move the connection matching this cookie from the map_
    // data structure (under connsLock).
    connection_t conn;
    {
        LockHolder lh(connsLock);
        std::map<const void*, connection_t>::iterator itr(map_.find(cookie));
        if (itr != map_.end()) {
            conn = itr->second;
            if (conn.get()) {
                conn->getLogger().log(EXTENSION_LOG_NOTICE,
                                      "Removing connection %p", cookie);
                conn->setDisconnect();
                map_.erase(itr);
            }
        }
    }

    // Note we shutdown the stream *not* under the connsLock; this is
    // because as part of closing a DcpConsumer stream we need to
    // acquire PassiveStream::buffer.bufMutex; and that could deadlock
    // in EPBucket::setVBucketState, via
    // PassiveStream::processBufferedMessages.
    if (conn) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (conn.get());
        if (producer) {
            producer->closeAllStreams();
            producer->clearCheckpointProcessorTaskQueues();
        } else {
            // Cancel consumer's processer task before closing all streams
            static_cast<DcpConsumer*>(conn.get())->cancelTask();
            static_cast<DcpConsumer*>(conn.get())->closeAllStreams();
        }
    }

    // Finished disconnecting the stream; add it to the
    // deadConnections list.
    if (conn) {
        LockHolder lh(connsLock);
        deadConnections.push_back(conn);
    }
}

void DcpConnMap::manageConnections() {
    std::list<connection_t> release;
    std::list<connection_t> toNotify;
    {
        LockHolder lh(connsLock);
        while (!deadConnections.empty()) {
            connection_t conn = deadConnections.front();
            release.push_back(conn);
            deadConnections.pop_front();
        }

        // Collect the list of connections that need to be signaled.
        std::map<const void*, connection_t>::iterator iter;
        for (iter = map_.begin(); iter != map_.end(); ++iter) {
            connection_t conn = iter->second;
            if (conn.get() && (conn->isPaused() || conn->doDisconnect()) &&
                conn->isReserved()) {
                /**
                 * Note: We want to send a notify even if we have sent one
                 * previously i.e. tp->sentNotify() == true.  The reason for this
                 * is manageConnections is used to notify idle connections once a
                 * second.  This results in the step function being invoked,
                 * which in turn may result in a dcp noop message being sent.
                 */
                toNotify.push_back(iter->second);
            }
        }
    }

    LockHolder rlh(releaseLock);
    std::list<connection_t>::iterator it;
    for (it = toNotify.begin(); it != toNotify.end(); ++it) {
        if ((*it).get() && (*it)->isReserved()) {
            engine.notifyIOComplete((*it)->getCookie(), ENGINE_SUCCESS);
        }
    }

    while (!release.empty()) {
        connection_t conn = release.front();
        conn->releaseReference();
        release.pop_front();
        removeVBConnections(conn);
    }
}

void DcpConnMap::removeVBConnections(connection_t &conn) {
    DcpProducer *tp = dynamic_cast<DcpProducer*>(conn.get());
    if (!tp) {
        return;
    }

    DcpProducer *prod = static_cast<DcpProducer*>(tp);
    for (const auto vbid : prod->getVBVector()) {
        size_t lock_num = vbid % vbConnLockNum;
        std::lock_guard<SpinLock> lh(vbConnLocks[lock_num]);
        std::list<connection_t> &vb_conns = vbConns[vbid];
        std::list<connection_t>::iterator itr = vb_conns.begin();
        for (; itr != vb_conns.end(); ++itr) {
            if (conn->getCookie() == (*itr)->getCookie()) {
                vb_conns.erase(itr);
                break;
            }
        }
    }
}

void DcpConnMap::notifyVBConnections(uint16_t vbid, uint64_t bySeqno) {
    size_t lock_num = vbid % vbConnLockNum;
    std::lock_guard<SpinLock> lh(vbConnLocks[lock_num]);

    std::list<connection_t> &conns = vbConns[vbid];
    std::list<connection_t>::iterator it = conns.begin();
    for (; it != conns.end(); ++it) {
        DcpProducer *conn = static_cast<DcpProducer*>((*it).get());
        conn->notifySeqnoAvailable(vbid, bySeqno);
    }
}

void DcpConnMap::notifyBackfillManagerTasks() {
    LockHolder lh(connsLock);
    std::map<const void*, connection_t>::iterator itr = map_.begin();
    for (; itr != map_.end(); ++itr) {
        DcpProducer* producer = dynamic_cast<DcpProducer*> (itr->second.get());
        if (producer) {
            producer->notifyBackfillManager();
        }
    }
}

bool DcpConnMap::canAddBackfillToActiveQ()
{
    std::lock_guard<std::mutex> lh(backfills.mutex);
    if (backfills.numActiveSnoozing < backfills.maxActiveSnoozing) {
        ++backfills.numActiveSnoozing;
        return true;
    }
    return false;
}

void DcpConnMap::decrNumActiveSnoozingBackfills()
{
    {
        std::lock_guard<std::mutex> lh(backfills.mutex);
        if (backfills.numActiveSnoozing > 0) {
            --backfills.numActiveSnoozing;
            return;
        }
    }
    LOG(EXTENSION_LOG_WARNING, "ActiveSnoozingBackfills already zero!!!");
}

void DcpConnMap::updateMaxActiveSnoozingBackfills(size_t maxDataSize)
{
    double numBackfillsMemThresholdPercent =
                         static_cast<double>(numBackfillsMemThreshold)/100;
    size_t max = maxDataSize * numBackfillsMemThresholdPercent / dbFileMem;

    uint16_t newMaxActive;
    {
        std::lock_guard<std::mutex> lh(backfills.mutex);
        /* We must have atleast one active/snoozing backfill */
        backfills.maxActiveSnoozing =
                std::max(static_cast<size_t>(1),
                         std::min(max, static_cast<size_t>(numBackfillsThreshold)));
        newMaxActive = backfills.maxActiveSnoozing;
    }
    LOG(EXTENSION_LOG_DEBUG, "Max active snoozing backfills set to %" PRIu16,
        newMaxActive);
}

void DcpConnMap::addStats(ADD_STAT add_stat, const void *c) {
    LockHolder lh(connsLock);
    add_casted_stat("ep_dcp_dead_conn_count", deadConnections.size(), add_stat,
                    c);
}

void DcpConnMap::updateMinCompressionRatioForProducers(float value) {
    minCompressionRatioForProducer.store(value);
}

float DcpConnMap::getMinCompressionRatio() {
    return minCompressionRatioForProducer.load();
}

DcpConnMap::DcpConfigChangeListener::DcpConfigChangeListener(DcpConnMap& connMap)
    : myConnMap(connMap){}

void DcpConnMap::DcpConfigChangeListener::sizeValueChanged(const std::string &key,
                                                           size_t value) {
    if (key == "dcp_consumer_process_buffered_messages_yield_limit") {
        myConnMap.consumerYieldConfigChanged(value);
    } else if (key == "dcp_consumer_process_buffered_messages_batch_size") {
        myConnMap.consumerBatchSizeConfigChanged(value);
    }
}

/*
 * Find all DcpConsumers and set the yield threshold
 */
void DcpConnMap::consumerYieldConfigChanged(size_t newValue) {
    LockHolder lh(connsLock);
    for (const auto cookieToConn : map_) {
        DcpConsumer* dcpConsumer = dynamic_cast<DcpConsumer*>(
                cookieToConn.second.get());
        if (dcpConsumer) {
            dcpConsumer->setProcessorYieldThreshold(newValue);
        }
    }
}

/*
 * Find all DcpConsumers and set the processor batchsize
 */
void DcpConnMap::consumerBatchSizeConfigChanged(size_t newValue) {
    LockHolder lh(connsLock);
    for (const auto cookieToConn : map_) {
        DcpConsumer* dcpConsumer = dynamic_cast<DcpConsumer*>(
                cookieToConn.second.get());
        if (dcpConsumer) {
            dcpConsumer->setProcessBufferedMessagesBatchSize(newValue);
        }
    }
}

connection_t DcpConnMap::findByName(const std::string& name) {
    LockHolder lh(connsLock);
    for (const auto cookieToConn : map_) {
        // If the connection is NOT about to be disconnected
        // and the names match
        if (!cookieToConn.second->doDisconnect() &&
            cookieToConn.second->getName() == name) {
            return cookieToConn.second.get();
        }
    }
    return connection_t(nullptr);
}
