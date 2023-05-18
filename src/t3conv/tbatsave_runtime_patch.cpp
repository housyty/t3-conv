#include "tbatsave_runtime_patch.h"
#include "tbatsave_reverse_contract.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace t3conv {

namespace {

constexpr DWORD kAcadNonUiFlagRva = 0x009F95B0;
constexpr DWORD kAcadUiStateRva = 0x009E6C80;
constexpr std::uint64_t kTbatsavePostInitTailTargetRva = 0x056170;
constexpr SIZE_T kHookPatchLength = 19;
constexpr SIZE_T kStubSize = 0x400;
constexpr std::uint64_t kTbatsaveTelemetryMagic = 0x314C455454414254ULL;  // "TBATTEL1"
constexpr std::uint64_t kTbatsaveWorkerTelemetryMagic = 0x324C455454414254ULL;  // "TBATTEL2"
constexpr SIZE_T kWorkerT3StubOffset = 0x000;
constexpr SIZE_T kWorkerGeneralStubOffset = 0x100;
constexpr SIZE_T kWorkerTelemetryOffset = 0x200;
constexpr std::uint64_t kTbatsaveDirectWorkerMagic = 0x314B524F57584254ULL;  // "TBXWORK1"
constexpr DWORD kTbatsaveDirectWorkerModuleWaitMilliseconds = 30000;
constexpr std::uint32_t kTbatsaveDirectWorkerSuccessReturn = 0x13EC;
constexpr std::uint64_t kTchAcDbDatabaseCtorRva = 0x64A8D0;
constexpr std::uint64_t kTchAcDbDatabaseDtorRva = 0x64A8D6;
constexpr std::uint64_t kTchAcDbDatabaseReadDwgFileRva = 0x64A912;
constexpr std::uint64_t kTchTbatsavePreprocessGroupsRva = 0x01B310;
constexpr std::uint64_t kTchTbatsavePreprocessBlocksRva = 0x01E850;
constexpr std::uint64_t kTchSaveAsTArch3Rva = 0x027310;
constexpr std::uint64_t kTchTbatsaveSelectorBaseRva = 0x9E6C7C;
constexpr std::array<std::uint8_t, kHookPatchLength> kExpectedPostInitSignature = {
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x53,
    0x48, 0x83, 0xEC, 0x30,
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,
};
constexpr std::array<std::uint8_t, 20> kExpectedWorkerT3Signature = {
    0x48, 0x8B, 0xC4,
    0x44, 0x88, 0x48, 0x20,
    0x44, 0x89, 0x40, 0x18,
    0x48, 0x89, 0x50, 0x10,
    0x55,
    0x56,
    0x57,
    0x41, 0x54,
};
constexpr std::array<std::uint8_t, 20> kExpectedWorkerGeneralSignature = {
    0x44, 0x89, 0x4C, 0x24, 0x20,
    0x44, 0x89, 0x44, 0x24, 0x18,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x48, 0x89, 0x4C, 0x24, 0x08,
};

struct TbatsaveRuntimePatchTelemetryBlock {
    std::uint64_t magic = kTbatsaveTelemetryMagic;
    std::uint64_t hit_count = 0;
    std::uint64_t first_export_this = 0;
    std::uint64_t first_wrapper = 0;
    std::uint64_t last_export_this = 0;
    std::uint64_t last_wrapper = 0;
    std::int64_t requested_bind_mode = -1;
    std::int64_t requested_bind_ref = -1;
    std::uint64_t before_bind_mode = 0;
    std::uint64_t after_bind_mode = 0;
    std::uint64_t before_bind_ref = 0;
    std::uint64_t after_bind_ref = 0;
};

struct TbatsaveWorkerRuntimeTelemetryBlock {
    std::uint64_t magic = kTbatsaveWorkerTelemetryMagic;
    std::uint64_t worker_t3_hit_count = 0;
    std::uint64_t worker_t3_first_ctx = 0;
    std::uint64_t worker_t3_first_path = 0;
    std::uint64_t worker_t3_last_ctx = 0;
    std::uint64_t worker_t3_last_path = 0;
    std::uint32_t worker_t3_last_selector = 0;
    std::uint8_t worker_t3_last_flag = 0;
    std::uint8_t worker_t3_reserved[3] = {};
    std::uint64_t worker_general_hit_count = 0;
    std::uint64_t worker_general_first_ctx = 0;
    std::uint64_t worker_general_first_path = 0;
    std::uint64_t worker_general_last_ctx = 0;
    std::uint64_t worker_general_last_path = 0;
    std::uint32_t worker_general_last_target_version = 0;
    std::uint32_t worker_general_last_selector = 0;
};

static_assert(
    sizeof(TbatsaveWorkerRuntimeTelemetryBlock) <= 0x100,
    "worker telemetry block must fit inside worker probe allocation"
);

struct TbatsaveDirectWorkerControlBlock {
    std::uint64_t magic = kTbatsaveDirectWorkerMagic;
    std::uint32_t step = 0;
    std::uint32_t status = 0;
    std::int32_t read_status = 0;
    std::int32_t save_result = 0;
    std::uint32_t selector_base = 0;
    std::uint32_t selector = 0;
    std::uint32_t thread_exit_code = 0;
    std::uint32_t reserved = 0;
    std::uint64_t db_remote = 0;
    std::uint64_t source_remote = 0;
    std::uint64_t output_remote = 0;
};

static_assert(
    sizeof(TbatsaveDirectWorkerControlBlock) <= 0x200,
    "direct worker control block must fit inside the fixed control area"
);

struct TbatsaveRuntimePatchSession {
    DWORD process_id = 0;
    uintptr_t telemetry_remote = 0;
    uintptr_t stub_remote = 0;
    uintptr_t worker_telemetry_remote = 0;
    bool installed = false;
    TbatsaveRuntimePatchTelemetryBlock cached{};
    bool has_cached = false;
    TbatsaveWorkerRuntimeTelemetryBlock worker_cached{};
    bool has_worker_cached = false;
};

TbatsaveRuntimePatchSession g_tbatsave_patch_session{};

std::optional<ULONGLONG> QueryProcessCreationTime(const DWORD process_id) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return std::nullopt;
    }

    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    const BOOL ok = GetProcessTimes(
        process,
        &creation_time,
        &exit_time,
        &kernel_time,
        &user_time
    );
    CloseHandle(process);
    if (ok != TRUE) {
        return std::nullopt;
    }

    ULARGE_INTEGER stamp{};
    stamp.LowPart = creation_time.dwLowDateTime;
    stamp.HighPart = creation_time.dwHighDateTime;
    return stamp.QuadPart;
}

std::optional<DWORD> FindNewestProcessIdByName(const wchar_t* image_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return std::nullopt;
    }

    std::optional<DWORD> best_pid;
    std::optional<ULONGLONG> best_creation_time;
    do {
        if (_wcsicmp(entry.szExeFile, image_name) == 0) {
            const auto creation_time = QueryProcessCreationTime(entry.th32ProcessID);
            if (creation_time.has_value() &&
                (!best_creation_time.has_value() || *creation_time > *best_creation_time)) {
                best_creation_time = creation_time;
                best_pid = entry.th32ProcessID;
            }
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return best_pid;
}

std::optional<uintptr_t> FindModuleBaseAddress(DWORD process_id, const wchar_t* module_name) {
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return std::nullopt;
    }

    do {
        if (_wcsicmp(entry.szModule, module_name) == 0) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            CloseHandle(snapshot);
            return base;
        }
    } while (Module32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return std::nullopt;
}

DWORD TimeoutSecondsToMilliseconds(const int timeout_seconds) {
    if (timeout_seconds <= 0) {
        return INFINITE;
    }
    constexpr int kMaxSafeSeconds = static_cast<int>(0xFFFFFFFFUL / 1000UL);
    if (timeout_seconds >= kMaxSafeSeconds) {
        return 0xFFFFFFFFUL;
    }
    return static_cast<DWORD>(timeout_seconds * 1000);
}

std::optional<uintptr_t> DecodeInstalledHookStubAddress(
    const std::array<std::uint8_t, kHookPatchLength>& existing
) {
    if (!(existing[0] == 0x49 && existing[1] == 0xBA && existing[10] == 0x41 &&
          existing[11] == 0xFF && existing[12] == 0xE2)) {
        return std::nullopt;
    }

    uintptr_t address = 0;
    for (int i = 0; i < 8; ++i) {
        address |= static_cast<uintptr_t>(existing[2 + i]) << (i * 8);
    }
    return address;
}

template <size_t N>
std::optional<uintptr_t> DecodeInstalledHookStubAddress(
    const std::array<std::uint8_t, N>& existing
) {
    if constexpr (N < 13) {
        return std::nullopt;
    }
    if (!(existing[0] == 0x49 && existing[1] == 0xBA && existing[10] == 0x41 &&
          existing[11] == 0xFF && existing[12] == 0xE2)) {
        return std::nullopt;
    }

    uintptr_t address = 0;
    for (int i = 0; i < 8; ++i) {
        address |= static_cast<uintptr_t>(existing[2 + i]) << (i * 8);
    }
    return address;
}

bool WriteAll(HANDLE process, uintptr_t remote, const void* data, SIZE_T size) {
    SIZE_T written = 0;
    return WriteProcessMemory(
               process,
               reinterpret_cast<LPVOID>(remote),
               data,
               size,
               &written
           ) == TRUE &&
           written == size;
}

bool ReadAll(HANDLE process, uintptr_t remote, void* data, SIZE_T size) {
    SIZE_T read = 0;
    return ReadProcessMemory(
               process,
               reinterpret_cast<LPCVOID>(remote),
               data,
               size,
               &read
           ) == TRUE &&
           read == size;
}

