/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "PrivateDnsConfiguration.h"
#include "tests/dns_responder/dns_responder.h"
#include "tests/dns_responder/dns_tls_frontend.h"
#include "tests/resolv_test_utils.h"

namespace android::net {

using namespace std::chrono_literals;

class PrivateDnsConfigurationTest : public ::testing::Test {
  public:
    static void SetUpTestSuite() {
        // stopServer() will be called in their destructor.
        ASSERT_TRUE(tls1.startServer());
        ASSERT_TRUE(tls2.startServer());
        ASSERT_TRUE(backend.startServer());
    }

    void SetUp() {
        mPdc.setObserver(&mObserver);

        // The default and sole action when the observer is notified of onValidationStateUpdate.
        // Don't override the action. In other words, don't use WillOnce() or WillRepeatedly()
        // when mObserver.onValidationStateUpdate is expected to be called, like:
        //
        //   EXPECT_CALL(mObserver, onValidationStateUpdate).WillOnce(Return());
        //
        // This is to ensure that tests can monitor how many validation threads are running. Tests
        // must wait until every validation thread finishes.
        ON_CALL(mObserver, onValidationStateUpdate)
                .WillByDefault([&](const std::string& server, Validation validation, uint32_t) {
                    if (validation == Validation::in_process) {
                        mObserver.runningThreads++;
                    } else if (validation == Validation::success ||
                               validation == Validation::fail) {
                        mObserver.runningThreads--;
                    }
                    std::lock_guard guard(mObserver.lock);
                    mObserver.serverStateMap[server] = validation;
                });
    }

  protected:
    class MockObserver : public PrivateDnsConfiguration::Observer {
      public:
        MOCK_METHOD(void, onValidationStateUpdate,
                    (const std::string& serverIp, Validation validation, uint32_t netId),
                    (override));

        std::map<std::string, Validation> getServerStateMap() const {
            std::lock_guard guard(lock);
            return serverStateMap;
        }

        void removeFromServerStateMap(const std::string& server) {
            std::lock_guard guard(lock);
            if (const auto it = serverStateMap.find(server); it != serverStateMap.end())
                serverStateMap.erase(it);
        }

        // The current number of validation threads running.
        std::atomic<int> runningThreads = 0;

        mutable std::mutex lock;
        std::map<std::string, Validation> serverStateMap GUARDED_BY(lock);
    };

    void expectPrivateDnsStatus(PrivateDnsMode mode) {
        const PrivateDnsStatus status = mPdc.getStatus(kNetId);
        EXPECT_EQ(status.mode, mode);

        std::map<std::string, Validation> serverStateMap;
        for (const auto& [server, validation] : status.serversMap) {
            serverStateMap[ToString(&server.ss)] = validation;
        }
        EXPECT_EQ(serverStateMap, mObserver.getServerStateMap());
    }

    static constexpr uint32_t kNetId = 30;
    static constexpr uint32_t kMark = 30;
    static constexpr char kBackend[] = "127.0.2.1";
    static constexpr char kServer1[] = "127.0.2.2";
    static constexpr char kServer2[] = "127.0.2.3";

    MockObserver mObserver;
    PrivateDnsConfiguration mPdc;

    // TODO: Because incorrect CAs result in validation failed in strict mode, have
    // PrivateDnsConfiguration run mocked code rather than DnsTlsTransport::validate().
    inline static test::DnsTlsFrontend tls1{kServer1, "853", kBackend, "53"};
    inline static test::DnsTlsFrontend tls2{kServer2, "853", kBackend, "53"};
    inline static test::DNSResponder backend{kBackend, "53"};
};

TEST_F(PrivateDnsConfigurationTest, ValidationSuccess) {
    testing::InSequence seq;
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::success, kNetId));

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
}

TEST_F(PrivateDnsConfigurationTest, ValidationFail_Opportunistic) {
    ASSERT_TRUE(backend.stopServer());

    testing::InSequence seq;
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));

    EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

    // Strictly wait for all of the validation finish; otherwise, the test can crash somehow.
    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
    ASSERT_TRUE(backend.startServer());
}

