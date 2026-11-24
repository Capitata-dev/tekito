#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tekito::japanese {

// Which kanji candidates may use (UserSettings::japaneseKanjiRange).
enum class KanjiRange {
    All = 0,
    // 常用漢字 and 人名用漢字.
    JoyoAndJinmei = 1,
    Joyo = 2,
    // 教育漢字.
    Kyoiku = 3,
};

// 教育漢字, 常用漢字 and 人名用漢字, from the kanji-sets pack.
class KanjiSets final {
public:
    // kanji.tsv: "kanji<TAB>level", 1-6 = 教育 of that school year,
    // 8 = 常用, 9 = 人名用.
    bool Load(const std::filesystem::path& file);
    void Add(char32_t kanji, int level) { levels_[kanji] = level; }
    [[nodiscard]] bool Empty() const noexcept { return levels_.empty(); }

    // Every kanji in `text` is in `range` (kana, letters and marks always
    // are); for 教育, taught by school year `grade` (1-6). `names`: 人名用
    // kanji are allowed too.
    [[nodiscard]] bool Allows(std::wstring_view text, KanjiRange range, int grade = 6, bool names = false) const;
    // Every kanji in `text` is 常用 or 人名用.
    [[nodiscard]] bool Common(std::wstring_view text) const { return Allows(text, KanjiRange::JoyoAndJinmei); }

    // Kanji variant forms (異体字), from the japanese-variants pack:
    // variants.tsv, "kanji<TAB>its variants" (高<TAB>髙).
    bool LoadVariants(const std::filesystem::path& file);
    void AddVariants(char32_t kanji, std::u32string variants) { variants_[kanji] = std::move(variants); }
    [[nodiscard]] bool HasVariants() const noexcept { return !variants_.empty(); }
    // Each kanji's old forms, 旧字体 (old-forms.tsv, "国<TAB>國"), from
    // the same pack.
    bool LoadOldForms(const std::filesystem::path& file);
    void AddOldForms(char32_t kanji, std::u32string oldForms);
    // `text` as written today: each old form (旧字体: 國) as its new form
    // (国), each other variant that is not 常用 (異体字: 髙, 﨑, 邉) as the
    // first 常用 kanji it is a variant of. Old when every kanji changed was
    // an old form, Other when one was another variant; none when no kanji
    // changed.
    enum class VariantKind : std::uint8_t { Old, Other };
    struct StandardText {
        std::wstring text;
        VariantKind kind{VariantKind::Old};
    };
    [[nodiscard]] std::optional<StandardText> StandardForm(std::wstring_view text) const;
    // `text` with one of its kanji written another way (高橋: 髙橋, 崎: 﨑),
    // kanji by kanji and each kanji's variants in order, at most `most`.
    [[nodiscard]] std::vector<std::wstring> VariantForms(std::wstring_view text, std::size_t most) const;

private:
    std::unordered_map<char32_t, int> levels_;
    std::unordered_map<char32_t, std::u32string> variants_;
    // Each old form's new form (國: 国).
    std::unordered_map<char32_t, char32_t> newForms_;
};

}  // namespace tekito::japanese
