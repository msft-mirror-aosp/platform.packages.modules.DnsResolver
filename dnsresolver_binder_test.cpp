/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <netdb.h>

#include <iostream>
#include <vector>

#include <android-base/strings.h>
#include <android/net/IDnsResolver.h>
#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>
#include <netdutils/Stopwatch.h>

#include "tests/dns_metrics_listener/base_metrics_listener.h"
#include "tests/dns_metrics_listener/test_metrics.h"

#include "ResolverStats.h"
#include "dns_responder.h"
#include "dns_responder_client.h"

namespace binder = android::binder;

using android::IBinder;
using android::IServiceManager;
using android::ProcessState;
using android::sp;
using android::String16;
using android::String8;
using android::net::IDnsResolver;
using android::net::ResolverParamsParcel;
using android::net::ResolverStats;
using android::net::metrics::INetdEventListener;
using android::net::metrics::TestOnDnsEvent;
using android::netdutils::Stopwatch;

// TODO: make this dynamic and stop depending on implementation details.
// Sync from TEST_NETID in dns_responder_client.cpp as resolv_integration_test.cpp does.
constexpr int TEST_NETID = 30;

class DnsResolverBinderTest : public ::testing::Test {
  public:
    DnsResolverBinderTest() {
        sp<IServiceManager> sm = android::defaultServiceManager();
        sp<IBinder> binder = sm->getService(String16("dnsresolver"));
        if (binder != nullptr) {
            mDnsResolver = android::interface_cast<IDnsResolver>(binder);
        }
        // This could happen when the test isn't running as root, or if netd isn't running.
        assert(nullptr != mDnsResolver.get());
        // Create cache for test
        mDnsResolver->createNetworkCache(TEST_NETID);
    }

    ~DnsResolverBinderTest() {
        // Destroy cache for test
        mDnsResolver->destroyNetworkCache(TEST_NETID);
    }

  protected:
    sp<IDnsResolver> mDnsResolver;
};

class TimedOperation : public Stopwatch {
  public:
    explicit TimedOperation(const std::string& name) : mName(name) {}
    virtual ~TimedOperation() {
        std::cerr << "    " << mName << ": " << timeTakenUs() << "us" << std::endl;
    }

  private:
    std::string mName;
};

namespace {

// TODO: Convert tests to ResolverParamsParcel and delete this stub.
ResolverParamsParcel makeResolverParamsParcel(int netId, const std::vector<int>& params,
                                              const std::vector<std::string>& servers,
                                              const std::vector<std::string>& domains,
                                              const std::string& tlsHostname,
                                              const std::vector<std::string>& tlsServers) {
    using android::net::IDnsResolver;
    ResolverParamsParcel paramsParcel;

    paramsParcel.netId = netId;
    paramsParcel.sampleValiditySeconds = params[IDnsResolver::RESOLVER_PARAMS_SAMPLE_VALIDITY];
    paramsParcel.successThreshold = params[IDnsResolver::RESOLVER_PARAMS_SUCCESS_THRESHOLD];
    paramsParcel.minSamples = params[IDnsResolver::RESOLVER_PARAMS_MIN_SAMPLES];
    paramsParcel.maxSamples = params[IDnsResolver::RESOLVER_PARAMS_MAX_SAMPLES];
    if (params.size() > IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC) {
        paramsParcel.baseTimeoutMsec = params[IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC];
    } else {
        paramsParcel.baseTimeoutMsec = 0;
    }
    if (params.size() > IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT) {
        paramsParcel.retryCount = params[IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT];
    } else {
        paramsParcel.retryCount = 0;
    }
    paramsParcel.servers = servers;
    paramsParcel.domains = domains;
    paramsParcel.tlsName = tlsHostname;
    paramsParcel.tlsServers = tlsServers;
    paramsParcel.tlsFingerprints = {};

    return paramsParcel;
}

}  // namespace

TEST_F(DnsResolverBinderTest, IsAlive) {
    TimedOperation t("isAlive RPC");
    bool isAlive = false;
    mDnsResolver->isAlive(&isAlive);
    ASSERT_TRUE(isAlive);
}

