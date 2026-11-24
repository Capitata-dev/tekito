#pragma once

#include <string>
#include <string_view>

namespace tekito::companion {

// What the desktop companion says about a word handed to it (dropped on it,
// or copied and looked up from its menu): how a Japanese word reads and
// what it means, what an English word means.
struct Lookup {
    std::wstring title;
    std::wstring body;
};

// `text`, trimmed and cut short, with its reading and meaning, in Japanese
// when `japaneseUi`; says so when neither is known. Reads the Japanese
// data the first time, so for a click, not for each key.
[[nodiscard]] Lookup LookUp(std::wstring_view text, bool japaneseUi);

}  // namespace tekito::companion
