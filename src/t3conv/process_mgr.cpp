#include "process_mgr.h"
#include "tbatsave_runtime_patch.h"

#include "../common/utils.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <comdef.h>
#include <oaidl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace t3conv {

namespace {

constexpr const char* kTbatsaveHostReadyMarker = "READY";
constexpr const char* kTbatsaveHostStopMarker = "STOP";
constexpr const char* kTbatsaveHostBootstrapMarker = "BOOTSTRAP";
constexpr const char* kHostStrategyReuseOrLaunch = "tgstart-reuse-or-launch";
constexpr const char* kReuseCheckAcadTchKernal = "acad+tch_kernal";
constexpr const char* kBridgeRuntimeFileName = "tangent_mnl_bridge.runtime.lsp";
constexpr const char* kRuntimeFontMapFileName = "fontmap.fmp";
constexpr const char* kDefaultTbatsaveFontAlt = "HZTXT.SHX";

int ToBindModeValue(const TbatsaveBindMode bind_mode) {
    return static_cast<int>(bind_mode);
}

std::string EscapeJson(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
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


std::string ToForwardSlashes(const std::filesystem::path& path) {
    std::string value = path.string();
    for (char& ch : value) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return value;
}


std::filesystem::path BuildTbatsaveWorkDir(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_dir
) {
    const std::filesystem::path root = output_dir.empty() ? source_path.parent_path() : output_dir;
    return root / "_t3conv_work";
}


std::filesystem::path BuildTbatsaveWorkDir(const CliOptions& options) {
    if (!options.paths.working_dir.empty()) {
        return options.paths.working_dir / "_t3conv_work";
    }
    return BuildTbatsaveWorkDir(options.paths.source_path, options.paths.output_dir);
}


void CleanupWorkingDirectory(const ProcessLaunchPlan& plan) {
    if (plan.working_dir.empty() || !PathExists(plan.working_dir)) {
        return;
    }
    std::error_code error_code;
    const std::filesystem::path normalized_working_dir =
        std::filesystem::weakly_canonical(plan.working_dir, error_code);
    if (error_code) {
        return;
    }
    error_code.clear();
    const std::filesystem::path normalized_workspace =
        std::filesystem::weakly_canonical(plan.workspace_root, error_code);
    if (error_code) {
        return;
    }
    const std::filesystem::path expected_working_dir =
        (normalized_workspace / "_t3conv_work").lexically_normal();
    if (normalized_working_dir.lexically_normal() != expected_working_dir) {
        return;
    }
    std::filesystem::remove_all(plan.working_dir, error_code);
}


bool EnsureVarDirectories(const ProcessLaunchPlan& plan) {
    return EnsureDirectory(plan.var_root) &&
           EnsureDirectory(plan.host_dir) &&
           EnsureDirectory(plan.logs_dir) &&
           EnsureDirectory(plan.runtime_dir);
}


bool WriteTextFile(const std::filesystem::path& path, const std::string& contents) {
    std::error_code error_code;
    std::filesystem::create_directories(path.parent_path(), error_code);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return stream.good();
}


bool CopyFileOverwrite(
    const std::filesystem::path& source_path,
    const std::filesystem::path& target_path
) {
    std::error_code error_code;
    std::filesystem::create_directories(target_path.parent_path(), error_code);
    error_code.clear();
    std::filesystem::copy_file(
        source_path,
        target_path,
        std::filesystem::copy_options::overwrite_existing,
        error_code
    );
    return !error_code;
}


std::optional<std::string> ReadFirstLine(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::string line;
    std::getline(stream, line);
    return line;
}


std::vector<std::string> ReadAllLines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream stream(path);
    if (!stream.is_open()) {
        return lines;
    }

    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}


bool RemoveFileIfExists(const std::filesystem::path& path) {
    std::error_code error_code;
    return !PathExists(path) || std::filesystem::remove(path, error_code);
}


std::string FontMapTarget(const std::filesystem::path& font_path) {
    if (PathExists(font_path)) {
        return ToForwardSlashes(font_path);
    }
    return font_path.filename().string();
}


std::vector<std::filesystem::path> CollectFontFiles(const std::filesystem::path& font_dir) {
    std::vector<std::filesystem::path> fonts;
    if (!DirectoryExists(font_dir)) {
        return fonts;
    }

    std::error_code error_code;
    for (const auto& entry : std::filesystem::directory_iterator(font_dir, error_code)) {
        if (error_code || !entry.is_regular_file(error_code)) {
            continue;
        }
        const std::string extension = ToLowerAscii(entry.path().extension().string());
        if (extension == ".shx" || extension == ".ttf") {
            fonts.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
        }
    }
    std::sort(fonts.begin(), fonts.end());
    return fonts;
}


bool FilesHaveSameSize(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    std::error_code left_error;
    std::error_code right_error;
    const auto left_size = std::filesystem::file_size(left, left_error);
    const auto right_size = std::filesystem::file_size(right, right_error);
    return !left_error && !right_error && left_size == right_size;
}


std::string FileSizeToken(const std::filesystem::path& path) {
    std::error_code error_code;
    const auto size = std::filesystem::file_size(path, error_code);
    if (error_code) {
        return "unknown";
    }
    return std::to_string(size);
}


std::string FileHashToken(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return "unreadable";
    }

    std::uint64_t hash = 1469598103934665603ULL;
    char buffer[4096];
    while (stream.good()) {
        stream.read(buffer, sizeof(buffer));
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[index]);
            hash *= 1099511628211ULL;
        }
    }

    std::ostringstream token;
    token << std::hex << hash;
    return token.str();
}


std::filesystem::path ManagedAutocadFontTarget(
    const ProcessLaunchPlan& plan,
    const std::filesystem::path& source_font
) {
    if (plan.autocad_fonts_dir.empty()) {
        return {};
    }

    const std::filesystem::path normal_target = plan.autocad_fonts_dir / source_font.filename();
    if (!PathExists(normal_target) || FilesHaveSameSize(source_font, normal_target)) {
        return normal_target;
    }

    const std::string managed_name =
        "t3conv_" +
        source_font.stem().string() +
        "_" +
        FileSizeToken(source_font) +
        "_" +
        FileHashToken(source_font) +
        source_font.extension().string();
    return plan.autocad_fonts_dir / managed_name;
}