TEST_F(DnsResolverBinderTest, RegisterEventListener_NullListener) {
    android::binder::Status status = mDnsResolver->registerEventListener(
            android::interface_cast<INetdEventListener>(nullptr));
    ASSERT_FALSE(status.isOk());
    ASSERT_EQ(EINVAL, status.serviceSpecificErrorCode());
}

TEST_F(DnsResolverBinderTest, RegisterEventListener_DuplicateSubscription) {
    class DummyListener : public android::net::metrics::BaseMetricsListener {};

    // Expect to subscribe successfully.
    android::sp<DummyListener> dummyListener = new DummyListener();
    android::binder::Status status = mDnsResolver->registerEventListener(
            android::interface_cast<INetdEventListener>(dummyListener));
    ASSERT_TRUE(status.isOk()) << status.exceptionMessage();

    // Expect to subscribe failed with registered listener instance.
    status = mDnsResolver->registerEventListener(
            android::interface_cast<INetdEventListener>(dummyListener));
    ASSERT_FALSE(status.isOk());
    ASSERT_EQ(EEXIST, status.serviceSpecificErrorCode());
}

// TODO: Move this test to resolv_integration_test.cpp
TEST_F(DnsResolverBinderTest, RegisterEventListener_onDnsEvent) {
    // The test configs are used to trigger expected events. The expected results are defined in
    // expectedResults.
    static const struct TestConfig {
        std::string hostname;
        int returnCode;
    } testConfigs[] = {
            {"hi", 0 /*success*/},
            {"nonexistent", EAI_NODATA},
    };

    // The expected results define expected event content for test verification.
    static const std::vector<TestOnDnsEvent::TestResult> expectedResults = {
            {TEST_NETID, INetdEventListener::EVENT_GETADDRINFO, 0 /*success*/, 1, "hi", "1.2.3.4"},
            {TEST_NETID, INetdEventListener::EVENT_GETADDRINFO, EAI_NODATA, 0, "nonexistent", ""},
    };

    // Start the Binder thread pool.
    // TODO: Consider doing this once if there has another event listener unit test.
    android::ProcessState::self()->startThreadPool();

    // Setup network.
    // TODO: Setup device configuration and DNS responser server as resolver test does.
    // Currently, leave DNS related configuration in this test because only it needs DNS
    // client-server testing environment.
    DnsResponderClient dnsClient;
    dnsClient.SetUp();

    // Setup DNS responder server.
    constexpr char listen_addr[] = "127.0.0.3";
    constexpr char listen_srv[] = "53";
    test::DNSResponder dns(listen_addr, listen_srv, ns_rcode::ns_r_servfail);
    dns.addMapping("hi.example.com.", ns_type::ns_t_a, "1.2.3.4");
    ASSERT_TRUE(dns.startServer());

    // Setup DNS configuration.
    const std::vector<std::string> test_servers = {listen_addr};
    std::vector<std::string> test_domains = {"example.com"};
    std::vector<int> test_params = {300 /*sample_validity*/, 25 /*success_threshold*/,
                                    8 /*min_samples*/, 8 /*max_samples*/};

    ASSERT_TRUE(dnsClient.SetResolversForNetwork(test_servers, test_domains, test_params));
    dns.clearQueries();

    // Register event listener.
    android::sp<TestOnDnsEvent> testOnDnsEvent = new TestOnDnsEvent(expectedResults);
    android::binder::Status status = mDnsResolver->registerEventListener(
            android::interface_cast<INetdEventListener>(testOnDnsEvent));
    ASSERT_TRUE(status.isOk()) << status.exceptionMessage();

    // DNS queries.
    // Once all expected events of expectedResults are received by the listener, the unit test will
    // be notified. Otherwise, notified with a timeout expired failure.
    auto& cv = testOnDnsEvent->getCv();
    auto& cvMutex = testOnDnsEvent->getCvMutex();
    {
        std::unique_lock lock(cvMutex);

        for (const auto& config : testConfigs) {
            SCOPED_TRACE(config.hostname);

            addrinfo* result = nullptr;
            addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
            int status = getaddrinfo(config.hostname.c_str(), nullptr, &hints, &result);
            EXPECT_EQ(config.returnCode, status);

            if (result) freeaddrinfo(result);
        }

        // Wait for receiving expected events.
        EXPECT_EQ(std::cv_status::no_timeout, cv.wait_for(lock, std::chrono::seconds(2)));
    }

    // Verify that all testcases are passed.
    EXPECT_TRUE(testOnDnsEvent->isVerified());

    dnsClient.TearDown();
}

