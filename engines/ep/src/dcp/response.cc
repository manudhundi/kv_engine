/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2014 Couchbase, Inc
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

#include "dcp/response.h"

#include <xattr/utils.h>

/*
 * These constants are calculated from the size of the packets that are
 * created by each message when it gets sent over the wire. The packet
 * structures are located in the protocol_binary.h file in the memcached
 * project.
 */

const uint32_t StreamRequest::baseMsgBytes = 72;
const uint32_t AddStreamResponse::baseMsgBytes = 28;
const uint32_t SnapshotMarkerResponse::baseMsgBytes = 24;
const uint32_t SetVBucketStateResponse::baseMsgBytes = 24;
const uint32_t StreamEndResponse::baseMsgBytes = 28;
const uint32_t SetVBucketState::baseMsgBytes = 25;
const uint32_t SnapshotMarker::baseMsgBytes = 44;

const char* DcpResponse::to_string() const {
    switch (event_) {
    case Event::Mutation:
        return "mutation";
    case Event::Deletion:
        return "deletion";
    case Event::Expiration:
        return "expiration";
    case Event::Flush:
        return "flush";
    case Event::SetVbucket:
        return "set vbucket";
    case Event::StreamReq:
        return "stream req";
    case Event::StreamEnd:
        return "stream end";
    case Event::SnapshotMarker:
        return "snapshot marker";
    case Event::AddStream:
        return "add stream";
    case Event::SystemEvent:
        return "system event";
    }
    throw std::logic_error(
        "DcpResponse::to_string(): " + std::to_string(int(event_)));
}

uint32_t MutationResponse::getMessageSize() const {
    const uint32_t header = item_->isDeleted() ? deletionBaseMsgBytes :
                    mutationBaseMsgBytes;

    uint32_t body = item_->getKey().size();
    uint32_t sz = 0;

    // If the item has xattributes then calculate the size of the xattributes
    if (mcbp::datatype::is_xattr(item_->getDataType())) {
        auto root = reinterpret_cast<const char*>(item_->getData());
        const cb::const_char_buffer buffer{root, item_->getValue()->vlength()};
        sz = cb::xattr::get_body_offset(buffer);
    }

    // If need to include the xattributes in the response
    if (includeXattributes == IncludeXattrs::Yes) {
        // Include the xattributes size, but not the value size
        body += sz;
    }

    // If need to include the value in the response
    if (includeValue == IncludeValue::Yes) {
        body += (item_->getNBytes() - sz);
    }

    // Check to see if we need to include the extended meta data size.
    if (emd) {
        body += emd->getExtMeta().second;
    }

    return header + body;
}