std::filesystem::path ProjectFontMapTarget(
    const ProcessLaunchPlan& plan,
    const std::filesystem::path& source_font
) {
    const std::filesystem::path managed_target = ManagedAutocadFontTarget(plan, source_font);
    if (!managed_target.empty() && PathExists(managed_target)) {
        return managed_target;
    }

    if (!DirectoryExists(plan.autocad_fonts_dir)) {
        return source_font;
    }

    const std::filesystem::path normal_target = plan.autocad_fonts_dir / source_font.filename();
    if (!normal_target.empty() &&
        PathExists(normal_target) &&
        FilesHaveSameSize(source_font, normal_target)) {
        return normal_target;
    }

    return source_font;
}


void SyncProjectFontsToAutocadFonts(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    if (!DirectoryExists(plan.font_dir)) {
        diagnostics.push_back("font_sync=source_missing");
        return;
    }
    if (!DirectoryExists(plan.autocad_fonts_dir)) {
        diagnostics.push_back("font_sync=target_missing");
        return;
    }

    for (const auto& source_font : CollectFontFiles(plan.font_dir)) {
        const std::filesystem::path target_font = plan.autocad_fonts_dir / source_font.filename();
        if (PathExists(target_font)) {
            if (FilesHaveSameSize(source_font, target_font)) {
                diagnostics.push_back("font_sync=skipped_existing:" + source_font.filename().string());
            } else {
                diagnostics.push_back("font_sync=skipped_conflict:" + source_font.filename().string());
                const std::filesystem::path managed_target =
                    ManagedAutocadFontTarget(plan, source_font);
                if (PathExists(managed_target)) {
                    if (FilesHaveSameSize(source_font, managed_target)) {
                        diagnostics.push_back(
                            "font_sync=skipped_conflict_existing:" +
                            source_font.filename().string() +
                            "->" +
                            managed_target.filename().string()
                        );
                    } else {
                        diagnostics.push_back(
                            "font_sync=conflict_alias_collision:" +
                            source_font.filename().string() +
                            "->" +
                            managed_target.filename().string()
                        );
                    }
                    continue;
                }

                std::error_code error_code;
                std::filesystem::copy_file(source_font, managed_target, error_code);
                if (error_code) {
                    diagnostics.push_back(
                        "font_sync=conflict_copy_failed:" +
                        source_font.filename().string() +
                        "->" +
                        managed_target.filename().string()
                    );
                } else {
                    diagnostics.push_back(
                        "font_sync=conflict_copied_as:" +
                        source_font.filename().string() +
                        "->" +
                        managed_target.filename().string()
                    );
                }
            }
            continue;
        }

        std::error_code error_code;
        std::filesystem::copy_file(source_font, target_font, error_code);
        if (error_code) {
            diagnostics.push_back("font_sync=copy_failed:" + source_font.filename().string());
        } else {
            diagnostics.push_back("font_sync=copied:" + source_font.filename().string());
        }
    }
}


std::string BuildFontSearchPath(const ProcessLaunchPlan& plan) {
    std::vector<std::filesystem::path> paths;
    if (DirectoryExists(plan.autocad_fonts_dir)) {
        paths.push_back(plan.autocad_fonts_dir);
    }

    std::ostringstream stream;
    bool first = true;
    for (const auto& path : paths) {
        if (!first) {
            stream << ";";
        }
        stream << ToForwardSlashes(path);
        first = false;
    }
    return stream.str();
}


std::string BuildTbatsaveFontMapContents(const ProcessLaunchPlan& plan) {
    const std::filesystem::path sys_dir = plan.tarch_root / "SYS";
    const std::string hztxt = FontMapTarget(sys_dir / "HZTXT.SHX");
    const std::string hzfs = FontMapTarget(sys_dir / "HZFS.SHX");
    const std::string hzht = FontMapTarget(sys_dir / "HZHT.SHX");
    const std::string hzkt = FontMapTarget(sys_dir / "HZKT.SHX");
    const std::string hzst = FontMapTarget(sys_dir / "HZST.SHX");
    const std::string gbcbig = FontMapTarget(sys_dir / "gbcbig0.shx");

    std::ostringstream stream;
    stream << "; t3-conv runtime font map for unattended TBatSave\n";
    stream << "; Generated from t3conv.ini tangent_root; safe to overwrite.\n";

    for (const auto& font_file : CollectFontFiles(plan.font_dir)) {
        const std::filesystem::path map_target = ProjectFontMapTarget(plan, font_file);
        stream << font_file.filename().string() << ";" << ToForwardSlashes(map_target) << "\n";
    }

    stream << "hztxt.shx;" << hztxt << "\n";
    stream << "hzfs.shx;" << hzfs << "\n";
    stream << "hzht.shx;" << hzht << "\n";
    stream << "hzkt.shx;" << hzkt << "\n";
    stream << "hzst.shx;" << hzst << "\n";
    stream << "txt.shx;" << hztxt << "\n";
    stream << "tssdchn.shx;" << hztxt << "\n";
    stream << "china.shx;" << hztxt << "\n";
    stream << "gbcbig.shx;" << gbcbig << "\n";
    stream << "gbcbig0.shx;" << gbcbig << "\n";
    stream << "complex.shx;" << hztxt << "\n";
    stream << "simplex.shx;" << hztxt << "\n";
    return stream.str();
}


bool FileContainsMarker(
    const std::filesystem::path& path,
    const std::string& expected
) {
    const auto line = ReadFirstLine(path);
    return line.has_value() && *line == expected;
}


bool IsReusableAcadHostRunning();
bool IsReusableAcadHostHealthy();


bool IsTbatsaveHostReady(const ProcessLaunchPlan& plan) {
    return FileContainsMarker(plan.host_ready_path, kTbatsaveHostReadyMarker) &&
           IsReusableAcadHostHealthy();
}


std::optional<std::string> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}


std::string ReplaceAll(std::string text, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return text;
    }

    std::string::size_type pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return text;
}


bool IsManagedTbatsaveBridgeLoadLine(const std::string& line) {
    if (line.rfind("(load \"", 0) != 0) {
        return false;
    }
    if (line.find("\" nil)") == std::string::npos) {
        return false;
    }
    return line.find("tangent_mnl_bridge.runtime.lsp") != std::string::npos ||
           line.find("tangent_mnl_bridge.lsp") != std::string::npos;
}