// TODO: Need to test more than one server cases.
TEST_F(DnsResolverBinderTest, SetResolverConfiguration_Tls) {
    const std::vector<std::string> LOCALLY_ASSIGNED_DNS{"8.8.8.8", "2001:4860:4860::8888"};
    static const std::vector<std::string> valid_v4_addr = {"192.0.2.1"};
    static const std::vector<std::string> valid_v6_addr = {"2001:db8::2"};
    static const std::vector<std::string> invalid_v4_addr = {"192.0.*.5"};
    static const std::vector<std::string> invalid_v6_addr = {"2001:dg8::6"};
    constexpr char valid_tls_name[] = "example.com";
    std::vector<int> test_params = {300, 25, 8, 8};
    // We enumerate valid and invalid v4/v6 address, and several different TLS names
    // to be the input data and verify the binder status.
    static const struct TestData {
        const std::vector<std::string> servers;
        const std::string tlsName;
        const int expectedReturnCode;
    } kTlsTestData[] = {
            {valid_v4_addr, valid_tls_name, 0},
            {valid_v4_addr, "host.com", 0},
            {valid_v4_addr, "@@@@", 0},
            {valid_v4_addr, "", 0},
            {valid_v6_addr, valid_tls_name, 0},
            {valid_v6_addr, "host.com", 0},
            {valid_v6_addr, "@@@@", 0},
            {valid_v6_addr, "", 0},
            {invalid_v4_addr, valid_tls_name, EINVAL},
            {invalid_v4_addr, "host.com", EINVAL},
            {invalid_v4_addr, "@@@@", EINVAL},
            {invalid_v4_addr, "", EINVAL},
            {invalid_v6_addr, valid_tls_name, EINVAL},
            {invalid_v6_addr, "host.com", EINVAL},
            {invalid_v6_addr, "@@@@", EINVAL},
            {invalid_v6_addr, "", EINVAL},
            {{}, "", 0},
            {{""}, "", EINVAL},
    };

    for (size_t i = 0; i < std::size(kTlsTestData); i++) {
        const auto& td = kTlsTestData[i];

        const auto resolverParams = makeResolverParamsParcel(
                TEST_NETID, test_params, LOCALLY_ASSIGNED_DNS, {}, td.tlsName, td.servers);
        binder::Status status = mDnsResolver->setResolverConfiguration(resolverParams);

        if (td.expectedReturnCode == 0) {
            SCOPED_TRACE(String8::format("test case %zu should have passed", i));
            SCOPED_TRACE(status.toString8());
            EXPECT_EQ(0, status.exceptionCode());
        } else {
            SCOPED_TRACE(String8::format("test case %zu should have failed", i));
            EXPECT_EQ(binder::Status::EX_SERVICE_SPECIFIC, status.exceptionCode());
            EXPECT_EQ(td.expectedReturnCode, status.serviceSpecificErrorCode());
        }
    }
}