std::string EscapeDiagnosticValue(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

template <size_t N>
bool ProtectWriteJumpPatch(
    HANDLE process,
    const uintptr_t hook_address,
    const std::array<std::uint8_t, N>& jump_patch,
    std::vector<std::string>& diagnostics,
    const char* protect_failure_key
) {
    DWORD old_protect = 0;
    if (!VirtualProtectEx(
            process,
            reinterpret_cast<LPVOID>(hook_address),
            jump_patch.size(),
            PAGE_EXECUTE_READWRITE,
            &old_protect
        )) {
        diagnostics.push_back(protect_failure_key);
        return false;
    }

    const bool wrote_hook = WriteAll(process, hook_address, jump_patch.data(), jump_patch.size());
    FlushInstructionCache(process, reinterpret_cast<LPCVOID>(hook_address), jump_patch.size());
    DWORD ignored = 0;
    VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(hook_address),
        jump_patch.size(),
        old_protect,
        &ignored
    );
    return wrote_hook;
}

void AppendMovRaxImm64(std::vector<std::uint8_t>& code, const uintptr_t value) {
    code.push_back(0x48);
    code.push_back(0xB8);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovR11QwordPtrRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x4C);
    code.push_back(0x8B);
    code.push_back(0x18);
}

void AppendMovQwordPtrRaxRcx(std::vector<std::uint8_t>& code) {
    code.push_back(0x48);
    code.push_back(0x89);
    code.push_back(0x08);
}

void AppendMovQwordPtrRaxRdx(std::vector<std::uint8_t>& code) {
    code.push_back(0x48);
    code.push_back(0x89);
    code.push_back(0x10);
}

void AppendMovQwordPtrRaxR11(std::vector<std::uint8_t>& code) {
    code.push_back(0x4C);
    code.push_back(0x89);
    code.push_back(0x18);
}

void AppendMovDwordPtrRaxR11d(std::vector<std::uint8_t>& code) {
    code.push_back(0x44);
    code.push_back(0x89);
    code.push_back(0x18);
}

void AppendMovBytePtrRaxR11b(std::vector<std::uint8_t>& code) {
    code.push_back(0x44);
    code.push_back(0x88);
    code.push_back(0x18);
}

void AppendIncQwordPtrRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x48);
    code.push_back(0xFF);
    code.push_back(0x00);
}

void AppendTestR11R11(std::vector<std::uint8_t>& code) {
    code.push_back(0x4D);
    code.push_back(0x85);
    code.push_back(0xDB);
}

void AppendTestRaxRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x48);
    code.push_back(0x85);
    code.push_back(0xC0);
}

std::size_t AppendJneRel8Placeholder(std::vector<std::uint8_t>& code) {
    code.push_back(0x75);
    code.push_back(0x00);
    return code.size() - 1;
}

std::size_t AppendJeRel8Placeholder(std::vector<std::uint8_t>& code) {
    code.push_back(0x74);
    code.push_back(0x00);
    return code.size() - 1;
}

void PatchRel8Jump(std::vector<std::uint8_t>& code, const std::size_t displacement_index) {
    const std::ptrdiff_t delta =
        static_cast<std::ptrdiff_t>(code.size()) - static_cast<std::ptrdiff_t>(displacement_index + 1);
    code[displacement_index] = static_cast<std::uint8_t>(delta);
}

void AppendMovEdxImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0xBA);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovR11dDwordPtrRaxDisp32(
    std::vector<std::uint8_t>& code,
    const std::uint32_t displacement
) {
    code.push_back(0x44);
    code.push_back(0x8B);
    code.push_back(0x98);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((displacement >> (i * 8)) & 0xFF));
    }
}

void AppendMovRaxQwordPtrRaxDisp32(
    std::vector<std::uint8_t>& code,
    const std::uint32_t displacement
) {
    code.push_back(0x48);
    code.push_back(0x8B);
    code.push_back(0x80);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((displacement >> (i * 8)) & 0xFF));
    }
}

void AppendJmpRax(std::vector<std::uint8_t>& code) {
    code.push_back(0xFF);
    code.push_back(0xE0);
}

void AppendCallRax(std::vector<std::uint8_t>& code) {
    code.push_back(0xFF);
    code.push_back(0xD0);
}

void AppendRet(std::vector<std::uint8_t>& code) {
    code.push_back(0xC3);
}

void AppendPushRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x50);
}

void AppendPopRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x58);
}

void AppendMovR11Imm64(std::vector<std::uint8_t>& code, const uintptr_t value) {
    code.push_back(0x49);
    code.push_back(0xBB);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendJmpR11(std::vector<std::uint8_t>& code) {
    code.push_back(0x41);
    code.push_back(0xFF);
    code.push_back(0xE3);
}

void AppendMovRcxRdx(std::vector<std::uint8_t>& code) {
    code.push_back(0x48);
    code.push_back(0x89);
    code.push_back(0xD1);
}

void AppendMovRcxImm64(std::vector<std::uint8_t>& code, const uintptr_t value) {
    code.push_back(0x48);
    code.push_back(0xB9);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovRdxImm64(std::vector<std::uint8_t>& code, const uintptr_t value) {
    code.push_back(0x48);
    code.push_back(0xBA);
    for (int i = 0; i < 8; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovR8dImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0x41);
    code.push_back(0xB8);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovR9dImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0x41);
    code.push_back(0xB9);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovEaxImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0xB8);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovR8dDwordPtrRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x44);
    code.push_back(0x8B);
    code.push_back(0x00);
}

void AppendMovEaxDwordPtrRax(std::vector<std::uint8_t>& code) {
    code.push_back(0x8B);
    code.push_back(0x00);
}

void AppendAddR8dImm8(std::vector<std::uint8_t>& code, const std::uint8_t value) {
    code.push_back(0x41);
    code.push_back(0x83);
    code.push_back(0xC0);
    code.push_back(value);
}

void AppendMovDwordPtrRaxEax(std::vector<std::uint8_t>& code) {
    code.push_back(0x89);
    code.push_back(0x00);
}

void AppendMovDwordPtrR11Eax(std::vector<std::uint8_t>& code) {
    code.push_back(0x41);
    code.push_back(0x89);
    code.push_back(0x03);
}

void AppendMovDwordPtrRaxR8d(std::vector<std::uint8_t>& code) {
    code.push_back(0x44);
    code.push_back(0x89);
    code.push_back(0x00);
}

void AppendTestEaxEax(std::vector<std::uint8_t>& code) {
    code.push_back(0x85);
    code.push_back(0xC0);
}

void AppendCmpEaxImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0x3D);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

std::size_t AppendJmpRel32Placeholder(std::vector<std::uint8_t>& code) {
    code.push_back(0xE9);
    const std::size_t displacement_index = code.size();
    code.insert(code.end(), 4, 0x00);
    return displacement_index;
}

std::size_t AppendJneRel32Placeholder(std::vector<std::uint8_t>& code) {
    code.push_back(0x0F);
    code.push_back(0x85);
    const std::size_t displacement_index = code.size();
    code.insert(code.end(), 4, 0x00);
    return displacement_index;
}

void PatchRel32Jump(
    std::vector<std::uint8_t>& code,
    const std::size_t displacement_index,
    const std::size_t target_index
) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target_index) -
        static_cast<std::int64_t>(displacement_index + 4);
    const std::int32_t rel = static_cast<std::int32_t>(delta);
    for (int i = 0; i < 4; ++i) {
        code[displacement_index + i] = static_cast<std::uint8_t>((rel >> (i * 8)) & 0xFF);
    }
}

