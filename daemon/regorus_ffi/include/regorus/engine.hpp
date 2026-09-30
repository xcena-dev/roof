// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// regorus/engine.hpp -- a rego engine, as C++ holds it.
//
// The C linkage the Rust side exports is an implementation detail of this library: it lives beside
// lib.rs, which only src/engine.cpp includes. A caller names this class and nothing else.

#pragma once

#include <string>

namespace fsdaemon::regorus
{

namespace abi
{
// The handle the Rust side allocated. Held by pointer, so this header needs no more of the
// generated prototypes than a forward declaration.
struct EngineHandle_t;
}  // namespace abi

// One rego engine holding one policy module.
//
// The engine a constructor builds is never mutated afterwards, and each evaluation runs on a copy,
// so one engine serves every thread that copies it under the caller's own lock.
class Engine
{
public:
    // Builds an engine holding @rego under @moduleName. Throws std::runtime_error when the module
    // does not compile or the engine cannot be created.
    Engine(const std::string& moduleName, const std::string& rego);
    ~Engine();

    // A copy is an independent engine carrying the same policy, which is what an evaluation runs
    // on. Throws std::runtime_error when the copy cannot be made.
    Engine(const Engine& other);
    Engine& operator=(const Engine& other) = delete;

    Engine(Engine&& other) noexcept;
    Engine& operator=(Engine&& other) noexcept;

    // The first expression's value of @query over @inputJson, as a JSON string, or empty when the
    // evaluation failed. Evaluating sets the input on this engine, so it is not const.
    [[nodiscard]] std::string evaluate(const std::string& inputJson, const std::string& query);

private:
    abi::EngineHandle_t* handle_{nullptr};
};

}  // namespace fsdaemon::regorus
