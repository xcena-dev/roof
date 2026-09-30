// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// policy/opa_transport.cpp -- see internal/opa_transport.hpp.

#include "policy/internal/opa_transport.hpp"

#include <curl/curl.h>
#include <curl/easy.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "policy/internal/opa.hpp"

namespace fsdaemon::policy
{

namespace
{

// What curl_easy_setopt and curl_easy_getinfo take through their varargs. The assert holds this
// target to the width those calls read, since a mismatch there is undefined and silent.
using CurlNumber = long;  // NOLINT(google-runtime-int) the width curl's varargs read
static_assert(sizeof(CurlNumber) == sizeof(std::int64_t), "curl reads 64 bits here");

// libcurl write callback: appends the received bytes to the std::string behind @userdata.
std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* userdata)
{
    const auto total = size * count;
    static_cast<std::string*>(userdata)->append(data, total);
    return total;
}

// Runs one POST and returns the body, or throws OpaTransportError. Held out of the lambda so the
// header list and the handle are freed on every path, including a throw.
std::string postOnce(const std::string& url, const std::string& requestBody,
                     const std::string& unixSocketPath, std::chrono::milliseconds timeout)
{
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{curl_easy_init(), curl_easy_cleanup};
    if (!handle)
    {
        throw OpaTransportError{"could not create a curl handle"};
    }

    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers{
        curl_slist_append(nullptr, "Content-Type: application/json"), curl_slist_free_all};

    std::string body;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    // The path decides where the bytes go, so the host in the url reaches no name resolver and no
    // port. A deployment that leaves this empty is back on whoever holds that port.
    if (!unixSocketPath.empty())
    {
        curl_easy_setopt(handle.get(), CURLOPT_UNIX_SOCKET_PATH, unixSocketPath.c_str());
    }
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<CurlNumber>(requestBody.size()));
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, static_cast<CurlNumber>(timeout.count()));
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, &appendBody);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &body);

    const auto code = curl_easy_perform(handle.get());
    if (code != CURLE_OK)
    {
        throw OpaTransportError{curl_easy_strerror(code)};
    }

    CurlNumber status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    constexpr CurlNumber FirstSuccess = 200;
    constexpr CurlNumber FirstRedirect = 300;
    if (status < FirstSuccess || status >= FirstRedirect)
    {
        throw OpaTransportError{"OPA answered HTTP " + std::to_string(status)};
    }
    return body;
}

}  // namespace

// Each call opens and closes its own handle, so several worker threads may use one of these at once.
OpaTransport makeCurlTransport(std::chrono::milliseconds timeout, std::string unixSocketPath)
{
    return [timeout, socketPath = std::move(unixSocketPath)](const std::string& url, const std::string& requestBody)
    {
        return postOnce(url, requestBody, socketPath, timeout);
    };
}

}  // namespace fsdaemon::policy
