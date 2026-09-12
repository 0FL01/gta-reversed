#include "app/platform/linux/NativeScriptCorpus.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
template<typename T>
void Hash(std::uint64_t& hash, T value) noexcept {
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    for (unsigned i = 0; i < sizeof(U); ++i) {
        hash ^= std::uint8_t(bits >> (i * 8));
        hash *= 1099511628211ULL;
    }
}

NativeScriptCorpusThread ThreadOf(const NativeScriptInstructionForm& form) {
    return {form.ThreadIndex, form.BaseIP, form.LocalCount, form.ThreadGeneration,
        form.MissionIndex, form.ThreadForm, form.UsesMissionCleanup,
        form.ExclusiveMission, form.External};
}

bool SameSiteForm(const NativeScriptInstructionForm& a, const NativeScriptInstructionForm& b) {
    return a.Session == b.Session && a.ThreadGeneration == b.ThreadGeneration &&
        a.ThreadIndex == b.ThreadIndex && a.IP == b.IP && a.NextIP == b.NextIP &&
        a.BaseIP == b.BaseIP && a.LocalCount == b.LocalCount && a.MissionIndex == b.MissionIndex &&
        a.Opcode == b.Opcode && a.RawOpcode == b.RawOpcode && a.OperandTypes == b.OperandTypes &&
        a.OperandTags == b.OperandTags && a.ArrayCounts == b.ArrayCounts &&
        a.ArrayFlags == b.ArrayFlags && a.OperandCount == b.OperandCount &&
        a.Semantics == b.Semantics && a.ThreadForm == b.ThreadForm &&
        a.Negated == b.Negated && a.UsesMissionCleanup == b.UsesMissionCleanup &&
        a.ExclusiveMission == b.ExclusiveMission && a.External == b.External;
}
}

bool NativeScriptCorpusManifest::Observe(const NativeScriptInstructionForm& form, std::string& error) {
    try {
        auto candidate = *this;
        if (!candidate.ObserveInPlace(form, error)) return false;
        *this = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("script corpus allocation failed: ") + exception.what();
        return false;
    } catch (...) {
        error = "script corpus allocation failed";
        return false;
    }
}

bool NativeScriptCorpusManifest::ObserveInPlace(const NativeScriptInstructionForm& form, std::string& error) {
    const auto* schema = NativeScriptLookupSchema(form.Opcode);
    const auto tagValid = [](NativeScriptOperandType type, std::uint8_t tag) {
        switch (type) {
        case NativeScriptOperandType::Integer: return tag == 1 || tag == 2 || tag == 3 || tag == 4 || tag == 5 || tag == 7 || tag == 8;
        case NativeScriptOperandType::Float: return tag == 2 || tag == 3 || tag == 6 || tag == 7 || tag == 8;
        case NativeScriptOperandType::String: return tag == 9;
        case NativeScriptOperandType::Output:
        case NativeScriptOperandType::FloatOutput:
        case NativeScriptOperandType::InOutInteger:
        case NativeScriptOperandType::InOutFloat:
            return tag == 2 || tag == 3 || tag == 7 || tag == 8;
        }
        return false;
    };
    if (!form.Session || !form.Sequence || !form.ThreadGeneration || !form.LocalCount ||
        !schema || form.RawOpcode != std::uint16_t(form.Opcode | (form.Negated ? 0x8000 : 0)) ||
        form.OperandCount != schema->OperandCount || form.OperandTypes != schema->Operands ||
        form.Semantics != schema->Semantics || form.NextIP <= form.IP) {
        error = "invalid/unclassified instruction form";
        return false;
    }
    if (m_Session && m_Session != form.Session) {
        error = "mixed script sessions in corpus";
        return false;
    }
    if (form.ThreadForm == NativeScriptThreadForm::Main &&
        (form.BaseIP || form.MissionIndex != -1 || form.UsesMissionCleanup || form.ExclusiveMission || form.External)) {
        error = "invalid main-thread form";
        return false;
    }
    if (form.ThreadForm == NativeScriptThreadForm::Mission &&
        (form.BaseIP != 200000 || form.MissionIndex < 0 || !form.UsesMissionCleanup || !form.ExclusiveMission || form.External)) {
        error = "invalid mission-thread form";
        return false;
    }
    if (form.ThreadForm == NativeScriptThreadForm::Streamed && !form.External) {
        error = "invalid streamed-thread form";
        return false;
    }
    for (unsigned i = 0; i < form.OperandCount; ++i) {
        if (!tagValid(form.OperandTypes[i], form.OperandTags[i])) {
            error = "operand tag contradicts classified schema";
            return false;
        }
        const bool array = form.OperandTags[i] == 7 || form.OperandTags[i] == 8;
        if (array != (form.ArrayCounts[i] != 0) || (!array && form.ArrayFlags[i])) {
            error = "invalid array operand form";
            return false;
        }
        const bool floating = form.OperandTypes[i] == NativeScriptOperandType::Float ||
            form.OperandTypes[i] == NativeScriptOperandType::FloatOutput ||
            form.OperandTypes[i] == NativeScriptOperandType::InOutFloat;
        if (array && (form.ArrayFlags[i] & 0x7F) != (floating ? 1 : 0)) {
            error = "array element type contradicts schema";
            return false;
        }
    }
    for (unsigned i = form.OperandCount; i < form.OperandTags.size(); ++i) {
        if (form.OperandTags[i] || form.ArrayCounts[i] || form.ArrayFlags[i]) {
            error = "nonzero operand form beyond schema arity";
            return false;
        }
    }
    const auto thread = ThreadOf(form);
    const auto threadAt = std::ranges::find_if(m_Threads, [&](const auto& value) {
        return value.ThreadIndex == thread.ThreadIndex && value.Generation == thread.Generation;
    });
    if (threadAt == m_Threads.end()) m_Threads.push_back(thread);
    else if (*threadAt != thread) {
        error = "thread form changed within one generation";
        return false;
    }
    const auto at = std::ranges::find_if(m_Sites, [&](const auto& site) {
        return site.Form.ThreadIndex == form.ThreadIndex &&
            site.Form.ThreadGeneration == form.ThreadGeneration && site.Form.IP == form.IP;
    });
    if (at == m_Sites.end()) m_Sites.push_back({form, 1});
    else if (!SameSiteForm(at->Form, form)) {
        error = "instruction form changed at one site";
        return false;
    } else if (at->Visits == std::numeric_limits<std::uint64_t>::max()) {
        error = "instruction visit count overflow";
        return false;
    } else {
        ++at->Visits;
    }
    m_Session = form.Session;
    return true;
}

