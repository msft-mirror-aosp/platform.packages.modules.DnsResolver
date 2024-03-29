/**
 * Copyright (c) 2019, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <condition_variable>
#include <map>
#include <utility>

#include <android-base/thread_annotations.h>

#include "base_metrics_listener.h"

enum ExpectNat64PrefixStatus : bool {
    EXPECT_FOUND,
    EXPECT_NOT_FOUND,
};

namespace android {
namespace net {
namespace metrics {

// TODO: Perhaps use condition variable but continually polling.
// TODO: Perhaps create a queue to monitor the event changes. That improves the unit test which can
// verify the event count, the event change order, and so on.
class DnsMetricsListener : public BaseMetricsListener {
  public:
    DnsMetricsListener() = delete;
    DnsMetricsListener(int32_t netId) : mNetId(netId){};

    // Override DNS metrics event(s).
    android::binder::Status onNat64PrefixEvent(int32_t netId, bool added,
                                               const std::string& prefixString,
                                               int32_t /*prefixLength*/) override;

    android::binder::Status onPrivateDnsValidationEvent(int32_t netId,
                                                        const ::android::String16& ipAddress,
                                                        const ::android::String16& /*hostname*/,
                                                        bool validated) override;

    // Wait for expected NAT64 prefix status until timeout.
    bool waitForNat64Prefix(ExpectNat64PrefixStatus status,
                            std::chrono::milliseconds timeout) const;

    // Wait for the expected private DNS validation result until timeout.
    bool waitForPrivateDnsValidation(const std::string& serverAddr, const bool validated);

  private:
    typedef std::pair<int32_t, std::string> ServerKey;

    // Search mValidationRecords. Return true if |key| exists and its value is equal to
    // |value|, and then remove it; otherwise, return false.
    bool findAndRemoveValidationRecord(const ServerKey& key, const bool value) REQUIRES(mMutex);

    // Monitor the event which was fired on specific network id.
    const int32_t mNetId;

    // The NAT64 prefix of the network |mNetId|. It is updated by the event onNat64PrefixEvent().
    std::string mNat64Prefix GUARDED_BY(mMutex);

    // Used to store the data from onPrivateDnsValidationEvent.
    std::map<ServerKey, bool> mValidationRecords GUARDED_BY(mMutex);

    mutable std::mutex mMutex;
    std::condition_variable mCv;
};

}  // namespace metrics
}  // namespace net
}  // namespace android
