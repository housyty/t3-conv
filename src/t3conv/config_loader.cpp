#include "config_loader.h"

#include "../common/utils.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace t3conv {

namespace {

std::filesystem::path CurrentExecutableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (copied == buffer.size()) {
        buffer.resize(buffer.size() * 2, L'\0');
        copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (copied == 0) {
        return {};
    }
    buffer.resize(copied);
    return std::filesystem::path(buffer).parent_path().lexically_normal();
}


std::optional<std::filesystem::path> FindWorkspaceRootFrom(std::filesystem::path start) {
    if (start.empty()) {
        return std::nullopt;
    }

    std::error_code error_code;
    start = std::filesystem::absolute(start, error_code).lexically_normal();
    if (error_code) {
        start = start.lexically_normal();
    }

    std::filesystem::path cursor = start;
    while (!cursor.empty()) {
        if (PathExists(cursor / "t3conv.ini")) {
            return cursor;
        }
        if (PathExists(cursor / "runtime" / "tgstart_host" / "tbatsave_experimental_trigger.lsp")) {
            return cursor;
        }
        if (PathExists(
                cursor / "t3-conv" / "runtime" / "tgstart_host" /
                "tbatsave_experimental_trigger.lsp"
            )) {
            return cursor / "t3-conv";
        }

        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }

    return std::nullopt;
}


std::filesystem::path ResolveWorkspaceRoot() {
    const auto from_exe = FindWorkspaceRootFrom(CurrentExecutableDirectory());
    if (from_exe.has_value()) {
        return *from_exe;
    }

    const auto from_cwd = FindWorkspaceRootFrom(std::filesystem::current_path());
    if (from_cwd.has_value()) {
        return *from_cwd;
    }

    return {};
}


std::optional<std::string> ReadIniValue(
    const std::filesystem::path& config_path,
    const std::string& section,
    const std::string& key
) {
    std::ifstream stream(config_path);
    if (!stream.is_open()) {
        return std::nullopt;
    }

    bool in_section = false;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == ';' || line[first] == '#') {
            continue;
        }
        line = line.substr(first);
        if (line.front() == '[' && line.back() == ']') {
            in_section = line == ("[" + section + "]");
            continue;
        }
        if (!in_section) {
            continue;
        }
        const std::string prefix = key + "=";
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }

    return std::nullopt;
}


std::filesystem::path NormalizeConfiguredPath(
    const std::filesystem::path& workspace_root,
    const std::string& value
) {
    std::filesystem::path path(value);
    if (path.empty()) {
        return {};
    }
    if (!path.is_absolute()) {
        path = workspace_root / path;
    }
    return path.lexically_normal();
}


