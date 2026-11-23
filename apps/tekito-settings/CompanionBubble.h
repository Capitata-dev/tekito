#pragma once

#include <windows.h>

#include <memory>
#include <string>

namespace tekito::companion {

// A short note beside the desktop companion (Caps Lock turned on, what a
// word reads and means): a rounded card in Windows' light or dark colors,
// fading in, gone after a while or when clicked.
class Bubble final {
public:
    Bubble();
    ~Bubble();
    Bubble(const Bubble&) = delete;
    Bubble& operator=(const Bubble&) = delete;

    bool Create(HINSTANCE instance);
    // `title`, and `body` under it when there is one, beside `anchor` (the
    // companion's window, in screen pixels) for `seconds`.
    void Show(const std::wstring& title, const std::wstring& body, const RECT& anchor, UINT dpi, double seconds);
    void Hide();
    [[nodiscard]] bool Visible() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace tekito::companion
