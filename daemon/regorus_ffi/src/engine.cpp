// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// src/engine.cpp -- see include/regorus/engine.hpp.
//
// The one translation unit that names the C linkage. Everything above it sees a C++ class.

#include "regorus/engine.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "daemon_regorus_ffi.h"

namespace fsdaemon::regorus
{

Engine::Engine(const std::string& moduleName, const std::string& rego)
    : handle_{abi::daemon_regorus_new()}
{
    if (handle_ == nullptr)
    {
        throw std::runtime_error{"regorus: could not create an engine"};
    }
    if (abi::daemon_regorus_add_policy(handle_, moduleName.c_str(), rego.c_str()) != 0)
    {
        abi::daemon_regorus_free(handle_);
        throw std::runtime_error{"regorus: " + moduleName + " did not compile"};
    }
}

Engine::~Engine()
{
    abi::daemon_regorus_free(handle_);
}

Engine::Engine(const Engine& other)
    : handle_{abi::daemon_regorus_clone(other.handle_)}
{
    if (handle_ == nullptr)
    {
        throw std::runtime_error{"regorus: could not copy an engine"};
    }
}

Engine::Engine(Engine&& other) noexcept
    : handle_{std::exchange(other.handle_, nullptr)}
{
}

Engine& Engine::operator=(Engine&& other) noexcept
{
    if (this != &other)
    {
        abi::daemon_regorus_free(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

std::string Engine::evaluate(const std::string& inputJson, const std::string& query)
{
    char* raw = abi::daemon_regorus_eval(handle_, inputJson.c_str(), query.c_str());
    std::string result = (raw != nullptr) ? std::string{raw} : std::string{};
    abi::daemon_regorus_free_string(raw);
    return result;
}

}  // namespace fsdaemon::regorus