std::optional<std::filesystem::path> FindFirstExistingDirectory(
    const std::vector<std::filesystem::path>& candidates
) {
    for (const auto& candidate : candidates) {
        if (DirectoryExists(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }
    return std::nullopt;
}


std::filesystem::path AbsoluteLexicallyNormal(std::filesystem::path path) {
    std::error_code error_code;
    path = std::filesystem::absolute(path, error_code).lexically_normal();
    if (error_code) {
        path = path.lexically_normal();
    }
    return path;
}


std::vector<std::filesystem::path> AncestorDirectories(std::filesystem::path start) {
    std::vector<std::filesystem::path> directories;
    start = AbsoluteLexicallyNormal(start);

    std::filesystem::path cursor = start;
    while (!cursor.empty()) {
        directories.push_back(cursor);
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    return directories;
}


std::filesystem::path EnvPath(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string(value).empty()) {
        return {};
    }
    return std::filesystem::path(value).lexically_normal();
}


std::filesystem::path SystemDriveRoot() {
    std::filesystem::path system_drive = EnvPath("SystemDrive");
    if (system_drive.empty()) {
        return {};
    }

    const std::wstring drive = system_drive.wstring();
    if (drive.size() == 2 && drive[1] == L':') {
        return std::filesystem::path(drive + L"\\");
    }
    return system_drive;
}


std::vector<std::filesystem::path> FixedDriveRoots() {
    std::vector<std::filesystem::path> roots;
    const DWORD drive_mask = GetLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        const DWORD bit = 1UL << (letter - L'A');
        if ((drive_mask & bit) == 0) {
            continue;
        }

        wchar_t root[] = L"A:\\";
        root[0] = letter;
        if (GetDriveTypeW(root) == DRIVE_FIXED) {
            roots.emplace_back(root);
        }
    }
    return roots;
}


std::optional<std::filesystem::path> ConfiguredOrEnvPath(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& config_path,
    const std::string& section,
    const std::string& key,
    const char* env_name
) {
    const auto configured = ReadIniValue(config_path, section, key);
    if (configured.has_value() && !configured->empty()) {
        return NormalizeConfiguredPath(workspace_root, *configured);
    }

    const std::filesystem::path env_path = EnvPath(env_name);
    if (!env_path.empty()) {
        return NormalizeConfiguredPath(workspace_root, env_path.string());
    }
    return std::nullopt;
}


std::string BuildAutocadVersionDirectoryName(int year) {
    return std::string("AutoCAD ") + std::to_string(year);
}


std::optional<int> DetectAutocadYearFromRoot(const std::filesystem::path& autocad_root) {
    const std::string name = autocad_root.filename().string();
    constexpr std::string_view prefix = "AutoCAD ";
    const std::string lower_name = ToLowerAscii(name);
    const std::string lower_prefix = ToLowerAscii(std::string(prefix));
    if (lower_name.rfind(lower_prefix, 0) != 0) {
        return std::nullopt;
    }

    const std::string year_text = name.substr(prefix.size());
    if (year_text.empty()) {
        return std::nullopt;
    }
    try {
        size_t parsed = 0;
        const int year = std::stoi(year_text, &parsed);
        if (parsed == year_text.size()) {
            return year;
        }
    } catch (...) {
    }
    return std::nullopt;
}


std::string BuildTangentSysDirectoryNameForAutocadYear(const int year) {
    switch (year) {
        case 2020:
            return "sys23x64";
        case 2021:
            return "sys24x64";
        default:
            return {};
    }
}


std::filesystem::path ResolveTangentSysDir(
    const std::filesystem::path& tangent_root,
    const std::filesystem::path& autocad_root
) {
    const auto autocad_year = DetectAutocadYearFromRoot(autocad_root);
    if (autocad_year.has_value()) {
        const std::string sys_name =
            BuildTangentSysDirectoryNameForAutocadYear(*autocad_year);
        if (!sys_name.empty()) {
            return (tangent_root / sys_name).lexically_normal();
        }
    }
    return {};
}


void AddTangentNameCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& base
) {
    if (base.empty()) {
        return;
    }

    for (const std::string& name : {"TArchT20V9", "TArchT20V9.0", "T20V9"}) {
        candidates.push_back((base / name).lexically_normal());
    }
}


void AddTangentCommonBaseCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& base
) {
    if (base.empty()) {
        return;
    }

    AddTangentNameCandidates(candidates, base / "Tangent");
    AddTangentNameCandidates(candidates, base);
}


void AddDriveTangentRootCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::vector<std::filesystem::path>& drive_roots
) {
    for (const auto& drive_root : drive_roots) {
        AddTangentNameCandidates(candidates, drive_root / "Tangent");
    }
}


void AddDriveRootTangentCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::vector<std::filesystem::path>& drive_roots
) {
    for (const auto& drive_root : drive_roots) {
        AddTangentNameCandidates(candidates, drive_root);
    }
}


void AddWorkspaceTangentCandidates(
    std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& workspace_root
) {
    for (const auto& ancestor : AncestorDirectories(workspace_root)) {
        AddTangentNameCandidates(candidates, ancestor);
    }
}


void AddProgramFilesTangentCandidates(std::vector<std::filesystem::path>& candidates) {
    AddTangentCommonBaseCandidates(candidates, EnvPath("ProgramFiles"));
    AddTangentCommonBaseCandidates(candidates, EnvPath("ProgramFiles(x86)"));
}


std::vector<std::filesystem::path> CommonTangentRootCandidates(
    const std::filesystem::path& workspace_root
) {
    std::vector<std::filesystem::path> candidates;

    std::vector<std::filesystem::path> drive_roots;
    const std::filesystem::path system_drive_root = SystemDriveRoot();
    if (!system_drive_root.empty()) {
        drive_roots.push_back(system_drive_root);
    }
    for (const auto& drive_root : FixedDriveRoots()) {
        if (std::find(drive_roots.begin(), drive_roots.end(), drive_root) == drive_roots.end()) {
            drive_roots.push_back(drive_root);
        }
    }

    AddDriveTangentRootCandidates(candidates, drive_roots);
    AddWorkspaceTangentCandidates(candidates, workspace_root);
    AddDriveRootTangentCandidates(candidates, drive_roots);
    AddProgramFilesTangentCandidates(candidates);
    return candidates;
}