void AppendMovQwordRspDisp8Imm32(
    std::vector<std::uint8_t>& code,
    const std::uint8_t displacement,
    const std::uint32_t value
) {
    code.push_back(0x48);
    code.push_back(0xC7);
    code.push_back(0x44);
    code.push_back(0x24);
    code.push_back(displacement);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendMovQwordRspDisp8Reg(
    std::vector<std::uint8_t>& code,
    const std::uint8_t displacement,
    const std::uint8_t reg_opcode
) {
    code.push_back(0x48);
    code.push_back(0x89);
    code.push_back(static_cast<std::uint8_t>(0x44 | (reg_opcode << 3)));
    code.push_back(0x24);
    code.push_back(displacement);
}

void AppendMovRegQwordRspDisp8(
    std::vector<std::uint8_t>& code,
    const std::uint8_t reg_opcode,
    const std::uint8_t displacement
) {
    code.push_back(0x48);
    code.push_back(0x8B);
    code.push_back(static_cast<std::uint8_t>(0x44 | (reg_opcode << 3)));
    code.push_back(0x24);
    code.push_back(displacement);
}

void AppendMovRegDwordRspDisp8(
    std::vector<std::uint8_t>& code,
    const std::uint8_t reg_opcode,
    const std::uint8_t displacement
) {
    code.push_back(0x44);
    code.push_back(0x8B);
    code.push_back(static_cast<std::uint8_t>(0x44 | (reg_opcode << 3)));
    code.push_back(0x24);
    code.push_back(displacement);
}

void AppendMovzxRegByteRspDisp8(
    std::vector<std::uint8_t>& code,
    const std::uint8_t reg_opcode,
    const std::uint8_t displacement
) {
    code.push_back(0x44);
    code.push_back(0x0F);
    code.push_back(0xB6);
    code.push_back(static_cast<std::uint8_t>(0x44 | (reg_opcode << 3)));
    code.push_back(0x24);
    code.push_back(displacement);
}

void AppendLeaRcxDisp32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0x48);
    code.push_back(0x8D);
    code.push_back(0x89);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendAddRcxImm32(std::vector<std::uint8_t>& code, const std::uint32_t value) {
    code.push_back(0x48);
    code.push_back(0x81);
    code.push_back(0xC1);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendSubRspImm8(std::vector<std::uint8_t>& code, const std::uint8_t value) {
    code.push_back(0x48);
    code.push_back(0x83);
    code.push_back(0xEC);
    code.push_back(value);
}

void AppendAddRspImm8(std::vector<std::uint8_t>& code, const std::uint8_t value) {
    code.push_back(0x48);
    code.push_back(0x83);
    code.push_back(0xC4);
    code.push_back(value);
}

void AppendStoreImm32(
    std::vector<std::uint8_t>& code,
    const uintptr_t address,
    const std::uint32_t value
) {
    AppendMovR11Imm64(code, address);
    code.push_back(0x41);
    code.push_back(0xC7);
    code.push_back(0x03);
    for (int i = 0; i < 4; ++i) {
        code.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void AppendStoreEax(std::vector<std::uint8_t>& code, const uintptr_t address) {
    AppendMovR11Imm64(code, address);
    AppendMovDwordPtrR11Eax(code);
}

void AppendStoreR8d(std::vector<std::uint8_t>& code, const uintptr_t address) {
    AppendMovRaxImm64(code, address);
    AppendMovDwordPtrRaxR8d(code);
}

void AppendTelemetryStoreCurrentPointers(
    std::vector<std::uint8_t>& code,
    const uintptr_t telemetry_address
) {
    AppendMovRaxImm64(code, telemetry_address + offsetof(TbatsaveRuntimePatchTelemetryBlock, hit_count));
    AppendMovR11QwordPtrRax(code);
    AppendIncQwordPtrRax(code);
    AppendTestR11R11(code);
    const std::size_t skip_first = AppendJneRel8Placeholder(code);

    AppendMovRaxImm64(
        code,
        telemetry_address + offsetof(TbatsaveRuntimePatchTelemetryBlock, first_export_this)
    );
    AppendMovQwordPtrRaxRcx(code);
    AppendMovRaxImm64(
        code,
        telemetry_address + offsetof(TbatsaveRuntimePatchTelemetryBlock, first_wrapper)
    );
    AppendMovQwordPtrRaxRdx(code);
    PatchRel8Jump(code, skip_first);

    AppendMovRaxImm64(
        code,
        telemetry_address + offsetof(TbatsaveRuntimePatchTelemetryBlock, last_export_this)
    );
    AppendMovQwordPtrRaxRcx(code);
    AppendMovRaxImm64(
        code,
        telemetry_address + offsetof(TbatsaveRuntimePatchTelemetryBlock, last_wrapper)
    );
    AppendMovQwordPtrRaxRdx(code);
}

void AppendTelemetryReadBindMode(
    std::vector<std::uint8_t>& code,
    const uintptr_t telemetry_address,
    const std::uint32_t export_offset,
    const std::uint32_t telemetry_offset
) {
    AppendMovRegQwordRspDisp8(code, 0, static_cast<std::uint8_t>(export_offset));
    AppendMovR11dDwordPtrRaxDisp32(code, static_cast<std::uint32_t>(kTbatsaveExportUiLayout.bind_mode_field_offset));
    AppendMovRaxImm64(code, telemetry_address + telemetry_offset);
    AppendMovQwordPtrRaxR11(code);
}

void AppendTelemetryReadBindRef(
    std::vector<std::uint8_t>& code,
    const uintptr_t telemetry_address,
    const std::uint32_t export_offset,
    const std::uint32_t telemetry_offset
) {
    AppendMovRegQwordRspDisp8(code, 0, static_cast<std::uint8_t>(export_offset));
    AppendMovRaxQwordPtrRaxDisp32(
        code,
        static_cast<std::uint32_t>(kTbatsaveExportUiLayout.bind_ref_control_offset)
    );
    AppendTestRaxRax(code);
    const std::size_t skip = AppendJeRel8Placeholder(code);
    AppendMovR11dDwordPtrRaxDisp32(
        code,
        static_cast<std::uint32_t>(kTbatsaveExportUiLayout.bind_ref_value_field_offset)
    );
    AppendMovRaxImm64(code, telemetry_address + telemetry_offset);
    AppendMovQwordPtrRaxR11(code);
    PatchRel8Jump(code, skip);
}

std::vector<std::uint8_t> BuildHookStub(
    const uintptr_t tch_kernal_base,
    const uintptr_t telemetry_address,
    const uintptr_t return_address,
    const std::array<std::uint8_t, kHookPatchLength>& displaced,
    const ProcessLaunchPlan& plan
) {
    const auto& layout = kTbatsaveExportUiLayout;

    std::vector<std::uint8_t> code;
    code.reserve(160);
    code.insert(code.end(), displaced.begin(), displaced.end());
    AppendSubRspImm8(code, 0x30);
    AppendMovQwordRspDisp8Reg(code, 0x20, 1);  // [rsp+0x20] = rcx (export object)
    AppendMovQwordRspDisp8Reg(code, 0x28, 2);  // [rsp+0x28] = rdx (wrapper)
    AppendTelemetryStoreCurrentPointers(code, telemetry_address);
    AppendTelemetryReadBindMode(
        code,
        telemetry_address,
        0x20,
        static_cast<std::uint32_t>(offsetof(TbatsaveRuntimePatchTelemetryBlock, before_bind_mode))
    );
    AppendTelemetryReadBindRef(
        code,
        telemetry_address,
        0x20,
        static_cast<std::uint32_t>(offsetof(TbatsaveRuntimePatchTelemetryBlock, before_bind_ref))
    );

    if (plan.tbatsave_bind_mode != TbatsaveBindMode::kKeepDefault) {
        AppendMovRegQwordRspDisp8(code, 1, 0x20);  // rcx = export object
        AppendMovEdxImm32(code, static_cast<std::uint32_t>(static_cast<int>(plan.tbatsave_bind_mode)));
        AppendMovRaxImm64(code, tch_kernal_base + kTbatsaveBindModeSetterRva);
        AppendCallRax(code);
    }

    if (plan.tbatsave_bind_ref >= 0) {
        AppendMovRegQwordRspDisp8(code, 1, 0x20);  // rcx = export object
        AppendAddRcxImm32(code, static_cast<std::uint32_t>(layout.bind_ref_control_offset));
        AppendMovEdxImm32(code, static_cast<std::uint32_t>(plan.tbatsave_bind_ref));
        AppendMovRaxImm64(code, tch_kernal_base + kTbatsaveBindRefSetterRva);
        AppendCallRax(code);

        AppendMovRegQwordRspDisp8(code, 1, 0x20);  // rcx = export object
        AppendAddRcxImm32(code, static_cast<std::uint32_t>(layout.bind_ref_control_offset));
        AppendMovRaxImm64(code, tch_kernal_base + 0x1BCE20);
        AppendCallRax(code);
    }

    AppendTelemetryReadBindMode(
        code,
        telemetry_address,
        0x20,
        static_cast<std::uint32_t>(offsetof(TbatsaveRuntimePatchTelemetryBlock, after_bind_mode))
    );
    AppendTelemetryReadBindRef(
        code,
        telemetry_address,
        0x20,
        static_cast<std::uint32_t>(offsetof(TbatsaveRuntimePatchTelemetryBlock, after_bind_ref))
    );
    AppendMovRegQwordRspDisp8(code, 2, 0x28);  // rdx = wrapper
    AppendAddRspImm8(code, 0x30);
    AppendMovRaxImm64(code, return_address);
    AppendJmpRax(code);
    return code;
}

enum class WorkerTelemetryKind {
    kWorkerT3,
    kWorkerGeneral,
};

template <size_t N>
std::vector<std::uint8_t> BuildWorkerHookStub(
    const uintptr_t telemetry_address,
    const uintptr_t return_address,
    const std::array<std::uint8_t, N>& displaced,
    const WorkerTelemetryKind kind
) {
    std::vector<std::uint8_t> code;
    code.reserve(192);
    code.insert(code.end(), displaced.begin(), displaced.end());
    AppendPushRax(code);
    AppendSubRspImm8(code, 0x20);

    if (kind == WorkerTelemetryKind::kWorkerT3) {
        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_hit_count)
        );
        AppendMovR11QwordPtrRax(code);
        AppendIncQwordPtrRax(code);
        AppendTestR11R11(code);
        const std::size_t skip_first = AppendJneRel8Placeholder(code);

        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_first_ctx)
        );
        AppendMovQwordPtrRaxRcx(code);
        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_first_path)
        );
        AppendMovQwordPtrRaxRdx(code);
        PatchRel8Jump(code, skip_first);

        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_last_ctx)
        );
        AppendMovQwordPtrRaxRcx(code);
        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_last_path)
        );
        AppendMovQwordPtrRaxRdx(code);
        AppendMovzxRegByteRspDisp8(code, 3, 0x68);
        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_last_flag)
        );
        AppendMovBytePtrRaxR11b(code);
        AppendMovRegDwordRspDisp8(code, 3, 0x60);
        AppendMovRaxImm64(
            code,
            telemetry_address + offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_t3_last_selector)
        );
        AppendMovDwordPtrRaxR11d(code);
    } else {
        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_hit_count)
        );
        AppendMovR11QwordPtrRax(code);
        AppendIncQwordPtrRax(code);
        AppendTestR11R11(code);
        const std::size_t skip_first = AppendJneRel8Placeholder(code);

        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_first_ctx)
        );
        AppendMovQwordPtrRaxRcx(code);
        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_first_path)
        );
        AppendMovQwordPtrRaxRdx(code);
        PatchRel8Jump(code, skip_first);

        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_last_ctx)
        );
        AppendMovQwordPtrRaxRcx(code);
        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_last_path)
        );
        AppendMovQwordPtrRaxRdx(code);
        AppendMovRegDwordRspDisp8(code, 3, 0x40);
        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_last_target_version)
        );
        AppendMovDwordPtrRaxR11d(code);
        AppendMovRegDwordRspDisp8(code, 3, 0x48);
        AppendMovRaxImm64(
            code,
            telemetry_address +
                offsetof(TbatsaveWorkerRuntimeTelemetryBlock, worker_general_last_selector)
        );
        AppendMovDwordPtrRaxR11d(code);
    }

    AppendAddRspImm8(code, 0x20);
    AppendPopRax(code);
    AppendMovR11Imm64(code, return_address);
    AppendJmpR11(code);
    return code;
}