bool RemoveKnownTbatsaveBridgeLine(
    const std::string& line,
    const ProcessLaunchPlan& plan,
    const std::string& bridge_load_line
) {
    if (line == bridge_load_line) {
        return true;
    }
    if (IsManagedTbatsaveBridgeLoadLine(line)) {
        return true;
    }

    const std::string repo_template_line =
        "(load \"" + ToForwardSlashes(plan.host_bridge_template_path) + "\" nil)";
    if (line == repo_template_line) {
        return true;
    }

    const std::filesystem::path old_repo_template_path =
        plan.workspace_root / "runtime" / "tgstart_host" / "tangent_mnl_bridge.lsp";
    const std::string old_runtime_line =
        "(load \"" + ToForwardSlashes(old_repo_template_path) + "\" nil)";
    if (line == old_runtime_line) {
        return true;
    }

    const std::string legacy_runtime_line =
        "(load \"" + ToForwardSlashes(plan.host_runtime_script_path) + "\" nil)";
    return line == legacy_runtime_line;
}


bool CollapseManagedPrincLines(
    const std::string& line,
    const bool already_wrote_princ
) {
    return line == "(princ)" && already_wrote_princ;
}


std::string NormalizeTbatsaveHostStartupHook(
    const std::string& current,
    const ProcessLaunchPlan& plan,
    const std::string& bridge_load_line
) {
    std::istringstream input(current);
    std::ostringstream output;
    std::string line;
    bool wrote_initial_princ = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (RemoveKnownTbatsaveBridgeLine(line, plan, bridge_load_line)) {
            continue;
        }
        if (CollapseManagedPrincLines(line, wrote_initial_princ)) {
            continue;
        }
        if (line == "(princ)") {
            wrote_initial_princ = true;
        }
        output << line << "\n";
    }

    std::string normalized = output.str();
    if (normalized.empty()) {
        normalized = ";; tangent.mnl -- TBatSave host bridge\n(princ)\n";
    }
    if (normalized.back() != '\n') {
        normalized.push_back('\n');
    }
    normalized += bridge_load_line;
    normalized += "\n(princ)\n";
    return normalized;
}


std::string RenderLispTemplate(const std::string& source, const ProcessLaunchPlan& plan) {
    std::string rendered = source;
    rendered = ReplaceAll(rendered, "__TBX_HOST_READY__", ToForwardSlashes(plan.host_ready_path));
    rendered = ReplaceAll(rendered, "__TBX_HOST_STOP__", ToForwardSlashes(plan.host_stop_path));
    rendered = ReplaceAll(
        rendered,
        "__TBX_HOST_BOOTSTRAP__",
        ToForwardSlashes(plan.host_bootstrap_path)
    );
    rendered = ReplaceAll(rendered, "__TBX_TRIGGER_LOG__", ToForwardSlashes(plan.trigger_log_path));
    rendered = ReplaceAll(rendered, "__TBX_FONT_MAP__", ToForwardSlashes(plan.font_map_path));
    rendered = ReplaceAll(rendered, "__TBX_FONT_ALT__", plan.font_alt.empty() ? kDefaultTbatsaveFontAlt : plan.font_alt);
    rendered = ReplaceAll(rendered, "__TBX_FONT_SEARCH_PATH__", BuildFontSearchPath(plan));
    rendered = ReplaceAll(
        rendered,
        "__TBX_HOST_RUNTIME_SCRIPT__",
        ToForwardSlashes(plan.host_runtime_script_path)
    );
    rendered = ReplaceAll(rendered, "__TBX_HOST_BRIDGE_SCRIPT__", ToForwardSlashes(plan.host_bridge_script_path));
    return rendered;
}


bool RenderTbatsaveHostRuntimeScripts(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    const auto bridge_template = ReadWholeFile(plan.host_bridge_template_path);
    if (!bridge_template.has_value()) {
        diagnostics.push_back("host_hook=bridge_template_missing");
        return false;
    }

    const auto host_template = ReadWholeFile(plan.host_runtime_template_path);
    if (!host_template.has_value()) {
        diagnostics.push_back("host_hook=runtime_template_missing");
        return false;
    }

    if (!WriteTextFile(
            plan.host_bridge_script_path,
            RenderLispTemplate(*bridge_template, plan)
        )) {
        diagnostics.push_back("host_hook=bridge_runtime_write_failed");
        return false;
    }

    if (!WriteTextFile(plan.font_map_path, BuildTbatsaveFontMapContents(plan))) {
        diagnostics.push_back("host_hook=font_map_write_failed");
        return false;
    }

    if (!WriteTextFile(
            plan.host_runtime_script_path,
            RenderLispTemplate(*host_template, plan)
        )) {
        diagnostics.push_back("host_hook=runtime_script_write_failed");
        return false;
    }

    std::ostringstream bootstrap;
    bootstrap << "{\n";
    bootstrap << "  \"config\": \"" << EscapeJson(plan.config_path.string()) << "\",\n";
    bootstrap << "  \"tangent_root\": \"" << EscapeJson(plan.tarch_root.string()) << "\",\n";
    bootstrap << "  \"autocad_root\": \"" << EscapeJson(plan.autocad_root.string()) << "\",\n";
    bootstrap << "  \"runtime_fontmap\": \"" << EscapeJson(plan.font_map_path.string()) << "\",\n";
    bootstrap << "  \"host_runtime_script\": \"" << EscapeJson(plan.host_runtime_script_path.string()) << "\",\n";
    bootstrap << "  \"host_bridge_script\": \"" << EscapeJson(plan.host_bridge_script_path.string()) << "\",\n";
    bootstrap << "  \"host_mnl\": \"" << EscapeJson(plan.host_mnl_path.string()) << "\"\n";
    bootstrap << "}\n";
    if (!WriteTextFile(plan.bootstrap_state_path, bootstrap.str())) {
        diagnostics.push_back("host_hook=bootstrap_state_write_failed");
        return false;
    }

    diagnostics.push_back("host_hook=runtime_scripts_rendered");
    diagnostics.push_back("host_hook_bootstrap_state=written");
    diagnostics.push_back(std::string("host_hook_runtime_fontmap=") + kRuntimeFontMapFileName);
    return true;
}