bool IsValidTangentRootCandidate(const std::filesystem::path& candidate) {
    return DirectoryExists(candidate) &&
           PathExists(candidate / "TGStart.exe") &&
           DirectoryExists(candidate / "SYS");
}


std::optional<std::filesystem::path> FindFirstValidTangentRoot(
    const std::vector<std::filesystem::path>& candidates
) {
    for (const auto& candidate : candidates) {
        if (IsValidTangentRootCandidate(candidate)) {
            return AbsoluteLexicallyNormal(candidate);
        }
    }
    return std::nullopt;
}


std::filesystem::path ResolveConfiguredOrDetectedTangentRoot(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& config_path
) {
    const auto configured = ConfiguredOrEnvPath(
        workspace_root,
        config_path,
        "paths",
        "tangent_root",
        "T3CONV_TANGENT_ROOT"
    );
    if (configured.has_value()) {
        return *configured;
    }

    const auto detected = FindFirstValidTangentRoot(CommonTangentRootCandidates(workspace_root));
    return detected.value_or(workspace_root.parent_path() / "TArchT20V9");
}


std::filesystem::path ResolveConfiguredOrDetectedAutocadRoot(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& config_path
) {
    const auto configured = ConfiguredOrEnvPath(
        workspace_root,
        config_path,
        "paths",
        "autocad_root",
        "T3CONV_AUTOCAD_ROOT"
    );
    if (configured.has_value()) {
        return *configured;
    }

    const std::filesystem::path program_files = EnvPath("ProgramFiles");
    const std::filesystem::path autodesk_root = program_files.empty()
                                                    ? std::filesystem::path()
                                                    : program_files / "Autodesk";
    std::vector<std::filesystem::path> autocad_candidates;
    for (int year : {2021, 2020}) {
        autocad_candidates.push_back(autodesk_root / BuildAutocadVersionDirectoryName(year));
    }
    const auto detected = FindFirstExistingDirectory(autocad_candidates);
    return detected.value_or(std::filesystem::path());
}


std::filesystem::path ResolveProjectFontDir(const std::filesystem::path& workspace_root) {
    return (workspace_root / "fonts").lexically_normal();
}


std::string ResolveConfiguredFontAlt(const std::filesystem::path& config_path) {
    const auto configured = ReadIniValue(config_path, "fonts", "fontalt");
    if (configured.has_value() && !configured->empty()) {
        return *configured;
    }
    const char* env_font_alt = std::getenv("T3CONV_FONTALT");
    if (env_font_alt != nullptr && std::string(env_font_alt).size() > 0) {
        return env_font_alt;
    }
    return "HZTXT.SHX";
}


ResolvedPaths BuildResolvedPaths(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& tangent_root,
    const std::filesystem::path& tangent_sys_dir,
    const std::filesystem::path& autocad_root,
    const std::filesystem::path& font_dir
) {
    ResolvedPaths paths;
    paths.workspace_root = workspace_root;
    paths.config_path = workspace_root / "t3conv.ini";
    paths.tangent_root = tangent_root;
    paths.tangent_sys_dir = tangent_sys_dir;
    paths.autocad_root = autocad_root;
    paths.tgstart_exe = tangent_root / "TGStart.exe";
    paths.tangent_mnl = tangent_root / "SYS" / "tangent.mnl";
    paths.runtime_root = workspace_root / "runtime" / "tgstart_host";
    paths.bridge_template_path = paths.runtime_root / "tangent_mnl_bridge.lsp";
    paths.host_template_path = paths.runtime_root / "tbatsave_experimental_trigger.lsp";
    paths.var_root = workspace_root / "var";
    paths.host_dir = paths.var_root / "host";
    paths.logs_dir = paths.var_root / "logs";
    paths.runtime_dir = paths.var_root / "runtime";
    paths.bridge_runtime_path = paths.runtime_dir / "tangent_mnl_bridge.runtime.lsp";
    paths.host_runtime_path = paths.runtime_dir / "tbatsave_experimental_trigger.runtime.lsp";
    paths.font_map_path = paths.runtime_dir / "fontmap.fmp";
    paths.font_dir = font_dir;
    paths.autocad_fonts_dir = autocad_root.empty() ? std::filesystem::path() : autocad_root / "Fonts";
    paths.host_ready_path = paths.host_dir / "host_ready.txt";
    paths.host_stop_path = paths.host_dir / "host_stop.txt";
    paths.host_bootstrap_path = paths.host_dir / "host_bootstrap.txt";
    paths.bootstrap_state_path = paths.var_root / "bootstrap.json";
    paths.worker_status_path = paths.host_dir / "worker_status.txt";
    paths.trigger_log_path = paths.logs_dir / "trigger.log";
    paths.conversion_count_path = paths.host_dir / "conversion_count.txt";
    return paths;
}


