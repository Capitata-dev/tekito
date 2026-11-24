#pragma once

#include "Core/BuiltinLexicon.h"

#include <functional>
#include <cstddef>
#include <span>
#include <vector>

namespace tekito {

struct LexiconQuery {
    enum class Kind {
        Exact,
        Prefix,
        Candidate,
    };

    Kind kind{Kind::Exact};
    std::wstring_view text;
    std::size_t maxResults{0};
    // A Prefix query that only looks for words near a slip (not for
    // completions): a provider whose fuzzy index already answered the
    // Candidate query may skip it.
    bool nearSlip{false};
    // How many edits a Candidate may be from the text (at most 2).
    std::size_t maxDistance{2};
};

class ILexiconProvider {
public:
    using Visitor = std::function<void(const LexiconEntry&)>;

    virtual ~ILexiconProvider() = default;
    [[nodiscard]] virtual std::span<const LexiconEntry> Entries() const noexcept = 0;
    virtual void Visit(const Visitor& visitor) const {
        for (const auto& entry : Entries()) visitor(entry);
    }
    virtual void Find(const LexiconQuery& query, const Visitor& visitor) const;
};

class BuiltinLexiconProvider final : public ILexiconProvider {
public:
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
};

class CompositeLexiconProvider final : public ILexiconProvider {
public:
    CompositeLexiconProvider(const ILexiconProvider& primary,
                             const ILexiconProvider& fallback);

    void Refresh();
    [[nodiscard]] std::span<const LexiconEntry> Entries() const noexcept override;
    void Visit(const Visitor& visitor) const override;
    void Find(const LexiconQuery& query, const Visitor& visitor) const override;

private:
    const ILexiconProvider& primary_;
    const ILexiconProvider& fallback_;
    std::vector<LexiconEntry> entries_;
};

}  // namespace tekito
