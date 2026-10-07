// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/local.hpp -- the identity backend that needs no server: a rules file matched to
// identities, and the rule parser its rules file is read through.

#pragma once

#include <sys/types.h>

#include <memory>
#include <string>
#include <vector>

#include "identity/base.hpp"
#include "model.hpp"

namespace fsdaemon::identity
{

// The identity backend that needs no server: rules file to identity.
//
// It loads a file of rules and matches a task's creds against them in order, so resolving a pid
// waits on nothing. The rules are guarded by a shared mutex: worker threads attest under a shared
// hold while a SIGHUP reload takes the exclusive one to swap them.
class LocalIdentityProvider : public IdentityProvider
{
public:
    // @rulesPath may be empty or absent, which resolves nothing rather than faults. @selectorNames
    // is the kinds the rules may match on, and @trustDomain the one each rule's SPIFFE id names.
    // Loads the rules at construction, so an unknown name throws from here.
    LocalIdentityProvider(std::string rulesPath, const std::vector<std::string>& selectorNames,
                          std::string trustDomain = {});

    [[nodiscard]] Identity_t attest(pid_t pid, const Creds_t& facts) override;

    [[nodiscard]] std::vector<Identity_t> getKnownIdentities() const override;

    // A sha256 clause reads the whole executable on every attest, so a set that carries that kind
    // answers on a worker. Every other kind reads what the kernel already sent.
    [[nodiscard]] bool needsWorker() const noexcept override;

    // Re-reads the rules file. A parse error or an unsafe file keeps the rules already loaded, so a
    // bad edit does not empty the backend under a running daemon.
    void reload() override;

    // Throws on the parse error or the unsafe file reload() would keep the old rules over.
    void checkReload() const override;

    // Out of line, so the rule record stays inside the backend's own translation unit.
    ~LocalIdentityProvider() override;

private:
    // The reload body, called directly by the constructor and by the reload() override alike, so
    // construction never dispatches through a virtual function.
    void loadRules();

    // Everything this backend reads a task against: where the rules came from, the rules it loaded,
    // and the lock that guards them. Held behind a pointer so a caller that only constructs a
    // backend never compiles a rule or a selector kind.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fsdaemon::identity