TEST_F(PrivateDnsConfigurationTest, ValidationBlock) {
    backend.setDeferredResp(true);

    // onValidationStateUpdate() is called in sequence.
    {
        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 1; }));
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer2, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer2}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 2; }));
        mObserver.removeFromServerStateMap(kServer1);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // No duplicate validation as long as not in OFF mode; otherwise, an unexpected
        // onValidationStateUpdate() will be caught.
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1, kServer2}, {}, {}), 0);
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer2}, {}, {}), 0);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        // The status keeps unchanged if pass invalid arguments.
        EXPECT_EQ(mPdc.set(kNetId, kMark, {"invalid_addr"}, {}, {}), -EINVAL);
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
    }

    // The update for |kServer1| will be Validation::fail because |kServer1| is not an expected
    // server for the network.
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));
    EXPECT_CALL(mObserver, onValidationStateUpdate(kServer2, Validation::success, kNetId));
    backend.setDeferredResp(false);

    ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));

    // kServer1 is not a present server and thus should not be available from
    // PrivateDnsConfiguration::getStatus().
    mObserver.removeFromServerStateMap(kServer1);

    expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);
}

TEST_F(PrivateDnsConfigurationTest, Validation_NetworkDestroyedOrOffMode) {
    for (const std::string_view config : {"OFF", "NETWORK_DESTROYED"}) {
        SCOPED_TRACE(config);
        backend.setDeferredResp(true);

        testing::InSequence seq;
        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::in_process, kNetId));
        EXPECT_EQ(mPdc.set(kNetId, kMark, {kServer1}, {}, {}), 0);
        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 1; }));
        expectPrivateDnsStatus(PrivateDnsMode::OPPORTUNISTIC);

        if (config == "OFF") {
            EXPECT_EQ(mPdc.set(kNetId, kMark, {}, {}, {}), 0);
        } else if (config == "NETWORK_DESTROYED") {
            mPdc.clear(kNetId);
        }

        EXPECT_CALL(mObserver, onValidationStateUpdate(kServer1, Validation::fail, kNetId));
        backend.setDeferredResp(false);

        ASSERT_TRUE(PollForCondition([&]() { return mObserver.runningThreads == 0; }));
        mObserver.removeFromServerStateMap(kServer1);
        expectPrivateDnsStatus(PrivateDnsMode::OFF);
    }
}

TEST_F(PrivateDnsConfigurationTest, NoValidation) {
    // If onValidationStateUpdate() is called, the test will fail with uninteresting mock
    // function calls in the end of the test.

    const auto expectStatus = [&]() {
        const PrivateDnsStatus status = mPdc.getStatus(kNetId);
        EXPECT_EQ(status.mode, PrivateDnsMode::OFF);
        EXPECT_THAT(status.serversMap, testing::IsEmpty());
    };

    EXPECT_EQ(mPdc.set(kNetId, kMark, {"invalid_addr"}, {}, {}), -EINVAL);
    expectStatus();

    EXPECT_EQ(mPdc.set(kNetId, kMark, {}, {}, {}), 0);
    expectStatus();
}

TEST_F(PrivateDnsConfigurationTest, ServerIdentity_Comparison) {
    using ServerIdentity = PrivateDnsConfiguration::ServerIdentity;

    DnsTlsServer server(netdutils::IPSockAddr::toIPSockAddr("127.0.0.1", 853));
    server.name = "dns.example.com";
    server.protocol = 1;

    // Different IP address (port is ignored).
    DnsTlsServer other = server;
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.ss = netdutils::IPSockAddr::toIPSockAddr("127.0.0.1", 5353);
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.ss = netdutils::IPSockAddr::toIPSockAddr("127.0.0.2", 853);
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));

    // Different provider hostname.
    other = server;
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.name = "other.example.com";
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));
    other.name = "";
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));

    // Different protocol.
    other = server;
    EXPECT_EQ(ServerIdentity(server), ServerIdentity(other));
    other.protocol++;
    EXPECT_NE(ServerIdentity(server), ServerIdentity(other));
}

// TODO: add ValidationFail_Strict test.

}  // namespace android::net