bool EnsureTbatsaveHostStartupHook(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    if (!EnsureVarDirectories(plan)) {
        diagnostics.push_back("host_hook=var_dir_create_failed");
        return false;
    }
    if (!PathExists(plan.host_runtime_template_path)) {
        diagnostics.push_back("host_hook=runtime_template_path_missing");
        return false;
    }
    if (!PathExists(plan.host_bridge_template_path)) {
        diagnostics.push_back("host_hook=bridge_template_path_missing");
        return false;
    }
    SyncProjectFontsToAutocadFonts(plan, diagnostics);
    if (!RenderTbatsaveHostRuntimeScripts(plan, diagnostics)) {
        return false;
    }
    diagnostics.push_back(std::string("host_hook_runtime_bridge=") + kBridgeRuntimeFileName);
    if (!PathExists(plan.host_mnl_path)) {
        diagnostics.push_back("host_hook=mnl_missing");
        return false;
    }

    const std::string bridge_load_line =
        "(load \"" + ToForwardSlashes(plan.host_bridge_script_path) + "\" nil)";
    const auto current = ReadWholeFile(plan.host_mnl_path);
    const std::string base_mnl =
        current.value_or(";; tangent.mnl -- TBatSave host bridge\n(princ)\n");
    const std::string normalized =
        NormalizeTbatsaveHostStartupHook(base_mnl, plan, bridge_load_line);
    if (current.has_value() && *current == normalized) {
        diagnostics.push_back("host_hook=already_normalized");
        return true;
    }

    if (!WriteTextFile(plan.host_mnl_path, normalized)) {
        diagnostics.push_back("host_hook=write_failed");
        return false;
    }

    diagnostics.push_back("host_hook=normalized_mnl");
    return true;
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


std::vector<DWORD> EnumTianzhengAcadProcessIds() {
    std::vector<DWORD> process_ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return process_ids;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return process_ids;
    }

    do {
        if (_wcsicmp(entry.szExeFile, L"acad.exe") != 0) {
            continue;
        }
        if (FindModuleBaseAddress(entry.th32ProcessID, L"tch_kernal.arx").has_value()) {
            process_ids.push_back(entry.th32ProcessID);
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return process_ids;
}


struct HungWindowProbe {
    DWORD process_id = 0;
    bool found_window = false;
    bool found_hung_window = false;
};


BOOL CALLBACK ProbeHungWindowForProcess(HWND window, LPARAM param) {
    auto* probe = reinterpret_cast<HungWindowProbe*>(param);
    DWORD window_process_id = 0;
    GetWindowThreadProcessId(window, &window_process_id);
    if (window_process_id != probe->process_id) {
        return TRUE;
    }

    if (!IsWindowVisible(window)) {
        return TRUE;
    }

    probe->found_window = true;
    if (IsHungAppWindow(window)) {
        probe->found_hung_window = true;
        return FALSE;
    }
    return TRUE;
}


bool IsTianzhengAcadProcessHung(DWORD process_id) {
    HungWindowProbe probe;
    probe.process_id = process_id;
    EnumWindows(ProbeHungWindowForProcess, reinterpret_cast<LPARAM>(&probe));
    return probe.found_window && probe.found_hung_window;
}


bool IsReusableAcadHostHealthy() {
    const std::vector<DWORD> process_ids = EnumTianzhengAcadProcessIds();
    if (process_ids.empty()) {
        return false;
    }
    for (const DWORD process_id : process_ids) {
        if (!IsTianzhengAcadProcessHung(process_id)) {
            return true;
        }
    }
    return false;
}


bool IsReusableAcadHostRunning() {
    return !EnumTianzhengAcadProcessIds().empty();
}


size_t KillTianzhengAcadHosts() {
    size_t killed_count = 0;
    const std::vector<DWORD> process_ids = EnumTianzhengAcadProcessIds();
    for (const DWORD process_id : process_ids) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id);
        if (process == nullptr) {
            continue;
        }

        if (TerminateProcess(process, static_cast<UINT>(ErrorCode::kLoadTimeout))) {
            WaitForSingleObject(process, 5000);
            ++killed_count;
        }
        CloseHandle(process);
    }
    return killed_count;
}


bool RecoverHungTianzhengAcadHosts(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
) {
    const std::vector<DWORD> process_ids = EnumTianzhengAcadProcessIds();
    if (process_ids.empty()) {
        RemoveFileIfExists(plan.host_ready_path);
        diagnostics.push_back("host_recovery=no_tianzheng_acad_running");
        return false;
    }

    bool has_hung_process = false;
    for (const DWORD process_id : process_ids) {
        if (IsTianzhengAcadProcessHung(process_id)) {
            has_hung_process = true;
            break;
        }
    }
    if (!has_hung_process) {
        return false;
    }

    const size_t killed_count = KillTianzhengAcadHosts();
    RemoveFileIfExists(plan.host_ready_path);
    RemoveFileIfExists(plan.host_stop_path);
    RemoveFileIfExists(plan.worker_status_path);
    diagnostics.push_back("host_recovery=hung_tianzheng_acad_killed");
    diagnostics.push_back("host_recovery_killed_count=" + std::to_string(killed_count));
    return killed_count > 0;
}


std::wstring ToWide(const std::filesystem::path& path) {
    return path.wstring();
}


std::wstring ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), size);
    return wide;
}


std::filesystem::path DeriveBatchTargetPath(
    const std::filesystem::path& batch_output_dir,
    const std::filesystem::path& source_path
) {
    return batch_output_dir / (source_path.stem().string() + "_t3" + source_path.extension().string());
}