std::array<std::uint8_t, kHookPatchLength> BuildAbsoluteJumpPatch(const uintptr_t destination) {
    std::array<std::uint8_t, kHookPatchLength> patch{};
    patch[0] = 0x49;
    patch[1] = 0xBA;
    for (int i = 0; i < 8; ++i) {
        patch[2 + i] = static_cast<std::uint8_t>((destination >> (i * 8)) & 0xFF);
    }
    patch[10] = 0x41;
    patch[11] = 0xFF;
    patch[12] = 0xE2;
    patch[13] = 0x90;
    patch[14] = 0x90;
    patch[15] = 0x90;
    patch[16] = 0x90;
    patch[17] = 0x90;
    patch[18] = 0x90;
    return patch;
}

template <size_t N>
std::array<std::uint8_t, N> BuildAbsoluteJumpPatch(const uintptr_t destination) {
    std::array<std::uint8_t, N> patch{};
    patch[0] = 0x49;
    patch[1] = 0xBA;
    for (int i = 0; i < 8; ++i) {
        patch[2 + i] = static_cast<std::uint8_t>((destination >> (i * 8)) & 0xFF);
    }
    patch[10] = 0x41;
    patch[11] = 0xFF;
    patch[12] = 0xE2;
    for (size_t index = 13; index < N; ++index) {
        patch[index] = 0x90;
    }
    return patch;
}

bool TryWriteNonUiFlags(HANDLE process, const uintptr_t tch_kernal_base) {
    const DWORD non_ui_enabled = 1;
    const DWORD ui_state_enabled = 1;
    return WriteAll(process, tch_kernal_base + kAcadNonUiFlagRva, &non_ui_enabled, sizeof(non_ui_enabled)) &&
           WriteAll(process, tch_kernal_base + kAcadUiStateRva, &ui_state_enabled, sizeof(ui_state_enabled));
}

std::vector<std::string> FormatTelemetryLines(
    const TbatsaveRuntimePatchTelemetryBlock& telemetry,
    const char* source
) {
    std::vector<std::string> lines;
    std::ostringstream stream;

    lines.push_back(std::string("tbatsave_telemetry_source=") + source);
    stream << "tbatsave_telemetry_hit_count=" << telemetry.hit_count;
    lines.push_back(stream.str());
    stream.str("");
    stream.clear();

    auto append_hex = [&lines](const char* key, const std::uint64_t value) {
        std::ostringstream local;
        local << key << "0x" << std::hex << value;
        lines.push_back(local.str());
    };
    auto append_dec = [&lines](const char* key, const std::int64_t value) {
        std::ostringstream local;
        local << key << value;
        lines.push_back(local.str());
    };

    append_hex("tbatsave_telemetry_first_this=", telemetry.first_export_this);
    append_hex("tbatsave_telemetry_first_wrapper=", telemetry.first_wrapper);
    append_hex("tbatsave_telemetry_last_this=", telemetry.last_export_this);
    append_hex("tbatsave_telemetry_last_wrapper=", telemetry.last_wrapper);
    append_dec("tbatsave_telemetry_requested_bind_mode=", telemetry.requested_bind_mode);
    append_dec("tbatsave_telemetry_requested_bind_ref=", telemetry.requested_bind_ref);
    append_hex("tbatsave_telemetry_before_bind_mode=", telemetry.before_bind_mode);
    append_hex("tbatsave_telemetry_after_bind_mode=", telemetry.after_bind_mode);
    append_hex("tbatsave_telemetry_before_bind_ref=", telemetry.before_bind_ref);
    append_hex("tbatsave_telemetry_after_bind_ref=", telemetry.after_bind_ref);
    return lines;
}

std::vector<std::string> FormatWorkerTelemetryLines(
    const TbatsaveWorkerRuntimeTelemetryBlock& telemetry,
    const char* source
) {
    std::vector<std::string> lines;
    lines.push_back(std::string("tbatsave_worker_telemetry_source=") + source);

    auto append_hex = [&lines](const char* key, const std::uint64_t value) {
        std::ostringstream local;
        local << key << "0x" << std::hex << value;
        lines.push_back(local.str());
    };
    auto append_dec64 = [&lines](const char* key, const std::uint64_t value) {
        std::ostringstream local;
        local << key << value;
        lines.push_back(local.str());
    };
    auto append_dec32 = [&lines](const char* key, const std::uint32_t value) {
        std::ostringstream local;
        local << key << value;
        lines.push_back(local.str());
    };

    append_dec64("tbatsave_telemetry_worker_t3_hit_count=", telemetry.worker_t3_hit_count);
    append_hex("tbatsave_telemetry_worker_t3_first_ctx=", telemetry.worker_t3_first_ctx);
    append_hex("tbatsave_telemetry_worker_t3_first_path=", telemetry.worker_t3_first_path);
    append_hex("tbatsave_telemetry_worker_t3_last_ctx=", telemetry.worker_t3_last_ctx);
    append_hex("tbatsave_telemetry_worker_t3_last_path=", telemetry.worker_t3_last_path);
    append_dec32("tbatsave_telemetry_worker_t3_last_selector=", telemetry.worker_t3_last_selector);
    append_dec32("tbatsave_telemetry_worker_t3_last_flag=", telemetry.worker_t3_last_flag);

    append_dec64(
        "tbatsave_telemetry_worker_general_hit_count=",
        telemetry.worker_general_hit_count
    );
    append_hex(
        "tbatsave_telemetry_worker_general_first_ctx=",
        telemetry.worker_general_first_ctx
    );
    append_hex(
        "tbatsave_telemetry_worker_general_first_path=",
        telemetry.worker_general_first_path
    );
    append_hex(
        "tbatsave_telemetry_worker_general_last_ctx=",
        telemetry.worker_general_last_ctx
    );
    append_hex(
        "tbatsave_telemetry_worker_general_last_path=",
        telemetry.worker_general_last_path
    );
    append_dec32(
        "tbatsave_telemetry_worker_general_last_target_version=",
        telemetry.worker_general_last_target_version
    );
    append_dec32(
        "tbatsave_telemetry_worker_general_last_selector=",
        telemetry.worker_general_last_selector
    );
    return lines;
}

std::optional<TbatsaveRuntimePatchTelemetryBlock> TryReadLiveTelemetry() {
    if (!g_tbatsave_patch_session.installed || g_tbatsave_patch_session.process_id == 0 ||
        g_tbatsave_patch_session.telemetry_remote == 0) {
        return std::nullopt;
    }

    HANDLE process = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        g_tbatsave_patch_session.process_id
    );
    if (process == nullptr) {
        return std::nullopt;
    }

    TbatsaveRuntimePatchTelemetryBlock telemetry{};
    const bool ok = ReadAll(
        process,
        g_tbatsave_patch_session.telemetry_remote,
        &telemetry,
        sizeof(telemetry)
    );
    CloseHandle(process);
    if (!ok || telemetry.magic != kTbatsaveTelemetryMagic) {
        return std::nullopt;
    }
    return telemetry;
}

std::optional<TbatsaveWorkerRuntimeTelemetryBlock> TryReadLiveWorkerTelemetry() {
    if (!g_tbatsave_patch_session.installed || g_tbatsave_patch_session.process_id == 0 ||
        g_tbatsave_patch_session.worker_telemetry_remote == 0) {
        return std::nullopt;
    }

    HANDLE process = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        g_tbatsave_patch_session.process_id
    );
    if (process == nullptr) {
        return std::nullopt;
    }

    TbatsaveWorkerRuntimeTelemetryBlock telemetry{};
    const bool ok = ReadAll(
        process,
        g_tbatsave_patch_session.worker_telemetry_remote,
        &telemetry,
        sizeof(telemetry)
    );
    CloseHandle(process);
    if (!ok || telemetry.magic != kTbatsaveWorkerTelemetryMagic) {
        return std::nullopt;
    }
    return telemetry;
}

std::optional<std::string> TryReadRemoteUtf16String(
    HANDLE process,
    const uintptr_t remote,
    const size_t max_wchars
) {
    if (remote == 0 || max_wchars == 0 || max_wchars > 4096) {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(max_wchars + 1, L'\0');
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(remote),
            buffer.data(),
            max_wchars * sizeof(wchar_t),
            &bytes_read
        ) != TRUE ||
        bytes_read < sizeof(wchar_t)) {
        return std::nullopt;
    }

    const size_t wchar_count = bytes_read / sizeof(wchar_t);
    size_t actual_length = 0;
    for (; actual_length < wchar_count; ++actual_length) {
        const wchar_t ch = buffer[actual_length];
        if (ch == L'\0') {
            break;
        }
        if ((ch < 0x20 && ch != L'\\' && ch != L'/' && ch != L':') || ch == 0xFFFF) {
            return std::nullopt;
        }
    }
    if (actual_length == 0 || actual_length == wchar_count) {
        return std::nullopt;
    }

    const int utf8_size = WideCharToMultiByte(
        CP_UTF8,
        0,
        buffer.data(),
        static_cast<int>(actual_length),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (utf8_size <= 0) {
        return std::nullopt;
    }

    std::string utf8(static_cast<size_t>(utf8_size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            buffer.data(),
            static_cast<int>(actual_length),
            utf8.data(),
            utf8_size,
            nullptr,
            nullptr
        ) <= 0) {
        return std::nullopt;
    }
    return utf8;
}

std::optional<std::string> TryDecodeWorkerPathText(
    HANDLE process,
    const uintptr_t candidate
) {
    if (candidate == 0) {
        return std::nullopt;
    }

    if (const auto direct = TryReadRemoteUtf16String(process, candidate, 520); direct.has_value()) {
        return direct;
    }

    constexpr std::array<uintptr_t, 6> kPointerOffsets = {
        0x0,
        0x8,
        0x10,
        0x18,
        0x20,
        0x28,
    };
    for (const uintptr_t offset : kPointerOffsets) {
        uintptr_t nested = 0;
        if (!ReadAll(process, candidate + offset, &nested, sizeof(nested)) || nested == 0) {
            continue;
        }
        if (const auto nested_text = TryReadRemoteUtf16String(process, nested, 520);
            nested_text.has_value()) {
            return nested_text;
        }
    }

    return std::nullopt;
}