TEST_F(DnsResolverBinderTest, GetResolverInfo) {
    std::vector<std::string> servers = {"127.0.0.1", "127.0.0.2"};
    std::vector<std::string> domains = {"example.com"};
    std::vector<int> testParams = {
            300,     // sample validity in seconds
            25,      // success threshod in percent
            8,   8,  // {MIN,MAX}_SAMPLES
            100,     // BASE_TIMEOUT_MSEC
            3,       // retry count
    };
    const auto resolverParams =
            makeResolverParamsParcel(TEST_NETID, testParams, servers, domains, "", {});
    binder::Status status = mDnsResolver->setResolverConfiguration(resolverParams);
    EXPECT_TRUE(status.isOk()) << status.exceptionMessage();

    std::vector<std::string> res_servers;
    std::vector<std::string> res_domains;
    std::vector<std::string> res_tls_servers;
    std::vector<int32_t> params32;
    std::vector<int32_t> stats32;
    std::vector<int32_t> wait_for_pending_req_timeout_count32{0};
    status = mDnsResolver->getResolverInfo(TEST_NETID, &res_servers, &res_domains, &res_tls_servers,
                                           &params32, &stats32,
                                           &wait_for_pending_req_timeout_count32);

    EXPECT_TRUE(status.isOk()) << status.exceptionMessage();
    EXPECT_EQ(servers.size(), res_servers.size());
    EXPECT_EQ(domains.size(), res_domains.size());
    EXPECT_EQ(0U, res_tls_servers.size());
    ASSERT_EQ(static_cast<size_t>(IDnsResolver::RESOLVER_PARAMS_COUNT), testParams.size());
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_SAMPLE_VALIDITY],
              params32[IDnsResolver::RESOLVER_PARAMS_SAMPLE_VALIDITY]);
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_SUCCESS_THRESHOLD],
              params32[IDnsResolver::RESOLVER_PARAMS_SUCCESS_THRESHOLD]);
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_MIN_SAMPLES],
              params32[IDnsResolver::RESOLVER_PARAMS_MIN_SAMPLES]);
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_MAX_SAMPLES],
              params32[IDnsResolver::RESOLVER_PARAMS_MAX_SAMPLES]);
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC],
              params32[IDnsResolver::RESOLVER_PARAMS_BASE_TIMEOUT_MSEC]);
    EXPECT_EQ(testParams[IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT],
              params32[IDnsResolver::RESOLVER_PARAMS_RETRY_COUNT]);

    std::vector<ResolverStats> stats;
    ResolverStats::decodeAll(stats32, &stats);

    EXPECT_EQ(servers.size(), stats.size());

    EXPECT_THAT(res_servers, testing::UnorderedElementsAreArray(servers));
    EXPECT_THAT(res_domains, testing::UnorderedElementsAreArray(domains));
}

TEST_F(DnsResolverBinderTest, CreateDestroyNetworkCache) {
    // Must not be the same as TEST_NETID
    const int ANOTHER_TEST_NETID = TEST_NETID + 1;

    // Create a new network cache.
    EXPECT_TRUE(mDnsResolver->createNetworkCache(ANOTHER_TEST_NETID).isOk());

    // create it again, expect a EEXIST.
    EXPECT_EQ(EEXIST,
              mDnsResolver->createNetworkCache(ANOTHER_TEST_NETID).serviceSpecificErrorCode());

    // destroy it.
    EXPECT_TRUE(mDnsResolver->destroyNetworkCache(ANOTHER_TEST_NETID).isOk());

    // re-create it
    EXPECT_TRUE(mDnsResolver->createNetworkCache(ANOTHER_TEST_NETID).isOk());

    // destroy it.
    EXPECT_TRUE(mDnsResolver->destroyNetworkCache(ANOTHER_TEST_NETID).isOk());

    // re-destroy it
    EXPECT_TRUE(mDnsResolver->destroyNetworkCache(ANOTHER_TEST_NETID).isOk());
}

TEST_F(DnsResolverBinderTest, setLogSeverity) {
    // Expect fail
    EXPECT_EQ(EINVAL, mDnsResolver->setLogSeverity(-1).serviceSpecificErrorCode());

    // Test set different log level
    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_VERBOSE).isOk());

    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_DEBUG).isOk());

    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_INFO).isOk());

    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_WARNING).isOk());

    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_ERROR).isOk());

    // Set back to default
    EXPECT_TRUE(mDnsResolver->setLogSeverity(IDnsResolver::DNS_RESOLVER_LOG_WARNING).isOk());
}
