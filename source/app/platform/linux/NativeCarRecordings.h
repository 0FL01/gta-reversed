#pragma once

#include "app/platform/linux/NativeScriptSession.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct NativeCarRecordingEntry {
    std::int32_t Id = 0;
    std::uint32_t Offset = 0, Size = 0;
};

class NativeCarRecordings {
public:
    static constexpr bool RuntimePlayback = false;
    bool LoadBeforeWorker(const char* gameDir, std::string& error);
    NativeScriptServiceResult Request(std::int32_t id);
    bool IsLoaded(std::int32_t id) const { return m_Loaded.contains(id); }
    std::size_t Count() const { return m_Entries.size(); }
    std::size_t LoadedCount() const { return m_Loaded.size(); }
private:
    std::map<std::int32_t, NativeCarRecordingEntry> m_Entries;
    std::set<std::int32_t> m_Loaded;
    std::shared_ptr<const std::vector<std::uint8_t>> m_Archive;
};