bool TryRefreshSessionFromInstalledHook(std::vector<std::string>& diagnostics) {
    const auto acad_pid = FindNewestProcessIdByName(L"acad.exe");
    if (!acad_pid.has_value()) {
        diagnostics.push_back("tbatsave_patch=acad_not_found");
        return false;
    }

    const auto tch_kernal_base = FindModuleBaseAddress(*acad_pid, L"tch_kernal.arx");
    if (!tch_kernal_base.has_value()) {
        diagnostics.push_back("tbatsave_patch=tch_kernal_not_loaded");
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        *acad_pid
    );
    if (process == nullptr) {
        diagnostics.push_back("tbatsave_patch=open_process_failed");
        return false;
    }

    const uintptr_t hook_address = *tch_kernal_base + kTbatsavePostInitCallbackRva;
    std::array<std::uint8_t, kHookPatchLength> existing{};
    if (!ReadAll(process, hook_address, existing.data(), existing.size())) {
        diagnostics.push_back("tbatsave_patch=read_hook_target_failed");
        CloseHandle(process);
        return false;
    }

    const auto remote_stub_address = DecodeInstalledHookStubAddress(existing);
    if (!remote_stub_address.has_value()) {
        diagnostics.push_back("tbatsave_patch=not_installed");
        CloseHandle(process);
        return false;
    }

    const uintptr_t remote_telemetry_address = *remote_stub_address + kStubSize;
    TbatsaveRuntimePatchTelemetryBlock telemetry{};
    if (!ReadAll(process, remote_telemetry_address, &telemetry, sizeof(telemetry)) ||
        telemetry.magic != kTbatsaveTelemetryMagic) {
        diagnostics.push_back("tbatsave_patch=telemetry_unreadable");
        CloseHandle(process);
        return false;
    }

    const uintptr_t worker_t3_hook_address = *tch_kernal_base + 0x027310;
    std::array<std::uint8_t, kExpectedWorkerT3Signature.size()> worker_t3_existing{};
    if (!ReadAll(
            process,
            worker_t3_hook_address,
            worker_t3_existing.data(),
            worker_t3_existing.size()
        )) {
        diagnostics.push_back("tbatsave_patch=worker_t3_read_hook_target_failed");
        CloseHandle(process);
        return false;
    }

    const auto remote_worker_t3_stub_address = DecodeInstalledHookStubAddress(worker_t3_existing);
    if (!remote_worker_t3_stub_address.has_value()) {
        diagnostics.push_back("tbatsave_patch=worker_t3_not_installed");
        CloseHandle(process);
        return false;
    }

    const uintptr_t worker_telemetry_address =
        *remote_worker_t3_stub_address - kWorkerT3StubOffset + kWorkerTelemetryOffset;
    TbatsaveWorkerRuntimeTelemetryBlock worker_telemetry{};
    if (!ReadAll(process, worker_telemetry_address, &worker_telemetry, sizeof(worker_telemetry)) ||
        worker_telemetry.magic != kTbatsaveWorkerTelemetryMagic) {
        diagnostics.push_back("tbatsave_patch=worker_telemetry_unreadable");
        CloseHandle(process);
        return false;
    }

    CloseHandle(process);
    g_tbatsave_patch_session.process_id = *acad_pid;
    g_tbatsave_patch_session.telemetry_remote = remote_telemetry_address;
    g_tbatsave_patch_session.stub_remote = *remote_stub_address;
    g_tbatsave_patch_session.worker_telemetry_remote = worker_telemetry_address;
    g_tbatsave_patch_session.installed = true;
    g_tbatsave_patch_session.cached = telemetry;
    g_tbatsave_patch_session.has_cached = true;
    g_tbatsave_patch_session.worker_cached = worker_telemetry;
    g_tbatsave_patch_session.has_worker_cached = true;
    diagnostics.push_back("tbatsave_patch=session_refreshed_from_installed_hook");
    return true;
}

std::wstring ToWidePath(const std::filesystem::path& path) {
    return path.wstring();
}

std::filesystem::path DeriveTbatsaveBatchTargetPath(const ProcessLaunchPlan& plan) {
    return plan.batch_output_dir /
           (plan.stage_source_path.stem().string() + "_t3" +
            plan.stage_source_path.extension().string());
}

bool WriteDirectWorkerResultFile(
    const ProcessLaunchPlan& plan,
    const bool success,
    const TbatsaveDirectWorkerControlBlock& control,
    const std::string& detail
) {
    if (plan.worker_status_path.empty()) {
        return false;
    }

    std::error_code error_code;
    std::filesystem::create_directories(plan.worker_status_path.parent_path(), error_code);
    std::ofstream stream(plan.worker_status_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }

    const std::filesystem::path batch_target = DeriveTbatsaveBatchTargetPath(plan);
    stream << (success ? "DONE" : "FAILED") << "\n";
    stream << "mode=tbatsave-direct-worker\n";
    stream << "src=" << plan.stage_source_path.string() << "\n";
    stream << "expected=" << batch_target.string() << "\n";
    stream << "step=" << control.step << "\n";
    stream << "status=" << control.status << "\n";
    stream << "read_status=" << control.read_status << "\n";
    stream << "save_result=" << control.save_result << "\n";
    stream << "selector_base=" << control.selector_base << "\n";
    stream << "selector=" << control.selector << "\n";
    stream << "thread_exit_code=" << control.thread_exit_code << "\n";
    if (!detail.empty()) {
        stream << "detail=" << detail << "\n";
    }
    return stream.good();
}

std::vector<std::uint8_t> BuildTbatsaveDirectWorkerStub(
    const uintptr_t tch_kernal_base,
    const uintptr_t control_address,
    const uintptr_t db_address,
    const uintptr_t source_address,
    const uintptr_t output_address
) {
    const uintptr_t step_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, step);
    const uintptr_t status_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, status);
    const uintptr_t read_status_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, read_status);
    const uintptr_t save_result_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, save_result);
    const uintptr_t selector_base_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, selector_base);
    const uintptr_t selector_address =
        control_address + offsetof(TbatsaveDirectWorkerControlBlock, selector);

    std::vector<std::uint8_t> code;
    code.reserve(512);
    AppendSubRspImm8(code, 0x48);
    AppendStoreImm32(code, step_address, 1);

    // AcDbDatabase::AcDbDatabase(false, true)
    AppendMovRcxImm64(code, db_address);
    AppendMovEdxImm32(code, 0);
    AppendMovR8dImm32(code, 1);
    AppendMovRaxImm64(code, tch_kernal_base + kTchAcDbDatabaseCtorRva);
    AppendCallRax(code);
    AppendStoreImm32(code, step_address, 2);

    // Acad::ErrorStatus AcDbDatabase::readDwgFile(src, 3, false, nullptr)
    AppendMovQwordRspDisp8Imm32(code, 0x20, 0);
    AppendMovRcxImm64(code, db_address);
    AppendMovRdxImm64(code, source_address);
    AppendMovR8dImm32(code, 3);
    AppendMovR9dImm32(code, 0);
    AppendMovRaxImm64(code, tch_kernal_base + kTchAcDbDatabaseReadDwgFileRva);
    AppendCallRax(code);
    AppendStoreEax(code, read_status_address);
    AppendStoreImm32(code, step_address, 3);
    AppendTestEaxEax(code);
    const std::size_t read_failed_jump = AppendJneRel32Placeholder(code);

    AppendMovRcxImm64(code, db_address);
    AppendMovRaxImm64(code, tch_kernal_base + kTchTbatsavePreprocessGroupsRva);
    AppendCallRax(code);
    AppendStoreImm32(code, step_address, 4);

    AppendMovRcxImm64(code, db_address);
    AppendMovRaxImm64(code, tch_kernal_base + kTchTbatsavePreprocessBlocksRva);
    AppendCallRax(code);
    AppendStoreImm32(code, step_address, 5);

    AppendMovRaxImm64(code, tch_kernal_base + kTchTbatsaveSelectorBaseRva);
    AppendMovEaxDwordPtrRax(code);
    AppendMovR8dImm32(code, 0);
    AppendStoreEax(code, selector_base_address);
    AppendMovRaxImm64(code, tch_kernal_base + kTchTbatsaveSelectorBaseRva);
    AppendMovR8dDwordPtrRax(code);
    AppendAddR8dImm8(code, 0x0E);
    AppendStoreR8d(code, selector_address);

    // int SaveAsTArch3(db, out_path, selector, false)
    AppendMovRcxImm64(code, db_address);
    AppendMovRdxImm64(code, output_address);
    AppendMovR9dImm32(code, 0);
    AppendMovRaxImm64(code, tch_kernal_base + kTchSaveAsTArch3Rva);
    AppendCallRax(code);
    AppendStoreEax(code, save_result_address);
    AppendStoreImm32(code, step_address, 6);
    AppendCmpEaxImm32(code, kTbatsaveDirectWorkerSuccessReturn);
    const std::size_t save_failed_jump = AppendJneRel32Placeholder(code);

    AppendStoreImm32(code, status_address, 1);
    const std::size_t cleanup_jump = AppendJmpRel32Placeholder(code);

    const std::size_t read_failed_label = code.size();
    AppendStoreImm32(code, status_address, 2);
    const std::size_t cleanup_after_read_failure_jump = AppendJmpRel32Placeholder(code);

    const std::size_t save_failed_label = code.size();
    AppendStoreImm32(code, status_address, 3);

    const std::size_t cleanup_label = code.size();
    AppendStoreImm32(code, step_address, 7);
    AppendMovRcxImm64(code, db_address);
    AppendMovRaxImm64(code, tch_kernal_base + kTchAcDbDatabaseDtorRva);
    AppendCallRax(code);
    AppendStoreImm32(code, step_address, 8);
    AppendMovRaxImm64(code, status_address);
    AppendMovEaxDwordPtrRax(code);
    AppendAddRspImm8(code, 0x48);
    AppendRet(code);

    PatchRel32Jump(code, read_failed_jump, read_failed_label);
    PatchRel32Jump(code, save_failed_jump, save_failed_label);
    PatchRel32Jump(code, cleanup_jump, cleanup_label);
    PatchRel32Jump(code, cleanup_after_read_failure_jump, cleanup_label);
    return code;
}