std::vector<std::string> CollectCommonDiagnostics(const ProcessLaunchPlan& plan) {
    std::vector<std::string> diagnostics;
    if (!plan.config_path.empty()) {
        diagnostics.push_back("config=" + plan.config_path.string());
    }
    if (!plan.workspace_root.empty()) {
        diagnostics.push_back("workspace=" + plan.workspace_root.string());
    }
    if (!plan.var_root.empty()) {
        diagnostics.push_back("var_root=" + plan.var_root.string());
    }
    diagnostics.push_back("script=" + plan.script_path.string());
    if (!plan.worker_status_path.empty()) {
        diagnostics.push_back("worker_status=" + plan.worker_status_path.string());
    }
    if (!plan.host_ready_path.empty()) {
        diagnostics.push_back("host_ready=" + plan.host_ready_path.string());
    }
    if (!plan.host_bootstrap_path.empty()) {
        diagnostics.push_back("host_bootstrap=" + plan.host_bootstrap_path.string());
    }
    if (!plan.bootstrap_state_path.empty()) {
        diagnostics.push_back("bootstrap_state=" + plan.bootstrap_state_path.string());
    }
    if (!plan.host_stop_path.empty()) {
        diagnostics.push_back("host_stop=" + plan.host_stop_path.string());
    }
    if (!plan.host_runtime_script_path.empty()) {
        diagnostics.push_back("host_runtime_script=" + plan.host_runtime_script_path.string());
    }
    if (!plan.host_bridge_script_path.empty()) {
        diagnostics.push_back("host_bridge_script=" + plan.host_bridge_script_path.string());
    }
    if (!plan.font_map_path.empty()) {
        diagnostics.push_back("font_map=" + plan.font_map_path.string());
    }
    if (!plan.font_dir.empty()) {
        diagnostics.push_back("font_dir=" + plan.font_dir.string());
    }
    if (!plan.autocad_fonts_dir.empty()) {
        diagnostics.push_back("autocad_fonts_dir=" + plan.autocad_fonts_dir.string());
    }
    if (!plan.font_alt.empty()) {
        diagnostics.push_back("font_alt=" + plan.font_alt);
    }
    diagnostics.push_back("font_search_path=" + BuildFontSearchPath(plan));
    if (!plan.host_runtime_template_path.empty()) {
        diagnostics.push_back("host_runtime_template=" + plan.host_runtime_template_path.string());
    }
    if (!plan.host_bridge_template_path.empty()) {
        diagnostics.push_back("host_bridge_template=" + plan.host_bridge_template_path.string());
    }
    if (!plan.host_mnl_path.empty()) {
        diagnostics.push_back("host_mnl=" + plan.host_mnl_path.string());
    }
    if (!plan.trigger_log_path.empty()) {
        diagnostics.push_back("trigger_log=" + plan.trigger_log_path.string());
    }
    if (!plan.stage_source_path.empty()) {
        diagnostics.push_back("stage_source=" + plan.stage_source_path.string());
    }
    if (!plan.batch_output_dir.empty()) {
        diagnostics.push_back("batch_output=" + plan.batch_output_dir.string());
    }
    diagnostics.push_back("tbatsave_bind_mode=" + std::to_string(ToBindModeValue(plan.tbatsave_bind_mode)));
    diagnostics.push_back("tbatsave_bind_ref=" + std::to_string(plan.tbatsave_bind_ref));
    diagnostics.push_back("host_control=" + HostControlModeToString(plan.host_control_mode));
    diagnostics.push_back(std::string("host_strategy=") + kHostStrategyReuseOrLaunch);
    diagnostics.push_back(std::string("reuse_check=") + kReuseCheckAcadTchKernal);
    diagnostics.push_back("command=" + JoinCommandLine(plan.executable, plan.arguments));
    return diagnostics;
}


ConversionResult FailWith(
    ErrorCode error_code,
    const std::string& message,
    const ProcessLaunchPlan& plan,
    std::vector<std::string> diagnostics = {}
) {
    std::vector<std::string> common = CollectCommonDiagnostics(plan);
    diagnostics.insert(diagnostics.end(), common.begin(), common.end());
    return MakeFailureResult(error_code, message, std::move(diagnostics));
}


std::vector<std::string> CollectSuccessDiagnostics(const ProcessLaunchPlan& plan) {
    std::vector<std::string> diagnostics = CollectCommonDiagnostics(plan);
    diagnostics.push_back("target=" + plan.target_path.string());
    diagnostics.push_back("log=" + plan.log_path.string());
    return diagnostics;
}


std::vector<std::string> PrefixLinesForDiagnostics(
    const std::vector<std::string>& lines,
    const std::string& prefix
) {
    std::vector<std::string> diagnostics;
    diagnostics.reserve(lines.size());
    for (const std::string& line : lines) {
        diagnostics.push_back(prefix + line);
    }
    return diagnostics;
}


ConversionResult BuildTbatsaveDirectWorkerSuccessResult(
    const ProcessLaunchPlan& plan,
    const std::chrono::steady_clock::time_point& start_time,
    std::vector<std::string> diagnostics
) {
    diagnostics.push_back("host_action=tbatsave_direct_worker_succeeded");
    std::vector<std::string> success_diagnostics = CollectSuccessDiagnostics(plan);
    success_diagnostics.insert(
        success_diagnostics.end(),
        diagnostics.begin(),
        diagnostics.end()
    );

    ConversionResult result = MakeSuccessResult(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count()
    );
    result.diagnostics = std::move(success_diagnostics);
    return result;
}


ConversionResult WaitForTbatsaveDirectWorkerRelocation(
    const ProcessLaunchPlan& plan,
    const std::chrono::steady_clock::time_point& start_time,
    std::vector<std::string> diagnostics
) {
    const std::filesystem::path batch_target =
        DeriveBatchTargetPath(plan.batch_output_dir, plan.stage_source_path);
    if (!PathExists(batch_target)) {
        diagnostics.push_back("tbatsave_direct_worker=relocation_source_missing");
        return FailWith(
            ErrorCode::kSaveFailed,
            "TBatSave direct worker reported success but batch target is missing",
            plan,
            std::move(diagnostics)
        );
    }

    std::error_code error_code;
    std::filesystem::rename(batch_target, plan.target_path, error_code);
    if (error_code) {
        error_code.clear();
        std::filesystem::copy_file(
            batch_target,
            plan.target_path,
            std::filesystem::copy_options::overwrite_existing,
            error_code
        );
    }
    if (error_code || !PathExists(plan.target_path)) {
        diagnostics.push_back("tbatsave_direct_worker=relocation_failed");
        return FailWith(
            ErrorCode::kSaveFailed,
            "TBatSave direct worker target relocation failed",
            plan,
            std::move(diagnostics)
        );
    }

    return BuildTbatsaveDirectWorkerSuccessResult(plan, start_time, std::move(diagnostics));
}


ConversionResult BuildDirectWorkerNoUiFallbackFailureResult(
    const ProcessLaunchPlan& plan,
    const std::chrono::steady_clock::time_point& start_time,
    std::vector<std::string> diagnostics
) {
    diagnostics.push_back("host_action=tbatsave_direct_worker_failed_no_ui_fallback");
    diagnostics.push_back("ui_fallback=disabled");
    std::vector<std::string> common = CollectCommonDiagnostics(plan);
    diagnostics.insert(diagnostics.end(), common.begin(), common.end());
    ConversionResult result = MakeFailureResult(
        ErrorCode::kFunctionCallFailed,
        "TBatSave direct worker failed; UI command fallback is disabled",
        std::move(diagnostics)
    );
    result.elapsed_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    return result;
}


bool LaunchProcess(const ProcessLaunchPlan& plan, PROCESS_INFORMATION& process_info) {
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);

    std::string command_line = JoinCommandLine(plan.executable, plan.arguments);
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    return CreateProcessA(
               nullptr,
               mutable_command_line.data(),
               nullptr,
               nullptr,
               FALSE,
               CREATE_NO_WINDOW,
               nullptr,
               plan.working_dir.string().c_str(),
               &startup_info,
               &process_info
           ) == TRUE;
}


}  // namespace


