/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <fuzzbinder/libbinder_ndk_driver.h>
#include <fuzzer/FuzzedDataProvider.h>

#include <android/binder_interface_utils.h>

#include "DnsResolverService.h"

using android::fuzzService;
using android::net::DnsResolverService;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    auto resolverService = ::ndk::SharedRefBase::make<DnsResolverService>();
    fuzzService(resolverService->asBinder().get(), FuzzedDataProvider(data, size));

    return 0;
}
