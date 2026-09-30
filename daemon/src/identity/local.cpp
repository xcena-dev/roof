// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// identity/local.cpp -- see internal/local.hpp.

#include "identity/internal/local.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "identity/base.hpp"
#include "identity/internal/selector.hpp"
#include "identity/internal/spiffe.hpp"
#include "model.hpp"
#include "util/file_text.hpp"
#include "util/text.hpp"

namespace fsdaemon::identity
{

namespace
{

// One rule the file names: the clauses a task must satisfy, and the identity it then carries.
struct Rule_t
{
    // Every match rule must hold for the rule to match (AND semantics).
    std::vector<Selector::MatchRule_t> match;
    Identity_t identity;
};

// One rule as its raw strings, before the kinds and values are resolved.
struct RawRule_t
{
    // Keeps the file's order, because the labels a matched rule records follow it.
    std::vector<std::pair<std::string, std::string>> match;
    Identity_t identity;
};

std::string_view stripQuotes(std::string_view text)
{
    if (text.size() > 1 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
    {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return text;
}

// A rule follows its section headers in order, so a scalar belongs to whichever of match or identity
// was named last. That is the whole of the layout the rules file uses.
enum class Section
{
    None,
    Match,
    Identity,
};

// Feeds one "key: value" or "key:" token into the rule being built and answers the section the next
// token belongs to. A word this layout does not name throws BridgeError, so a misspelling is refused.
Section takeKey(RawRule_t& rule, Section section, std::string_view token)
{
    const std::uint64_t colon = token.find(':');
    if (colon == std::string_view::npos)
    {
        throw BridgeError{std::string{"rules: no 'key: value' in '"}.append(token).append("'")};
    }
    const auto key = util::trimSpace(token.substr(0, colon));
    const auto value = util::trimSpace(token.substr(colon + 1));
    if (value.empty())
    {
        if (key == "match")
        {
            return Section::Match;
        }
        if (key == "identity")
        {
            return Section::Identity;
        }
        throw BridgeError{std::string{"rules: a rule has no section '"}.append(key).append("'")};
    }
    const std::string keyText{key};
    const std::string valueText{stripQuotes(value)};
    if (section == Section::Match)
    {
        rule.match.emplace_back(keyText, valueText);
        return section;
    }
    if (section == Section::Identity)
    {
        if (keyText == "spiffe_id")
        {
            rule.identity.spiffeId = valueText;
        }
        else if (keyText == "group")
        {
            rule.identity.group = valueText;
        }
        else if (keyText == "role")
        {
            rule.identity.role = valueText;
        }
        else
        {
            throw BridgeError{std::string{"rules: identity has no field '"}.append(keyText).append("'")};
        }
        return section;
    }
    throw BridgeError{std::string{"rules: '"}.append(keyText).append("' is under no section")};
}

std::vector<RawRule_t> parseRawRules(std::string_view text)
{
    std::vector<RawRule_t> rules;
    auto section = Section::None;
    auto inRules = false;
    std::uint64_t rulesIndent = 0;

    for (auto row : util::splitFields(text, "\n"))
    {
        // Where this line's own content starts, read before the comment strip, which never touches
        // leading space.
        const std::uint64_t indent = row.find_first_not_of(" \t");

        // Drop an inline comment and surrounding space. A '#' only starts a comment after space or at
        // the line start, so a '#' inside a value is left alone.
        if (const std::uint64_t hash = row.find(" #"); hash != std::string_view::npos)
        {
            row = row.substr(0, hash);
        }
        const auto trimmed = util::trimSpace(row);
        if (trimmed.empty() || trimmed.front() == '#')
        {
            continue;
        }

        if (!inRules)
        {
            if (util::trimSpace(trimmed.substr(0, trimmed.find(':'))) == "rules")
            {
                inRules = true;
                rulesIndent = indent;
            }
            continue;
        }

        if (trimmed.front() == '-')
        {
            rules.emplace_back();
            section = Section::None;
            const auto rest = util::trimSpace(trimmed.substr(1));
            if (!rest.empty())
            {
                section = takeKey(rules.back(), section, rest);
            }
            continue;
        }
        // A key back at the block's own column is the next top-level key, not a rule field. A
        // sequence item may sit at that column too, which is why the dash is read first.
        if (indent <= rulesIndent)
        {
            inRules = false;
            continue;
        }
        if (!rules.empty())
        {
            section = takeKey(rules.back(), section, trimmed);
        }
    }
    return rules;
}

// Appends the rule @keyText and @valueText name to @rules, reading the value text as the kind that
// @keyText spells expects it. False when @selectors does not support that kind, so a caller drops
// whatever named it. Throws BridgeError on a name no kind spells, or on an integer that will not
// parse.
bool appendMatchRule(std::vector<Selector::MatchRule_t>& rules, const Selector& selectors,
                     std::string_view keyText, std::string_view valueText)
{
    const auto kind = selectors.parseType(keyText);
    if (!kind)
    {
        if (!Selector::isKnownName(keyText))
        {
            throw BridgeError{std::string{"rules: no selector kind spells '"}.append(keyText).append("'")};
        }
        return false;
    }
    if (Selector::isIntType(*kind))
    {
        std::uint32_t number = 0;
        if (std::from_chars(valueText.data(), valueText.data() + valueText.size(), number).ec != std::errc{})
        {
            throw BridgeError{
                std::string{"rules: bad integer for '"}.append(keyText).append("': ").append(valueText)};
        }
        // The variant is filled where it lands. A moved MatchRule_t temporary here trips a g++ 13
        // -Wmaybe-uninitialized false positive on the string arm.
        auto& added = rules.emplace_back();
        added.kind = *kind;
        added.expected = number;
        return true;
    }
    auto& added = rules.emplace_back();
    added.kind = *kind;
    added.expected = std::string{valueText};
    return true;
}

// Every label @rules stamp when @creds satisfies all of them, or nullopt when one does not - the
// AND a rule's match block means. A sha256 rule may throw BridgeError, which the caller lets
// propagate as a denial.
std::optional<std::vector<std::string>> matchLabels(const std::vector<Selector::MatchRule_t>& rules,
                                                    const Creds_t& creds)
{
    std::vector<std::string> labels;
    labels.reserve(rules.size());
    for (const auto& rule : rules)
    {
        if (!Selector::matches(rule, creds))
        {
            return std::nullopt;
        }
        labels.push_back(Selector::makeLabel(rule));
    }
    return labels;
}

// The match rules @block names, or nullopt when @selectors does not support one of the kinds it
// names, so the caller drops the rule that block came from. Throws BridgeError on a value a kind
// refuses.
std::optional<std::vector<Selector::MatchRule_t>>
parseMatchBlock(const std::vector<std::pair<std::string, std::string>>& block, const Selector& selectors)
{
    std::vector<Selector::MatchRule_t> rules;
    rules.reserve(block.size());
    for (const auto& [keyText, valueText] : block)
    {
        if (!appendMatchRule(rules, selectors, keyText, valueText))
        {
            return std::nullopt;
        }
    }
    return rules;
}

// The rules in @text, keeping only those whose selector kinds @selectors supports. Throws
// BridgeError on a bad file or an unknown kind, so a caller retains the previous rules. A rule
// naming a kind outside @selectors, or with no match clause, is skipped rather than failing the
// whole file.
std::vector<Rule_t> parseRulesText(std::string_view text, const Selector& selectors)
{
    const auto raw = parseRawRules(text);
    std::vector<Rule_t> rules;

    for (const auto& rawRule : raw)
    {
        if (rawRule.match.empty())
        {
            continue;
        }

        auto matched = parseMatchBlock(rawRule.match, selectors);
        if (!matched)
        {
            continue;
        }

        auto identity = rawRule.identity;
        if (!identity.spiffeId.empty())
        {
            const auto [parsedGroup, parsedRole] = parseSpiffePath(identity.spiffeId);
            if (!identity.group.empty() && !parsedGroup.empty() && parsedGroup != identity.group)
            {
                continue;
            }
            if (!identity.role.empty() && !parsedRole.empty() && parsedRole != identity.role)
            {
                continue;
            }
            if (identity.group.empty())
            {
                identity.group = parsedGroup;
            }
            if (identity.role.empty())
            {
                identity.role = parsedRole;
            }
        }
        rules.push_back(Rule_t{std::move(*matched), std::move(identity)});
    }
    return rules;
}

// What one stat of the rules file settles, which is the whole of what loadRules branches on.
enum class Standing
{
    Absent,     // nothing there to load, so the backend resolves nothing
    Untrusted,  // a writer this daemon may not take rules from
    Trusted,
};

// Whether @path came from a writer this daemon may trust: owned by root or by the daemon's own
// euid, and not world-writable. A file anyone can rewrite chooses the admission rules.
Standing readStanding(const std::string& path)
{
    struct ::stat info
    {
    };
    if (::stat(path.c_str(), &info) != 0)
    {
        return Standing::Absent;
    }
    if (info.st_uid != 0 && info.st_uid != ::geteuid())
    {
        return Standing::Untrusted;
    }
    // Group-write is allowed for the umask-002 installs. World-write is not.
    return (info.st_mode & S_IWOTH) == 0 ? Standing::Trusted : Standing::Untrusted;
}

}  // namespace

// A shared hold lets worker threads attest in parallel while a SIGHUP reload takes the exclusive
// one to swap the list.
struct LocalIdentityProvider::Impl
{
    std::string path;

    mutable std::shared_mutex guard;
    std::vector<Rule_t> loaded;
};

LocalIdentityProvider::LocalIdentityProvider(std::string rulesPath,
                                             const std::vector<std::string>& selectorNames)
    : IdentityProvider{selectorNames},
      impl_{std::make_unique<Impl>()}
{
    impl_->path = std::move(rulesPath);
    loadRules();
}

LocalIdentityProvider::~LocalIdentityProvider() = default;

void LocalIdentityProvider::reload()
{
    loadRules();
}

void LocalIdentityProvider::loadRules()
{
    const auto standing = impl_->path.empty() ? Standing::Absent : readStanding(impl_->path);
    if (standing == Standing::Absent)
    {
        const std::unique_lock guard{impl_->guard};
        impl_->loaded.clear();
        return;
    }
    if (standing == Standing::Untrusted)
    {
        std::fprintf(stderr, "[identity-local] %s has unsafe permissions; refusing\n", impl_->path.c_str());
        return;
    }

    // A file the stat found but the open cannot reach reads as empty, which is a rules file that
    // resolves nothing rather than a reason to keep the last one.
    const auto text = util::readFileText(impl_->path).value_or(std::string{});
    std::vector<Rule_t> staged;
    try
    {
        staged = parseRulesText(text, getSelectors());
    }
    catch (const BridgeError& bad)
    {
        std::fprintf(stderr, "[identity-local] reload aborted (%s); previous rules retained\n", bad.what());
        return;
    }

    // The byte count is what an edit changes, so the line says whether this load read the edit.
    const std::uint64_t ruleCount = staged.size();
    {
        const std::unique_lock guard{impl_->guard};
        impl_->loaded = std::move(staged);
    }
    std::fprintf(stderr, "[identity-local] loaded %" PRIu64 " rules (%" PRIu64 " bytes) from %s\n", ruleCount,
                 static_cast<std::uint64_t>(text.size()), impl_->path.c_str());
}

Identity_t LocalIdentityProvider::attest(pid_t pid, const Creds_t& facts)
{
    const auto creds = resolveCreds(pid, facts);
    if (!creds)
    {
        throw BridgeError{"no rule for pid=" + std::to_string(pid) + " (proc read failed)"};
    }

    const std::shared_lock guard{impl_->guard};
    for (const auto& rule : impl_->loaded)
    {
        if (const auto labels = matchLabels(rule.match, *creds); labels)
        {
            auto identity = rule.identity;
            identity.selectors = *labels;
            return identity;
        }
    }
    throw BridgeError{"no rule for pid=" + std::to_string(pid)};
}

bool LocalIdentityProvider::needsWorker() const noexcept
{
    return getSelectors().isSupported(Selector::Sha256);
}

std::vector<Identity_t> LocalIdentityProvider::getKnownIdentities() const
{
    const std::shared_lock guard{impl_->guard};
    std::vector<Identity_t> out;
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& rule : impl_->loaded)
    {
        const std::pair key{rule.identity.group, rule.identity.role};
        if (seen.insert(key).second)
        {
            out.push_back(rule.identity);
        }
    }
    return out;
}

}  // namespace fsdaemon::identity
