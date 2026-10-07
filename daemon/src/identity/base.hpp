// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/base.hpp -- the identity model every backend shares: errors, process credentials,
// selectors, SPIFFE ids, and the IdentityProvider interface a caller resolves a pid through.

#pragma once

#include <sys/types.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "model.hpp"

namespace fsdaemon::identity
{

// A backend could not resolve the requested pid to an identity, or was asked to build an identity
// it cannot. The attest and access paths turn this into a denial rather than letting it escape.
class BridgeError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The set of selector kinds a backend resolves on. Only the backends name one, so the definition
// lives beside them and a serving layer that holds a provider never compiles it.
class Selector;

// Pid to identity, the surface the attest and access paths call.
//
// A backend resolves the calling task the kernel described to an identity the policy reads: a group
// and a role, and the SPIFFE id they came from. The local backend answers from a rules file; a
// directory backend would answer from a server. Both are this one interface, so the serving code
// names neither.
class IdentityProvider
{
public:
    IdentityProvider(const IdentityProvider&) = delete;
    IdentityProvider(IdentityProvider&&) = delete;
    IdentityProvider& operator=(const IdentityProvider&) = delete;
    IdentityProvider& operator=(IdentityProvider&&) = delete;
    // Out of line, so a caller that never names a Selector still destroys one correctly.
    virtual ~IdentityProvider();

    // The identity of @pid, or a throw of BridgeError when no rule resolves it or the task is not
    // the one the kernel read. @facts carries the kernel's word, so the common path opens no /proc.
    [[nodiscard]] virtual Identity_t attest(pid_t pid, const Creds_t& facts) = 0;

    // The identities this backend can resolve to, or empty when the set is not enumerable. A file of
    // rules names a finite set; a directory server does not.
    [[nodiscard]] virtual std::vector<Identity_t> getKnownIdentities() const
    {
        return {};
    }

    // Whether an answer from this backend belongs on a worker rather than on the serve loop, which
    // is true of a backend that asks something over a socket and false of one that reads a file it
    // already loaded. A worker costs a wake each way, so only a wait worth more than that earns one.
    [[nodiscard]] virtual bool needsWorker() const noexcept
    {
        return false;
    }

    // Re-read the backend's source. A failure keeps whatever was already loaded.
    virtual void reload() = 0;

    // Throws when the next reload() would fail, and changes nothing loaded. A backend with no source
    // of its own to re-read has nothing to check.
    virtual void checkReload() const
    {
    }

protected:
    // @selectorNames is the config list of kinds this backend resolves on, empty for the default
    // set. Every backend needs one, so the set is built here rather than in each of them.
    explicit IdentityProvider(const std::vector<std::string>& selectorNames);

    // The set is fixed at construction, so a backend reads it and never replaces it.
    [[nodiscard]] const Selector& getSelectors() const noexcept;

    // The credentials of @pid the chosen kinds need: @facts as the kernel sent them, plus the
    // group list when a kind asks for one, which only a /proc/<pid>/status read gives. nullopt
    // when the task is gone, when its files will not parse, or when the start time /proc reports
    // for that pid is not the one @facts carries.
    [[nodiscard]] std::optional<Creds_t> resolveCreds(pid_t pid, const Creds_t& facts) const;

private:
    std::unique_ptr<Selector> selectors_;
};

}  // namespace fsdaemon::identity