std::optional<uintptr_t> WaitForTchKernalBase(
    const DWORD process_id,
    std::vector<std::string>& diagnostics
) {
    const ULONGLONG started = GetTickCount64();
    while (GetTickCount64() - started < kTbatsaveDirectWorkerModuleWaitMilliseconds) {
        if (const auto base = FindModuleBaseAddress(process_id, L"tch_kernal.arx");
            base.has_value()) {
            return base;
        }
        Sleep(500);
    }
    diagnostics.push_back("tbatsave_direct_worker=tch_kernal_not_loaded");
    return std::nullopt;
}

}  // namespace

bool TryInstallTbatsaveRuntimePatch(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    const auto acad_pid = FindNewestProcessIdByName(L"acad.exe");
    if (!acad_pid.has_value()) {
        diagnostics.push_back("tbatsave_patch=acad_not_found");
        return false;
    }

    const auto tch_kernal_base = FindModuleBaseAddress(*acad_pid, L"tch_kernal.arx");
    if (!tch_kernal_base.has_value()) {
        diagnostics.push_back("tbatsave_patch=tch_kernal_not_loaded");
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE,
        *acad_pid
    );
    if (process == nullptr) {
        diagnostics.push_back("tbatsave_patch=open_process_failed");
        return false;
    }

    const uintptr_t hook_address = *tch_kernal_base + kTbatsavePostInitCallbackRva;
    const uintptr_t return_address = hook_address + kHookPatchLength;
    const uintptr_t worker_t3_hook_address = *tch_kernal_base + 0x027310;
    const uintptr_t worker_t3_return_address =
        worker_t3_hook_address + kExpectedWorkerT3Signature.size();
    const uintptr_t worker_general_hook_address = *tch_kernal_base + 0x023A80;
    const uintptr_t worker_general_return_address =
        worker_general_hook_address + kExpectedWorkerGeneralSignature.size();

    std::array<std::uint8_t, kHookPatchLength> existing{};
    if (!ReadAll(process, hook_address, existing.data(), existing.size())) {
        diagnostics.push_back("tbatsave_patch=read_hook_target_failed");
        CloseHandle(process);
        return false;
    }

    std::array<std::uint8_t, kExpectedWorkerT3Signature.size()> worker_t3_existing{};
    if (!ReadAll(
            process,
            worker_t3_hook_address,
            worker_t3_existing.data(),
            worker_t3_existing.size()
        )) {
        diagnostics.push_back("tbatsave_patch=worker_t3_read_hook_target_failed");
        CloseHandle(process);
        return false;
    }

    std::array<std::uint8_t, kExpectedWorkerGeneralSignature.size()> worker_general_existing{};
    if (!ReadAll(
            process,
            worker_general_hook_address,
            worker_general_existing.data(),
            worker_general_existing.size()
        )) {
        diagnostics.push_back("tbatsave_patch=worker_general_read_hook_target_failed");
        CloseHandle(process);
        return false;
    }

    if (const auto remote_stub_address = DecodeInstalledHookStubAddress(existing);
        remote_stub_address.has_value()) {
        const uintptr_t remote_telemetry_address = *remote_stub_address + kStubSize;
        TbatsaveRuntimePatchTelemetryBlock telemetry{};
        TbatsaveRuntimePatchTelemetryBlock previous{};
        if (ReadAll(process, remote_telemetry_address, &previous, sizeof(previous)) &&
            previous.magic == kTbatsaveTelemetryMagic) {
            telemetry = previous;
        }
        telemetry.requested_bind_mode =
            static_cast<std::int64_t>(static_cast<int>(plan.tbatsave_bind_mode));
        telemetry.requested_bind_ref = static_cast<std::int64_t>(plan.tbatsave_bind_ref);
        telemetry.before_bind_mode = 0;
        telemetry.after_bind_mode = 0;
        telemetry.before_bind_ref = 0;
        telemetry.after_bind_ref = 0;

        auto remote_worker_t3_stub_address = DecodeInstalledHookStubAddress(worker_t3_existing);
        auto remote_worker_general_stub_address = DecodeInstalledHookStubAddress(worker_general_existing);
        uintptr_t worker_probe_base = 0;
        uintptr_t worker_telemetry_address = 0;
        TbatsaveWorkerRuntimeTelemetryBlock worker_telemetry{};

        if (!remote_worker_t3_stub_address.has_value() ||
            !remote_worker_general_stub_address.has_value()) {
            if (worker_t3_existing != kExpectedWorkerT3Signature) {
                diagnostics.push_back("tbatsave_patch=worker_t3_signature_mismatch");
                CloseHandle(process);
                return false;
            }
            if (worker_general_existing != kExpectedWorkerGeneralSignature) {
                diagnostics.push_back("tbatsave_patch=worker_general_signature_mismatch");
                CloseHandle(process);
                return false;
            }

            constexpr SIZE_T kWorkerUpgradeAllocationSize = 0x300;
            LPVOID worker_probe_remote = VirtualAllocEx(
                process,
                nullptr,
                kWorkerUpgradeAllocationSize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE
            );
            if (worker_probe_remote == nullptr) {
                diagnostics.push_back("tbatsave_patch=alloc_worker_upgrade_failed");
                CloseHandle(process);
                return false;
            }

            worker_probe_base = reinterpret_cast<uintptr_t>(worker_probe_remote);
            remote_worker_t3_stub_address = worker_probe_base + kWorkerT3StubOffset;
            remote_worker_general_stub_address = worker_probe_base + kWorkerGeneralStubOffset;
            worker_telemetry_address = worker_probe_base + kWorkerTelemetryOffset;

            if (!WriteAll(
                    process,
                    worker_telemetry_address,
                    &worker_telemetry,
                    sizeof(worker_telemetry)
                )) {
                diagnostics.push_back("tbatsave_patch=write_worker_upgrade_telemetry_failed");
                VirtualFreeEx(process, worker_probe_remote, 0, MEM_RELEASE);
                CloseHandle(process);
                return false;
            }

            const std::vector<std::uint8_t> worker_t3_stub = BuildWorkerHookStub(
                worker_telemetry_address,
                worker_t3_return_address,
                worker_t3_existing,
                WorkerTelemetryKind::kWorkerT3
            );
            if (!WriteAll(
                    process,
                    *remote_worker_t3_stub_address,
                    worker_t3_stub.data(),
                    worker_t3_stub.size()
                )) {
                diagnostics.push_back("tbatsave_patch=write_worker_t3_upgrade_stub_failed");
                VirtualFreeEx(process, worker_probe_remote, 0, MEM_RELEASE);
                CloseHandle(process);
                return false;
            }

            const std::vector<std::uint8_t> worker_general_stub = BuildWorkerHookStub(
                worker_telemetry_address,
                worker_general_return_address,
                worker_general_existing,
                WorkerTelemetryKind::kWorkerGeneral
            );
            if (!WriteAll(
                    process,
                    *remote_worker_general_stub_address,
                    worker_general_stub.data(),
                    worker_general_stub.size()
                )) {
                diagnostics.push_back("tbatsave_patch=write_worker_general_upgrade_stub_failed");
                VirtualFreeEx(process, worker_probe_remote, 0, MEM_RELEASE);
                CloseHandle(process);
                return false;
            }

            const auto worker_t3_jump_patch =
                BuildAbsoluteJumpPatch<kExpectedWorkerT3Signature.size()>(
                    *remote_worker_t3_stub_address
                );
            if (!ProtectWriteJumpPatch(
                    process,
                    worker_t3_hook_address,
                    worker_t3_jump_patch,
                    diagnostics,
                    "tbatsave_patch=protect_worker_t3_hook_failed"
                )) {
                VirtualFreeEx(process, worker_probe_remote, 0, MEM_RELEASE);
                CloseHandle(process);
                return false;
            }

            const auto worker_general_jump_patch =
                BuildAbsoluteJumpPatch<kExpectedWorkerGeneralSignature.size()>(
                    *remote_worker_general_stub_address
                );
            if (!ProtectWriteJumpPatch(
                    process,
                    worker_general_hook_address,
                    worker_general_jump_patch,
                    diagnostics,
                    "tbatsave_patch=protect_worker_general_hook_failed"
                )) {
                CloseHandle(process);
                return false;
            }

            diagnostics.push_back("tbatsave_patch=worker_hooks_upgraded");
        } else {
            worker_probe_base = *remote_worker_t3_stub_address - kWorkerT3StubOffset;
            worker_telemetry_address = worker_probe_base + kWorkerTelemetryOffset;
            TbatsaveWorkerRuntimeTelemetryBlock previous_worker{};
            if (ReadAll(process, worker_telemetry_address, &previous_worker, sizeof(previous_worker)) &&
                previous_worker.magic == kTbatsaveWorkerTelemetryMagic) {
                worker_telemetry = previous_worker;
            }
        }

        const bool wrote_postinit_telemetry =
            WriteAll(process, remote_telemetry_address, &telemetry, sizeof(telemetry));
        const bool wrote_worker_telemetry = WriteAll(
            process,
            worker_telemetry_address,
            &worker_telemetry,
            sizeof(worker_telemetry)
        );
        if (wrote_postinit_telemetry && wrote_worker_telemetry) {
            g_tbatsave_patch_session.process_id = *acad_pid;
            g_tbatsave_patch_session.telemetry_remote = remote_telemetry_address;
            g_tbatsave_patch_session.stub_remote = *remote_stub_address;
            g_tbatsave_patch_session.worker_telemetry_remote = worker_telemetry_address;
            g_tbatsave_patch_session.installed = true;
            g_tbatsave_patch_session.cached = telemetry;
            g_tbatsave_patch_session.has_cached = true;
            g_tbatsave_patch_session.worker_cached = worker_telemetry;
            g_tbatsave_patch_session.has_worker_cached = true;
            diagnostics.push_back("tbatsave_patch=already_installed_refreshed_parameters");
        } else {
            diagnostics.push_back("tbatsave_patch=already_installed_reuses_previous_parameters");
        }
        const bool wrote_flags = TryWriteNonUiFlags(process, *tch_kernal_base);
        CloseHandle(process);
        return wrote_flags;
    }

    if (existing != kExpectedPostInitSignature) {
        diagnostics.push_back("tbatsave_patch=signature_mismatch");
        CloseHandle(process);
        return false;
    }
    if (worker_t3_existing != kExpectedWorkerT3Signature) {
        diagnostics.push_back("tbatsave_patch=worker_t3_signature_mismatch");
        CloseHandle(process);
        return false;
    }
    if (worker_general_existing != kExpectedWorkerGeneralSignature) {
        diagnostics.push_back("tbatsave_patch=worker_general_signature_mismatch");
        CloseHandle(process);
        return false;
    }

    const SIZE_T remote_allocation_size =
        kStubSize + sizeof(TbatsaveRuntimePatchTelemetryBlock) + 0x300;
    LPVOID remote_stub = VirtualAllocEx(
        process,
        nullptr,
        remote_allocation_size,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );
    if (remote_stub == nullptr) {
        diagnostics.push_back("tbatsave_patch=alloc_stub_failed");
        CloseHandle(process);
        return false;
    }

    const uintptr_t remote_stub_address = reinterpret_cast<uintptr_t>(remote_stub);
    const uintptr_t remote_telemetry_address = remote_stub_address + kStubSize;
    const uintptr_t worker_probe_base = remote_stub_address + kStubSize + sizeof(TbatsaveRuntimePatchTelemetryBlock);
    const uintptr_t remote_worker_t3_stub_address = worker_probe_base + kWorkerT3StubOffset;
    const uintptr_t remote_worker_general_stub_address = worker_probe_base + kWorkerGeneralStubOffset;
    const uintptr_t worker_telemetry_address = worker_probe_base + kWorkerTelemetryOffset;
    TbatsaveRuntimePatchTelemetryBlock telemetry{};
    telemetry.requested_bind_mode = static_cast<std::int64_t>(static_cast<int>(plan.tbatsave_bind_mode));
    telemetry.requested_bind_ref = static_cast<std::int64_t>(plan.tbatsave_bind_ref);
    TbatsaveWorkerRuntimeTelemetryBlock worker_telemetry{};

    if (!WriteAll(process, remote_telemetry_address, &telemetry, sizeof(telemetry))) {
        diagnostics.push_back("tbatsave_patch=write_telemetry_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    if (!WriteAll(process, worker_telemetry_address, &worker_telemetry, sizeof(worker_telemetry))) {
        diagnostics.push_back("tbatsave_patch=write_worker_telemetry_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const std::vector<std::uint8_t> stub = BuildHookStub(
        *tch_kernal_base,
        remote_telemetry_address,
        return_address,
        existing,
        plan
    );
    if (!WriteAll(process, reinterpret_cast<uintptr_t>(remote_stub), stub.data(), stub.size())) {
        diagnostics.push_back("tbatsave_patch=write_stub_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const std::vector<std::uint8_t> worker_t3_stub = BuildWorkerHookStub(
        worker_telemetry_address,
        worker_t3_return_address,
        worker_t3_existing,
        WorkerTelemetryKind::kWorkerT3
    );
    if (!WriteAll(process, remote_worker_t3_stub_address, worker_t3_stub.data(), worker_t3_stub.size())) {
        diagnostics.push_back("tbatsave_patch=write_worker_t3_stub_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const std::vector<std::uint8_t> worker_general_stub = BuildWorkerHookStub(
        worker_telemetry_address,
        worker_general_return_address,
        worker_general_existing,
        WorkerTelemetryKind::kWorkerGeneral
    );
    if (!WriteAll(
            process,
            remote_worker_general_stub_address,
            worker_general_stub.data(),
            worker_general_stub.size()
        )) {
        diagnostics.push_back("tbatsave_patch=write_worker_general_stub_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtectEx(
            process,
            reinterpret_cast<LPVOID>(hook_address),
            kHookPatchLength,
            PAGE_EXECUTE_READWRITE,
            &old_protect
        )) {
        diagnostics.push_back("tbatsave_patch=protect_hook_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const auto jump_patch = BuildAbsoluteJumpPatch(reinterpret_cast<uintptr_t>(remote_stub));
    const bool wrote_hook = WriteAll(process, hook_address, jump_patch.data(), jump_patch.size());
    FlushInstructionCache(process, reinterpret_cast<LPCVOID>(hook_address), jump_patch.size());

    DWORD worker_t3_old_protect = 0;
    if (!VirtualProtectEx(
            process,
            reinterpret_cast<LPVOID>(worker_t3_hook_address),
            worker_t3_existing.size(),
            PAGE_EXECUTE_READWRITE,
            &worker_t3_old_protect
        )) {
        diagnostics.push_back("tbatsave_patch=protect_worker_t3_hook_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const auto worker_t3_jump_patch =
        BuildAbsoluteJumpPatch<kExpectedWorkerT3Signature.size()>(remote_worker_t3_stub_address);
    const bool wrote_worker_t3_hook = WriteAll(
        process,
        worker_t3_hook_address,
        worker_t3_jump_patch.data(),
        worker_t3_jump_patch.size()
    );
    FlushInstructionCache(
        process,
        reinterpret_cast<LPCVOID>(worker_t3_hook_address),
        worker_t3_jump_patch.size()
    );
    DWORD ignored_worker_t3 = 0;
    VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(worker_t3_hook_address),
        worker_t3_existing.size(),
        worker_t3_old_protect,
        &ignored_worker_t3
    );

    DWORD worker_general_old_protect = 0;
    if (!VirtualProtectEx(
            process,
            reinterpret_cast<LPVOID>(worker_general_hook_address),
            worker_general_existing.size(),
            PAGE_EXECUTE_READWRITE,
            &worker_general_old_protect
        )) {
        diagnostics.push_back("tbatsave_patch=protect_worker_general_hook_failed");
        VirtualFreeEx(process, remote_stub, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    const auto worker_general_jump_patch =
        BuildAbsoluteJumpPatch<kExpectedWorkerGeneralSignature.size()>(
            remote_worker_general_stub_address
        );
    const bool wrote_worker_general_hook = WriteAll(
        process,
        worker_general_hook_address,
        worker_general_jump_patch.data(),
        worker_general_jump_patch.size()
    );
    FlushInstructionCache(
        process,
        reinterpret_cast<LPCVOID>(worker_general_hook_address),
        worker_general_jump_patch.size()
    );
    DWORD ignored_worker_general = 0;
    VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(worker_general_hook_address),
        worker_general_existing.size(),
        worker_general_old_protect,
        &ignored_worker_general
    );

    DWORD ignored = 0;
    VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(hook_address),
        kHookPatchLength,
        old_protect,
        &ignored
    );

    const bool wrote_flags = TryWriteNonUiFlags(process, *tch_kernal_base);

    if (wrote_hook && wrote_worker_t3_hook && wrote_worker_general_hook && wrote_flags) {
        g_tbatsave_patch_session.process_id = *acad_pid;
        g_tbatsave_patch_session.telemetry_remote = remote_telemetry_address;
        g_tbatsave_patch_session.stub_remote = remote_stub_address;
        g_tbatsave_patch_session.worker_telemetry_remote = worker_telemetry_address;
        g_tbatsave_patch_session.installed = true;
        g_tbatsave_patch_session.cached = telemetry;
        g_tbatsave_patch_session.has_cached = true;
        g_tbatsave_patch_session.worker_cached = worker_telemetry;
        g_tbatsave_patch_session.has_worker_cached = true;
        std::ostringstream stream;
        stream << "tbatsave_patch=installed hook=0x" << std::hex << hook_address
               << " stub=0x" << remote_stub_address
               << " telemetry=0x" << remote_telemetry_address;
        diagnostics.push_back(stream.str());
        std::ostringstream worker_stream;
        worker_stream << "tbatsave_patch=worker_hooks_installed worker_t3_hook=0x" << std::hex
                      << worker_t3_hook_address << " worker_general_hook=0x"
                      << worker_general_hook_address << " worker_telemetry=0x"
                      << worker_telemetry_address;
        diagnostics.push_back(worker_stream.str());
    } else if (!wrote_hook) {
        diagnostics.push_back("tbatsave_patch=write_hook_failed");
    } else if (!wrote_worker_t3_hook) {
        diagnostics.push_back("tbatsave_patch=write_worker_t3_hook_failed");
    } else if (!wrote_worker_general_hook) {
        diagnostics.push_back("tbatsave_patch=write_worker_general_hook_failed");
    } else {
        diagnostics.push_back("tbatsave_patch=flags_failed");
    }

    CloseHandle(process);
    return wrote_hook && wrote_worker_t3_hook && wrote_worker_general_hook && wrote_flags;
}

bool TryRunTbatsaveDirectWorker(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    diagnostics.push_back("tbatsave_direct_worker=started");

    if (plan.stage_source_path.empty() || plan.batch_output_dir.empty() ||
        plan.worker_status_path.empty()) {
        diagnostics.push_back("tbatsave_direct_worker=invalid_plan");
        return false;
    }

    const auto acad_pid = FindNewestProcessIdByName(L"acad.exe");
    if (!acad_pid.has_value()) {
        diagnostics.push_back("tbatsave_direct_worker=acad_not_found");
        return false;
    }

    const auto tch_kernal_base = WaitForTchKernalBase(*acad_pid, diagnostics);
    if (!tch_kernal_base.has_value()) {
        return false;
    }

    std::error_code error_code;
    std::filesystem::create_directories(plan.batch_output_dir, error_code);
    const std::filesystem::path batch_target = DeriveTbatsaveBatchTargetPath(plan);
    std::filesystem::remove(batch_target, error_code);

    const std::wstring source_path = ToWidePath(plan.stage_source_path);
    const std::wstring output_path = ToWidePath(batch_target);
    if (source_path.empty() || output_path.empty()) {
        diagnostics.push_back("tbatsave_direct_worker=empty_path");
        return false;
    }

    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
            PROCESS_QUERY_INFORMATION,
        FALSE,
        *acad_pid
    );
    if (process == nullptr) {
        diagnostics.push_back("tbatsave_direct_worker=open_process_failed");
        diagnostics.push_back("tbatsave_direct_worker_last_error=" + std::to_string(GetLastError()));
        return false;
    }

    constexpr SIZE_T kCodeOffset = 0x0000;
    constexpr SIZE_T kControlOffset = 0x0800;
    constexpr SIZE_T kSourceOffset = 0x0A00;
    constexpr SIZE_T kOutputOffset = 0x1200;
    constexpr SIZE_T kDbOffset = 0x2000;
    constexpr SIZE_T kDbSize = 0x4000;
    constexpr SIZE_T kAllocationSize = kDbOffset + kDbSize;

    LPVOID remote_memory = VirtualAllocEx(
        process,
        nullptr,
        kAllocationSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE
    );
    if (remote_memory == nullptr) {
        diagnostics.push_back("tbatsave_direct_worker=alloc_failed");
        diagnostics.push_back("tbatsave_direct_worker_last_error=" + std::to_string(GetLastError()));
        CloseHandle(process);
        return false;
    }

    const uintptr_t remote_base = reinterpret_cast<uintptr_t>(remote_memory);
    const uintptr_t code_address = remote_base + kCodeOffset;
    const uintptr_t control_address = remote_base + kControlOffset;
    const uintptr_t source_address = remote_base + kSourceOffset;
    const uintptr_t output_address = remote_base + kOutputOffset;
    const uintptr_t db_address = remote_base + kDbOffset;

    if (!TryWriteNonUiFlags(process, *tch_kernal_base)) {
        diagnostics.push_back("tbatsave_direct_worker=non_ui_flags_failed");
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    TbatsaveDirectWorkerControlBlock control{};
    control.db_remote = db_address;
    control.source_remote = source_address;
    control.output_remote = output_address;

    const std::vector<std::uint8_t> stub = BuildTbatsaveDirectWorkerStub(
        *tch_kernal_base,
        control_address,
        db_address,
        source_address,
        output_address
    );
    if (stub.size() > kControlOffset - kCodeOffset) {
        diagnostics.push_back("tbatsave_direct_worker=stub_too_large");
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const SIZE_T source_bytes = (source_path.size() + 1) * sizeof(wchar_t);
    const SIZE_T output_bytes = (output_path.size() + 1) * sizeof(wchar_t);
    if (source_bytes > kOutputOffset - kSourceOffset || output_bytes > kDbOffset - kOutputOffset) {
        diagnostics.push_back("tbatsave_direct_worker=path_too_long");
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    if (!WriteAll(process, control_address, &control, sizeof(control)) ||
        !WriteAll(process, source_address, source_path.c_str(), source_bytes) ||
        !WriteAll(process, output_address, output_path.c_str(), output_bytes) ||
        !WriteAll(process, code_address, stub.data(), stub.size())) {
        diagnostics.push_back("tbatsave_direct_worker=write_remote_failed");
        diagnostics.push_back("tbatsave_direct_worker_last_error=" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    FlushInstructionCache(process, reinterpret_cast<LPCVOID>(code_address), stub.size());

    diagnostics.push_back("tbatsave_direct_worker=readDwgFile");
    diagnostics.push_back("tbatsave_direct_worker=SaveAsTArch3");

    HANDLE thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(code_address),
        nullptr,
        0,
        nullptr
    );
    if (thread == nullptr) {
        diagnostics.push_back("tbatsave_direct_worker=create_thread_failed");
        diagnostics.push_back("tbatsave_direct_worker_last_error=" + std::to_string(GetLastError()));
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait_result =
        WaitForSingleObject(thread, TimeoutSecondsToMilliseconds(plan.timeout_seconds));
    if (wait_result == WAIT_TIMEOUT) {
        diagnostics.push_back("tbatsave_direct_worker=timeout");
        TerminateThread(thread, 0);
        CloseHandle(thread);
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        WriteDirectWorkerResultFile(plan, false, control, "timeout");
        return false;
    }

    DWORD thread_exit_code = 0;
    GetExitCodeThread(thread, &thread_exit_code);
    CloseHandle(thread);

    TbatsaveDirectWorkerControlBlock remote_control{};
    if (!ReadAll(process, control_address, &remote_control, sizeof(remote_control)) ||
        remote_control.magic != kTbatsaveDirectWorkerMagic) {
        diagnostics.push_back("tbatsave_direct_worker=read_control_failed");
        VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    remote_control.thread_exit_code = thread_exit_code;

    VirtualFreeEx(process, remote_memory, 0, MEM_RELEASE);
    CloseHandle(process);

    auto append_hex = [&diagnostics](const char* key, const std::uint64_t value) {
        std::ostringstream stream;
        stream << key << "0x" << std::hex << value;
        diagnostics.push_back(stream.str());
    };
    diagnostics.push_back("tbatsave_direct_worker_step=" + std::to_string(remote_control.step));
    diagnostics.push_back("tbatsave_direct_worker_status=" + std::to_string(remote_control.status));
    diagnostics.push_back(
        "tbatsave_direct_worker_read_status=" + std::to_string(remote_control.read_status)
    );
    diagnostics.push_back(
        "tbatsave_direct_worker_save_result=" + std::to_string(remote_control.save_result)
    );
    diagnostics.push_back(
        "tbatsave_direct_worker_selector=" + std::to_string(remote_control.selector)
    );
    diagnostics.push_back(
        "tbatsave_direct_worker_thread_exit_code=" + std::to_string(thread_exit_code)
    );
    append_hex("tbatsave_direct_worker_tch_kernal=", *tch_kernal_base);
    append_hex("tbatsave_direct_worker_stub=", code_address);
    append_hex("tbatsave_direct_worker_db=", db_address);

    const bool output_exists = std::filesystem::exists(batch_target, error_code);
    const bool success = remote_control.status == 1 && output_exists;
    if (!output_exists) {
        diagnostics.push_back("tbatsave_direct_worker=output_missing");
    }

    WriteDirectWorkerResultFile(
        plan,
        success,
        remote_control,
        success ? "ok" : "native-call-failed-or-output-missing"
    );
    diagnostics.push_back(
        std::string("tbatsave_direct_worker=") + (success ? "succeeded" : "failed")
    );
    return success;
}

bool EnsureTbatsaveRuntimePatchSession(std::vector<std::string>& diagnostics) {
    if (g_tbatsave_patch_session.installed) {
        return true;
    }
    return TryRefreshSessionFromInstalledHook(diagnostics);
}

std::vector<std::string> ReadTbatsaveRuntimePatchTelemetry() {
    if (!g_tbatsave_patch_session.installed) {
        return {};
    }

    auto append_worker_path_text_lines =
        [](std::vector<std::string>& lines, const DWORD process_id,
           const TbatsaveWorkerRuntimeTelemetryBlock& worker_telemetry) {
            if (process_id == 0) {
                return;
            }

            HANDLE process = OpenProcess(
                PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                FALSE,
                process_id
            );
            if (process == nullptr) {
                return;
            }

            const auto append_text = [&](const char* key, const uintptr_t remote) {
                if (const auto decoded = TryDecodeWorkerPathText(process, remote); decoded.has_value()) {
                    lines.push_back(std::string(key) + EscapeDiagnosticValue(*decoded));
                }
            };

            append_text(
                "tbatsave_telemetry_worker_t3_first_path_text=",
                worker_telemetry.worker_t3_first_path
            );
            append_text(
                "tbatsave_telemetry_worker_t3_last_path_text=",
                worker_telemetry.worker_t3_last_path
            );
            append_text(
                "tbatsave_telemetry_worker_general_first_path_text=",
                worker_telemetry.worker_general_first_path
            );
            append_text(
                "tbatsave_telemetry_worker_general_last_path_text=",
                worker_telemetry.worker_general_last_path
            );
            CloseHandle(process);
        };

    if (const auto live = TryReadLiveTelemetry(); live.has_value()) {
        g_tbatsave_patch_session.cached = *live;
        g_tbatsave_patch_session.has_cached = true;
        std::vector<std::string> lines = FormatTelemetryLines(*live, "live");
        if (const auto worker_live = TryReadLiveWorkerTelemetry(); worker_live.has_value()) {
            g_tbatsave_patch_session.worker_cached = *worker_live;
            g_tbatsave_patch_session.has_worker_cached = true;
            std::vector<std::string> worker_lines = FormatWorkerTelemetryLines(*worker_live, "live");
            lines.insert(lines.end(), worker_lines.begin(), worker_lines.end());
            append_worker_path_text_lines(
                lines,
                g_tbatsave_patch_session.process_id,
                *worker_live
            );
        } else if (g_tbatsave_patch_session.has_worker_cached) {
            std::vector<std::string> worker_lines = FormatWorkerTelemetryLines(
                g_tbatsave_patch_session.worker_cached,
                "cached"
            );
            lines.insert(lines.end(), worker_lines.begin(), worker_lines.end());
            append_worker_path_text_lines(
                lines,
                g_tbatsave_patch_session.process_id,
                g_tbatsave_patch_session.worker_cached
            );
        }
        return lines;
    }

    if (g_tbatsave_patch_session.has_cached) {
        std::vector<std::string> lines =
            FormatTelemetryLines(g_tbatsave_patch_session.cached, "cached");
        if (g_tbatsave_patch_session.has_worker_cached) {
            std::vector<std::string> worker_lines = FormatWorkerTelemetryLines(
                g_tbatsave_patch_session.worker_cached,
                "cached"
            );
            lines.insert(lines.end(), worker_lines.begin(), worker_lines.end());
            append_worker_path_text_lines(
                lines,
                g_tbatsave_patch_session.process_id,
                g_tbatsave_patch_session.worker_cached
            );
        }
        return lines;
    }

    return {"tbatsave_telemetry=unavailable"};
}

}  // namespace t3conv
