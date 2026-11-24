#pragma once

// A message from the Settings page, read.

#include "apps/tekito-settings/SettingsShared.h"

namespace tekito::settings {

// A request from the Settings page: one flat JSON object whose values are
// strings, numbers or booleans (see ui/src/host.js). Anything nested is
// skipped; a malformed message reads as empty.
class WebMessage final {
public:
    static WebMessage Parse(std::wstring_view json) {
        WebMessage message;
        Reader reader{json};
        if (!reader.Consume(L'{')) return {};
        if (reader.Consume(L'}')) return message;
        do {
            std::wstring key;
            if (!reader.ReadString(key) || !reader.Consume(L':')) return {};
            Value value;
            if (!reader.ReadValue(value)) return {};
            message.values_[std::move(key)] = std::move(value);
        } while (reader.Consume(L','));
        return reader.Consume(L'}') ? message : WebMessage{};
    }

    std::wstring String(std::wstring_view key) const {
        const auto* value = Find(key);
        return value && value->kind == Value::Kind::String ? value->text : std::wstring{};
    }

    bool Bool(std::wstring_view key, bool fallback = false) const {
        const auto* value = Find(key);
        if (!value) return fallback;
        if (value->kind == Value::Kind::Bool) return value->boolean;
        if (value->kind == Value::Kind::Number) return value->number != 0.0;
        return fallback;
    }

    // Numbers, with booleans read as 0 and 1 ("value" carries either).
    std::int64_t Number(std::wstring_view key, std::int64_t fallback = 0) const {
        const auto* value = Find(key);
        if (!value) return fallback;
        if (value->kind == Value::Kind::Number) return static_cast<std::int64_t>(value->number);
        if (value->kind == Value::Kind::Bool) return value->boolean ? 1 : 0;
        return fallback;
    }

private:
    struct Value {
        enum class Kind { Null, String, Number, Bool } kind{Kind::Null};
        std::wstring text;
        double number{0.0};
        bool boolean{false};
    };

    struct Reader {
        std::wstring_view json;
        std::size_t index{0};

        void SkipSpace() {
            while (index < json.size() && std::iswspace(json[index])) ++index;
        }

        bool Consume(wchar_t expected) {
            SkipSpace();
            if (index >= json.size() || json[index] != expected) return false;
            ++index;
            return true;
        }

        bool ConsumeWord(std::wstring_view word) {
            if (json.substr(index, word.size()) != word) return false;
            index += word.size();
            return true;
        }

        bool ReadString(std::wstring& out) {
            if (!Consume(L'"')) return false;
            while (index < json.size()) {
                const wchar_t ch = json[index++];
                if (ch == L'"') return true;
                if (ch != L'\\') {
                    out += ch;
                    continue;
                }
                if (index >= json.size()) return false;
                switch (const wchar_t escaped = json[index++]) {
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u': {
                    if (index + 4 > json.size()) return false;
                    unsigned code = 0;
                    for (int digit = 0; digit < 4; ++digit) {
                        const wchar_t hex = json[index++];
                        code <<= 4;
                        if (hex >= L'0' && hex <= L'9') code |= hex - L'0';
                        else if (hex >= L'a' && hex <= L'f') code |= hex - L'a' + 10;
                        else if (hex >= L'A' && hex <= L'F') code |= hex - L'A' + 10;
                        else return false;
                    }
                    out += static_cast<wchar_t>(code);
                    break;
                }
                default: out += escaped; break;  // \" \\ \/
                }
            }
            return false;
        }

        bool ReadValue(Value& value) {
            SkipSpace();
            if (index >= json.size()) return false;
            const wchar_t ch = json[index];
            if (ch == L'"') {
                value.kind = Value::Kind::String;
                return ReadString(value.text);
            }
            if (ConsumeWord(L"true")) {
                value.kind = Value::Kind::Bool;
                value.boolean = true;
                return true;
            }
            if (ConsumeWord(L"false")) {
                value.kind = Value::Kind::Bool;
                return true;
            }
            if (ConsumeWord(L"null")) return true;
            if (ch == L'{' || ch == L'[') return SkipNested();
            const auto begin = index;
            while (index < json.size() && (std::iswdigit(json[index]) || json[index] == L'-' ||
                                           json[index] == L'+' || json[index] == L'.' ||
                                           json[index] == L'e' || json[index] == L'E')) {
                ++index;
            }
            if (index == begin) return false;
            value.kind = Value::Kind::Number;
            value.number = std::wcstod(std::wstring(json.substr(begin, index - begin)).c_str(),
                                       nullptr);
            return true;
        }

        bool SkipNested() {
            int depth = 0;
            do {
                if (index >= json.size()) return false;
                const wchar_t ch = json[index];
                if (ch == L'"') {
                    std::wstring ignored;
                    if (!ReadString(ignored)) return false;
                    continue;
                }
                if (ch == L'{' || ch == L'[') ++depth;
                if (ch == L'}' || ch == L']') --depth;
                ++index;
            } while (depth > 0);
            return true;
        }
    };

    const Value* Find(std::wstring_view key) const {
        const auto found = values_.find(key);
        return found == values_.end() ? nullptr : &found->second;
    }

    std::map<std::wstring, Value, std::less<>> values_;
};

}  // namespace tekito::settings