NativeScriptCorpusSummary NativeScriptCorpusManifest::Summary() const {
    NativeScriptCorpusSummary result;
    result.Sites = m_Sites.size();
    result.Threads = m_Threads.size();
    std::set<std::uint16_t> opcodes;
    using OperandKey = std::tuple<std::uint16_t, std::uint8_t,
        std::array<NativeScriptOperandType, 16>, std::array<std::uint8_t, 16>,
        std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>;
    std::set<OperandKey> forms;
    for (const auto& site : m_Sites) {
        result.Encounters += site.Visits;
        opcodes.insert(site.Form.RawOpcode);
        forms.emplace(site.Form.RawOpcode, site.Form.OperandCount,
            site.Form.OperandTypes, site.Form.OperandTags,
            site.Form.ArrayCounts, site.Form.ArrayFlags);
        if (site.Form.ThreadForm == NativeScriptThreadForm::Main) ++result.MainSites;
        else if (site.Form.ThreadForm == NativeScriptThreadForm::Mission) ++result.MissionSites;
        else ++result.StreamedSites;
        if (site.Form.Semantics == NativeScriptSemanticCoverage::Implemented) ++result.ImplementedSites;
        else ++result.UnsupportedSites;
    }
    result.Opcodes = opcodes.size();
    result.OperandForms = forms.size();
    return result;
}

std::uint64_t NativeScriptCorpusManifest::Fingerprint() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& thread : m_Threads) {
        Hash(hash, thread.ThreadIndex); Hash(hash, thread.BaseIP); Hash(hash, thread.LocalCount);
        Hash(hash, thread.Generation); Hash(hash, thread.MissionIndex); Hash(hash, std::uint8_t(thread.Form));
        Hash(hash, std::uint8_t(thread.UsesMissionCleanup)); Hash(hash, std::uint8_t(thread.ExclusiveMission));
        Hash(hash, std::uint8_t(thread.External));
    }
    for (const auto& site : m_Sites) {
        const auto& form = site.Form;
        Hash(hash, form.ThreadIndex); Hash(hash, form.ThreadGeneration); Hash(hash, form.IP); Hash(hash, form.NextIP);
        Hash(hash, form.Opcode); Hash(hash, form.RawOpcode); Hash(hash, form.OperandCount);
        Hash(hash, std::uint8_t(form.Semantics)); Hash(hash, std::uint8_t(form.ThreadForm));
        for (unsigned i = 0; i < form.OperandCount; ++i) {
            Hash(hash, std::uint8_t(form.OperandTypes[i])); Hash(hash, form.OperandTags[i]);
            Hash(hash, form.ArrayCounts[i]); Hash(hash, form.ArrayFlags[i]);
        }
        Hash(hash, site.Visits);
    }
    return hash;
}
