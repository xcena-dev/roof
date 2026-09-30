// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/internal/selector.hpp -- the selector kinds a backend resolves on, and the kind
// vocabulary.
//
// internal/ means the identity backends and their probes only. The serving layers hold an
// IdentityProvider and never name a kind, so base.hpp forward declares the type instead.

#pragma once

#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "identity/base.hpp"
#include "model.hpp"

namespace fsdaemon::identity
{

// The selector kinds the operator chose, and everything the serving paths ask of that set.
//
// One kind is a Type. A Selector is the chosen set of them, built from the config list of names, so
// the set a backend holds is always one config already validated and deduplicated.
class Selector
{
public:
    // Unscoped inside the class: an enumerator indexes Rows and shifts into supported_, and
    // Selector::Uid still names its owner at every use.
    enum Type : std::uint8_t
    {
        Uid,
        Gid,
        SupplementaryGid,
        Path,
        Sha256,
    };

    // One clause of a rule: the kind it names, and the value it expects. The variant holds an id
    // for the integer kinds and a string for the rest, and exactly one of them, which is what
    // isIntType lets a caller decide before it reads.
    struct MatchRule_t
    {
        Type kind;
        std::variant<std::uint32_t, std::string> expected;
    };

    // ── Building one ────────────────────────────────────────────────────
    // @names is the config list of selector names. An empty list is the default set, the uid
    // alone, which derives no label a rule did not ask for and reads no /proc. Throws BridgeError
    // on an unknown name, so a misspelled selector does not silently widen the subscribe.
    // Duplicates are dropped.
    explicit Selector(const std::vector<std::string>& names);

    // ── Reading this set ────────────────────────────────────────────────
    // The kinds this set carries, in the order config named them.
    [[nodiscard]] const std::vector<Type>& getKinds() const noexcept
    {
        return kinds_;
    }

    // The Type @name spells when this set supports it, or nullopt when no kind spells @name or
    // this set does not carry the one that does. A caller that must tell those two apart asks
    // isKnownName, because only one of them is a misspelling.
    [[nodiscard]] std::optional<Type> parseType(std::string_view name) const;

    // ── Yes or no about a kind ──────────────────────────────────────────
    // The first asks this set and so is an instance method. The other two ask the enum itself and
    // so are static, which is why a caller can reach them without holding a set.

    // Whether this set carries @kind, which is what decides if work only that kind needs is worth
    // doing.
    [[nodiscard]] bool isSupported(Type kind) const noexcept;

    // Whether any kind spells @name, whatever this set carries. A rules file word no kind spells is
    // a misspelling, and a caller refuses the file rather than dropping the rule that named it.
    [[nodiscard]] static bool isKnownName(std::string_view name) noexcept;

    // Whether @kind carries an id the kernel sent, so its rule value is an integer. Every other kind
    // carries text: the exe path, or the digest of that exe.
    [[nodiscard]] static constexpr bool isIntType(Type kind) noexcept
    {
        return kind == Uid || kind == Gid || kind == SupplementaryGid;
    }

    // ── What a kind produces ────────────────────────────────────────────
    // These read no set, so a caller composes them its own way: the local backend into a rules
    // file's clauses, a directory backend into the labels its agent is asked for.

    // The unix:<kind>:<value> label @rule stamps.
    [[nodiscard]] static std::string makeLabel(const MatchRule_t& rule);

    // Whether @creds satisfies @rule. A sha256 rule opens and re-verifies the exe, so it may throw
    // BridgeError. The other kinds never do.
    [[nodiscard]] static bool matches(const MatchRule_t& rule, const Creds_t& creds);

    // The labels one @kind derives from @creds: one for a scalar kind, one per supplementary gid,
    // none for a path a request did not carry.
    [[nodiscard]] static std::vector<std::string> deriveKindLabels(Type kind, const Creds_t& creds);

private:
    // The word each kind reads and writes. A new kind is one row here, beside its enumerator above.
    static constexpr std::pair<Type, std::string_view> Rows[]{
        {Uid, "uid"},
        {Gid, "gid"},
        {SupplementaryGid, "supplementary_gid"},
        {Path, "path"},
        {Sha256, "sha256"},
    };

    // A row per enumerator, in enumerator order, so a kind indexes its own row.
    static_assert(std::size(Rows) == Sha256 + 1U, "every kind needs a row");
    static_assert(Rows[Uid].first == Uid && Rows[Gid].first == Gid &&
                      Rows[SupplementaryGid].first == SupplementaryGid && Rows[Path].first == Path &&
                      Rows[Sha256].first == Sha256,
                  "Rows must sit in enumerator order");

    // The kind @name spells, whether or not this set carries it, or nullopt when no kind spells it.
    [[nodiscard]] static std::optional<Type> findType(std::string_view name) noexcept;

    // Adds @kind unless the set already carries it. The list and the bitmask only ever move
    // together, so no caller can leave one behind.
    void addKind(Type kind);

    // The chosen kinds in the order config named them, which is the order a label list follows.
    std::vector<Type> kinds_;

    // The same set as one bit per Type, so asking whether a kind is carried costs no scan. The enum
    // is dense and small, so a bit each is exact and needs no allocation.
    std::uint8_t supported_{0};

    static_assert(Sha256 < std::numeric_limits<decltype(supported_)>::digits,
                  "a Type must fit in the supported_ bitmask");
};
}  // namespace fsdaemon::identity