ProcessLaunchPlan ProcessManager::BuildLaunchPlan(const CliOptions& options, const AppConfig& config) {
    ProcessLaunchPlan plan;
    const ResolvedPaths& resolved = config.resolved;
    plan.host_control_mode = options.host_control_mode;
    plan.tbatsave_bind_mode = options.tbatsave_bind_mode;
    plan.tbatsave_bind_ref = options.tbatsave_bind_ref;
    plan.timeout_seconds = options.timeout_seconds;
    plan.config_path = config.config_path;
    plan.workspace_root = resolved.workspace_root;
    plan.var_root = resolved.var_root;
    plan.host_dir = resolved.host_dir;
    plan.logs_dir = resolved.logs_dir;
    plan.runtime_dir = resolved.runtime_dir;
    plan.tarch_root = resolved.tangent_root;
    plan.autocad_root = resolved.autocad_root;
    plan.host_ready_path = resolved.host_ready_path;
    plan.host_stop_path = resolved.host_stop_path;
    plan.host_bootstrap_path = resolved.host_bootstrap_path;
    plan.bootstrap_state_path = resolved.bootstrap_state_path;
    plan.host_runtime_script_path = resolved.host_runtime_path;
    plan.host_bridge_script_path = resolved.bridge_runtime_path;
    plan.font_map_path = resolved.font_map_path;
    plan.font_dir = resolved.font_dir;
    plan.autocad_fonts_dir = resolved.autocad_fonts_dir;
    plan.host_runtime_template_path = resolved.host_template_path;
    plan.host_bridge_template_path = resolved.bridge_template_path;
    plan.host_mnl_path = resolved.tangent_mnl;
    plan.trigger_log_path = resolved.trigger_log_path;
    plan.working_dir = options.paths.source_path.empty()
                           ? resolved.workspace_root
                           : BuildTbatsaveWorkDir(options);
    const std::string script_stem = options.paths.source_path.empty()
                                        ? std::string("tbatsave_host")
                                        : options.paths.source_path.stem().string() + "_t3";
    plan.script_path = plan.working_dir / (script_stem + ".scr");
    plan.worker_status_path = resolved.worker_status_path;
    if (!options.paths.source_path.empty()) {
        plan.stage_source_path = plan.working_dir / options.paths.source_path.filename();
    }
    plan.batch_output_dir = plan.working_dir / "batch_output";
    plan.target_path = options.paths.target_path;
    plan.log_path = options.paths.log_path;
    plan.font_alt = config.font_alt.empty() ? kDefaultTbatsaveFontAlt : config.font_alt;

    if (!options.paths.source_path.empty() && plan.target_path.empty()) {
        plan.target_path = BuildDefaultTargetPath(options.paths.source_path, options.paths.output_dir);
    }

    if (!options.paths.source_path.empty() && plan.log_path.empty()) {
        plan.log_path = BuildDefaultLogPath(options.paths.source_path, options.paths.output_dir);
    }

    plan.executable = resolved.tgstart_exe;
    plan.arguments = {};
    if (options.host_control_mode != HostControlMode::kNone) {
        plan.script_path = plan.host_bootstrap_path;
        plan.script_contents = kTbatsaveHostBootstrapMarker;
    }

    return plan;
}