bool ValidateInstallRoots(
    const std::filesystem::path& tangent_root,
    const std::filesystem::path& tangent_sys_dir,
    const std::filesystem::path& autocad_root,
    std::string& error_message
) {
    if (!DirectoryExists(tangent_root)) {
        error_message = "Tianzheng T20V9 is not installed or not found: " + tangent_root.string();
        return false;
    }
    if (!PathExists(tangent_root / "TGStart.exe")) {
        error_message = "TGStart.exe not found under Tianzheng root: " + (tangent_root / "TGStart.exe").string();
        return false;
    }
    if (!DirectoryExists(tangent_root / "SYS")) {
        error_message = "Tianzheng SYS directory not found: " + (tangent_root / "SYS").string();
        return false;
    }
    if (!DirectoryExists(tangent_sys_dir)) {
        error_message =
            "Tianzheng AutoCAD-specific SYS directory not found: " +
            tangent_sys_dir.string();
        return false;
    }
    const std::filesystem::path candidate = tangent_sys_dir;
    if (!PathExists(candidate / "tch_kernal.arx")) {
        error_message =
            "tch_kernal.arx not found under Tianzheng AutoCAD-specific SYS directory: " +
            (candidate / "tch_kernal.arx").string();
        return false;
    }
    if (!DirectoryExists(autocad_root)) {
        error_message = "AutoCAD is not installed or not found. Set autocad_root in t3conv.ini or T3CONV_AUTOCAD_ROOT.";
        return false;
    }
    if (!DirectoryExists(autocad_root / "Fonts")) {
        error_message = "AutoCAD Fonts directory not found: " + (autocad_root / "Fonts").string();
        return false;
    }
    return true;
}

}  // namespace


bool ConfigLoader::Load(AppConfig& config, std::string& error_message) {
    const std::filesystem::path workspace_root = ResolveWorkspaceRoot();
    const std::filesystem::path config_path = workspace_root / "t3conv.ini";
    if (!PathExists(config_path)) {
        error_message = "missing config file: " + config_path.string();
        return false;
    }

    const std::filesystem::path tangent_root =
        ResolveConfiguredOrDetectedTangentRoot(workspace_root, config_path);
    if (tangent_root.empty() || !tangent_root.is_absolute()) {
        error_message = "config or detected tangent_root must be absolute";
        return false;
    }

    const std::filesystem::path autocad_root =
        ResolveConfiguredOrDetectedAutocadRoot(workspace_root, config_path);
    if (!DirectoryExists(autocad_root)) {
        error_message = "AutoCAD is not installed or not found. Set autocad_root in t3conv.ini or T3CONV_AUTOCAD_ROOT.";
        return false;
    }
    const std::filesystem::path tangent_sys_dir =
        ResolveTangentSysDir(tangent_root, autocad_root);
    if (tangent_sys_dir.empty()) {
        error_message =
            "AutoCAD version is not mapped to a Tianzheng SYS directory. Supported mappings: "
            "AutoCAD 2020 -> sys23x64, AutoCAD 2021 -> sys24x64.";
        return false;
    }
    if (!ValidateInstallRoots(tangent_root, tangent_sys_dir, autocad_root, error_message)) {
        return false;
    }

    const std::filesystem::path font_dir = ResolveProjectFontDir(workspace_root);
    const std::string font_alt = ResolveConfiguredFontAlt(config_path);

    config.config_path = config_path;
    config.tangent_root = tangent_root;
    config.tangent_sys_dir = tangent_sys_dir;
    config.autocad_root = autocad_root;
    config.font_dir = font_dir;
    config.autocad_fonts_dir = autocad_root.empty() ? std::filesystem::path() : autocad_root / "Fonts";
    config.font_alt = font_alt;
    config.resolved = BuildResolvedPaths(
        workspace_root,
        tangent_root,
        tangent_sys_dir,
        autocad_root,
        font_dir
    );
    return true;
}

}  // namespace t3conv
