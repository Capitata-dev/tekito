#include "Core/UserDictionary.h"

#include <algorithm>

namespace tekito {

bool UserDictionary::Add(UserDictionaryEntry entry) {
    if (entry.id == 0 || entry.raw.empty() || entry.candidate.empty()) return false;
    if (!ids_.insert(entry.id).second) return false;
    try {
        entries_.push_back(std::move(entry));
    } catch (...) {
        ids_.erase(entry.id);
        throw;
    }
    return true;
}

bool UserDictionary::Remove(UserDictionaryEntryId id) noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const auto& entry) { return entry.id == id; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    ids_.erase(id);
    return true;
}

const UserDictionaryEntry* UserDictionary::Find(UserDictionaryEntryId id) const noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const auto& entry) { return entry.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

std::span<const UserDictionaryEntry> UserDictionary::Entries() const noexcept {
    return entries_;
}

}  // namespace tekito