ConversionResult ProcessManager::Execute(const CliOptions& options, const ProcessLaunchPlan& plan) {
    std::vector<std::string> hook_diagnostics;
    if (!EnsureTbatsaveHostStartupHook(plan, hook_diagnostics)) {
        return FailWith(
            ErrorCode::kSaveFailed,
            "failed to install TBatSave host startup hook",
            plan,
            std::move(hook_diagnostics)
        );
    }

    auto append_diagnostics = [](ConversionResult& result, const std::vector<std::string>& diagnostics) {
        result.diagnostics.insert(result.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    };

    if (options.host_control_mode == HostControlMode::kStatus) {
        ConversionResult result = MakeSuccessResult(0.0);
        result.diagnostics = CollectCommonDiagnostics(plan);
        append_diagnostics(result, hook_diagnostics);
        result.diagnostics.push_back(
            std::string("host_ready_state=") + (IsTbatsaveHostReady(plan) ? "ready" : "not_ready")
        );
        return result;
    }

    if (options.host_control_mode == HostControlMode::kStop) {
        RemoveFileIfExists(plan.host_ready_path);
        if (!WriteTextFile(plan.host_stop_path, kTbatsaveHostStopMarker)) {
            hook_diagnostics.push_back("host_stop_requested=false");
            return FailWith(
                ErrorCode::kSaveFailed,
                "failed to write TBatSave host stop marker",
                plan,
                std::move(hook_diagnostics)
            );
        }
        ConversionResult result = MakeSuccessResult(0.0);
        result.diagnostics = CollectCommonDiagnostics(plan);
        append_diagnostics(result, hook_diagnostics);
        result.diagnostics.push_back("host_stop_requested=true");
        return result;
    }

    if (!PathExists(plan.executable)) {
        return FailWith(
            ErrorCode::kFileNotFound,
            "TGStart.exe not found",
            plan,
            std::move(hook_diagnostics)
        );
    }

    if (!EnsureDirectory(plan.working_dir)) {
        return FailWith(
            ErrorCode::kSaveFailed,
            "failed to create TBatSave working directory",
            plan,
            std::move(hook_diagnostics)
        );
    }
    std::vector<std::string> pre_launch_diagnostics = std::move(hook_diagnostics);
    RecoverHungTianzhengAcadHosts(plan, pre_launch_diagnostics);

    if (options.host_control_mode == HostControlMode::kStart) {
        RemoveFileIfExists(plan.host_stop_path);
        if (IsTbatsaveHostReady(plan)) {
            ConversionResult result = MakeSuccessResult(0.0);
            result.diagnostics = CollectCommonDiagnostics(plan);
            append_diagnostics(result, pre_launch_diagnostics);
            result.diagnostics.push_back("host_action=reuse_ready_host");
            return result;
        }

        RemoveFileIfExists(plan.host_ready_path);
        WriteTextFile(plan.host_bootstrap_path, kTbatsaveHostBootstrapMarker);

        const auto start_time = std::chrono::steady_clock::now();
        std::vector<std::string> launch_diagnostics = std::move(pre_launch_diagnostics);
        launch_diagnostics.push_back("host_action=launch_tgstart_host");

        PROCESS_INFORMATION process_info{};
        if (!LaunchProcess(plan, process_info)) {
            return FailWith(
                ErrorCode::kUnexpectedCrash,
                "failed to launch TGStart.exe",
                plan,
                {"last_error=" + std::to_string(GetLastError())}
            );
        }

        const auto deadline = start_time + std::chrono::seconds(options.timeout_seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            if (IsTbatsaveHostReady(plan)) {
                ConversionResult result = MakeSuccessResult(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count()
                );
                result.diagnostics = CollectCommonDiagnostics(plan);
                append_diagnostics(result, launch_diagnostics);
                CloseHandle(process_info.hThread);
                CloseHandle(process_info.hProcess);
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        TerminateProcess(process_info.hProcess, static_cast<UINT>(ErrorCode::kLoadTimeout));
        WaitForSingleObject(process_info.hProcess, 5000);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return FailWith(
            ErrorCode::kLoadTimeout,
            "timed out waiting for TBatSave host ready signal",
            plan,
            std::move(launch_diagnostics)
        );
    }

    if (!PathExists(options.paths.source_path)) {
        return FailWith(
            ErrorCode::kFileNotFound,
            "source DWG not found",
            plan,
            std::move(pre_launch_diagnostics)
        );
    }
    if (DirectoryExists(options.paths.source_path)) {
        return FailWith(
            ErrorCode::kFunctionCallFailed,
            "TBatSave no-UI path accepts one DWG file per invocation",
            plan,
            std::move(pre_launch_diagnostics)
        );
    }
    if (!options.overwrite && PathExists(plan.target_path)) {
        return FailWith(
            ErrorCode::kSaveFailed,
            "target already exists and overwrite is disabled",
            plan,
            std::move(pre_launch_diagnostics)
        );
    }

    EnsureDirectory(plan.batch_output_dir);
    RemoveFileIfExists(plan.worker_status_path);
    if (options.overwrite) {
        RemoveFileIfExists(plan.target_path);
    }
    if (!CopyFileOverwrite(options.paths.source_path, plan.stage_source_path)) {
        return FailWith(
            ErrorCode::kSaveFailed,
            "failed to stage source file for TGStart/TBatSave",
            plan,
            std::move(pre_launch_diagnostics)
        );
    }
    const auto start_time = std::chrono::steady_clock::now();
    std::vector<std::string> post_launch_diagnostics = std::move(pre_launch_diagnostics);

    if (IsTbatsaveHostReady(plan)) {
        post_launch_diagnostics.push_back("host_action=reuse_ready_host_tbatsave");
        if (TryRunTbatsaveDirectWorker(plan, post_launch_diagnostics)) {
            ConversionResult result = WaitForTbatsaveDirectWorkerRelocation(
                plan,
                start_time,
                std::move(post_launch_diagnostics)
            );
            CleanupWorkingDirectory(plan);
            return result;
        }
        CleanupWorkingDirectory(plan);
        return BuildDirectWorkerNoUiFallbackFailureResult(
            plan,
            start_time,
            std::move(post_launch_diagnostics)
        );
    }

    if (IsReusableAcadHostHealthy()) {
        post_launch_diagnostics.push_back("host_action=reuse_acad_tbatsave");
        if (TryRunTbatsaveDirectWorker(plan, post_launch_diagnostics)) {
            ConversionResult result = WaitForTbatsaveDirectWorkerRelocation(
                plan,
                start_time,
                std::move(post_launch_diagnostics)
            );
            CleanupWorkingDirectory(plan);
            return result;
        }
        CleanupWorkingDirectory(plan);
        return BuildDirectWorkerNoUiFallbackFailureResult(
            plan,
            start_time,
            std::move(post_launch_diagnostics)
        );
    } else if (std::find(
                   post_launch_diagnostics.begin(),
                   post_launch_diagnostics.end(),
                   "host_recovery=hung_tianzheng_acad_killed"
               ) != post_launch_diagnostics.end()) {
        post_launch_diagnostics.push_back("host_action=launch_tgstart_tbatsave_after_hung_recovery");
    } else {
        post_launch_diagnostics.push_back("host_action=launch_tgstart_tbatsave");
    }

    PROCESS_INFORMATION process_info{};
    if (!LaunchProcess(plan, process_info)) {
        return FailWith(
            ErrorCode::kUnexpectedCrash,
            "failed to launch TGStart.exe",
            plan,
            {"last_error=" + std::to_string(GetLastError())}
        );
    }

    const auto host_deadline = start_time + std::chrono::seconds(options.timeout_seconds);
    while (std::chrono::steady_clock::now() < host_deadline) {
        if (IsTbatsaveHostReady(plan) || IsReusableAcadHostHealthy()) {
            post_launch_diagnostics.push_back("host_action=launch_tgstart_host_ready");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (TryRunTbatsaveDirectWorker(plan, post_launch_diagnostics)) {
        ConversionResult direct_result = WaitForTbatsaveDirectWorkerRelocation(
            plan,
            start_time,
            std::move(post_launch_diagnostics)
        );
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CleanupWorkingDirectory(plan);
        return direct_result;
    }
    ConversionResult result = BuildDirectWorkerNoUiFallbackFailureResult(
        plan,
        start_time,
        std::move(post_launch_diagnostics)
    );
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CleanupWorkingDirectory(plan);
    return result;
}


std::string ProcessManager::RenderLaunchPlan(const CliOptions& options, const ProcessLaunchPlan& plan) {
    std::ostringstream stream;
    stream << "mode=tgstart_tbatsave_no_ui\n";
    stream << "host_control=" << HostControlModeToString(plan.host_control_mode) << "\n";
    stream << "tbatsave_bind_mode=" << ToBindModeValue(plan.tbatsave_bind_mode) << "\n";
    stream << "tbatsave_bind_ref=" << plan.tbatsave_bind_ref << "\n";
    stream << "config=" << plan.config_path.string() << "\n";
    stream << "workspace=" << plan.workspace_root.string() << "\n";
    stream << "var_root=" << plan.var_root.string() << "\n";
    stream << "source=" << options.paths.source_path.string() << "\n";
    stream << "target=" << plan.target_path.string() << "\n";
    stream << "log=" << plan.log_path.string() << "\n";
    stream << "script=" << plan.script_path.string() << "\n";
    stream << "worker_status=" << plan.worker_status_path.string() << "\n";
    stream << "host_ready=" << plan.host_ready_path.string() << "\n";
    stream << "host_bootstrap=" << plan.host_bootstrap_path.string() << "\n";
    stream << "bootstrap_state=" << plan.bootstrap_state_path.string() << "\n";
    stream << "host_stop=" << plan.host_stop_path.string() << "\n";
    stream << "host_runtime_script=" << plan.host_runtime_script_path.string() << "\n";
    stream << "host_bridge_script=" << plan.host_bridge_script_path.string() << "\n";
    stream << "font_map=" << plan.font_map_path.string() << "\n";
    stream << "font_dir=" << plan.font_dir.string() << "\n";
    stream << "autocad_fonts_dir=" << plan.autocad_fonts_dir.string() << "\n";
    stream << "font_alt=" << plan.font_alt << "\n";
    stream << "font_search_path=" << BuildFontSearchPath(plan) << "\n";
    stream << "host_runtime_template=" << plan.host_runtime_template_path.string() << "\n";
    stream << "host_bridge_template=" << plan.host_bridge_template_path.string() << "\n";
    stream << "host_mnl=" << plan.host_mnl_path.string() << "\n";
    stream << "trigger_log=" << plan.trigger_log_path.string() << "\n";
    stream << "stage_source=" << plan.stage_source_path.string() << "\n";
    stream << "batch_output=" << plan.batch_output_dir.string() << "\n";
    stream << "tarch_root=" << plan.tarch_root.string() << "\n";
    stream << "autocad_root=" << plan.autocad_root.string() << "\n";
    stream << "host_strategy=" << kHostStrategyReuseOrLaunch << "\n";
    stream << "reuse_check=" << kReuseCheckAcadTchKernal << "\n";
    stream << "timeout_seconds=" << options.timeout_seconds << "\n";
    stream << "overwrite=" << (options.overwrite ? "true" : "false") << "\n";
    stream << "command=" << JoinCommandLine(plan.executable, plan.arguments) << "\n";
    stream << "script_contents:\n" << plan.script_contents;
    if (plan.script_contents.empty() || plan.script_contents.back() != '\n') {
        stream << "\n";
    }
    return stream.str();
}


std::string ProcessManager::RenderLaunchPlanJson(const CliOptions& options, const ProcessLaunchPlan& plan) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"mode\": \"tgstart_tbatsave_no_ui\",\n";
    stream << "  \"host_control\": \"" << EscapeJson(HostControlModeToString(plan.host_control_mode))
           << "\",\n";
    stream << "  \"tbatsave_bind_mode\": " << ToBindModeValue(plan.tbatsave_bind_mode) << ",\n";
    stream << "  \"tbatsave_bind_ref\": " << plan.tbatsave_bind_ref << ",\n";
    stream << "  \"config\": \"" << EscapeJson(plan.config_path.string()) << "\",\n";
    stream << "  \"workspace\": \"" << EscapeJson(plan.workspace_root.string()) << "\",\n";
    stream << "  \"var_root\": \"" << EscapeJson(plan.var_root.string()) << "\",\n";
    stream << "  \"source\": \"" << EscapeJson(options.paths.source_path.string()) << "\",\n";
    stream << "  \"target\": \"" << EscapeJson(plan.target_path.string()) << "\",\n";
    stream << "  \"log\": \"" << EscapeJson(plan.log_path.string()) << "\",\n";
    stream << "  \"script\": \"" << EscapeJson(plan.script_path.string()) << "\",\n";
    stream << "  \"worker_status\": \"" << EscapeJson(plan.worker_status_path.string()) << "\",\n";
    stream << "  \"host_ready\": \"" << EscapeJson(plan.host_ready_path.string()) << "\",\n";
    stream << "  \"host_bootstrap\": \"" << EscapeJson(plan.host_bootstrap_path.string()) << "\",\n";
    stream << "  \"bootstrap_state\": \"" << EscapeJson(plan.bootstrap_state_path.string()) << "\",\n";
    stream << "  \"host_stop\": \"" << EscapeJson(plan.host_stop_path.string()) << "\",\n";
    stream << "  \"host_runtime_script\": \"" << EscapeJson(plan.host_runtime_script_path.string()) << "\",\n";
    stream << "  \"host_bridge_script\": \"" << EscapeJson(plan.host_bridge_script_path.string())
           << "\",\n";
    stream << "  \"font_map\": \"" << EscapeJson(plan.font_map_path.string()) << "\",\n";
    stream << "  \"font_dir\": \"" << EscapeJson(plan.font_dir.string()) << "\",\n";
    stream << "  \"autocad_fonts_dir\": \"" << EscapeJson(plan.autocad_fonts_dir.string()) << "\",\n";
    stream << "  \"font_alt\": \"" << EscapeJson(plan.font_alt) << "\",\n";
    stream << "  \"font_search_path\": \"" << EscapeJson(BuildFontSearchPath(plan)) << "\",\n";
    stream << "  \"host_runtime_template\": \"" << EscapeJson(plan.host_runtime_template_path.string())
           << "\",\n";
    stream << "  \"host_bridge_template\": \"" << EscapeJson(plan.host_bridge_template_path.string())
           << "\",\n";
    stream << "  \"host_mnl\": \"" << EscapeJson(plan.host_mnl_path.string()) << "\",\n";
    stream << "  \"trigger_log\": \"" << EscapeJson(plan.trigger_log_path.string()) << "\",\n";
    stream << "  \"stage_source\": \"" << EscapeJson(plan.stage_source_path.string()) << "\",\n";
    stream << "  \"batch_output\": \"" << EscapeJson(plan.batch_output_dir.string()) << "\",\n";
    stream << "  \"tarch_root\": \"" << EscapeJson(plan.tarch_root.string()) << "\",\n";
    stream << "  \"autocad_root\": \"" << EscapeJson(plan.autocad_root.string()) << "\",\n";
    stream << "  \"host_strategy\": \"" << EscapeJson(kHostStrategyReuseOrLaunch) << "\",\n";
    stream << "  \"reuse_check\": \"" << EscapeJson(kReuseCheckAcadTchKernal) << "\",\n";
    stream << "  \"timeout_seconds\": " << options.timeout_seconds << ",\n";
    stream << "  \"overwrite\": " << (options.overwrite ? "true" : "false") << ",\n";
    stream << "  \"command\": \"" << EscapeJson(JoinCommandLine(plan.executable, plan.arguments))
           << "\",\n";
    stream << "  \"script_contents\": \"" << EscapeJson(plan.script_contents) << "\"\n";
    stream << "}\n";
    return stream.str();
}


size_t ProcessManager::StopTianzhengAcadHosts() {
    return KillTianzhengAcadHosts();
}

}  // namespace t3conv
