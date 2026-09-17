#include "app/platform/linux/NativeScriptClothes.h"

#include <algorithm>
#include <limits>

namespace {
bool NameValid(const std::array<char, 16>& name) {
    const auto end = std::ranges::find(name, '\0');
    if (end == name.begin()) return false;
    return std::ranges::all_of(name.begin(), end, [](char value) {
        const auto c = static_cast<unsigned char>(value);
        return c >= 0x20 && c <= 0x7E;
    });
}
}

NativeScriptServiceResult NativeScriptClothes::Give(const NativeScriptClothesRequest& request) {
    if (request.PlayerIndex != 0 || request.BodyPart < 0 || request.BodyPart >= std::ssize(m_State.Parts) ||
        !NameValid(request.Texture) || !NameValid(request.Model)) {
        return {NativeScriptServiceStatus::Error, "invalid source clothes request"};
    }
    if (m_State.Revision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "clothes revision exhausted"};
    }
    m_State.Parts[std::size_t(request.BodyPart)] = {request.Texture, request.Model};
    ++m_State.Revision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeScriptClothes::Build(std::int32_t playerIndex) {
    if (playerIndex != 0) {
        return {NativeScriptServiceStatus::Error, "clothes build requires player0"};
    }
    if (m_State.BuildRevision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "clothes build revision exhausted"};
    }
    ++m_State.BuildRevision;
    return {NativeScriptServiceStatus::Ready, {}};
}

NativeScriptServiceResult NativeScriptClothes::Store() {
    if (m_State.StoreRevision == std::numeric_limits<std::uint64_t>::max()) {
        return {NativeScriptServiceStatus::Error, "clothes store revision exhausted"};
    }
    m_State.StoredParts = m_State.Parts;
    m_State.HasStoredState = true;
    ++m_State.StoreRevision;
    return {NativeScriptServiceStatus::Ready, {}};
}
