#include "args_parser.h"
#include "batch_runner.h"
#include "config_loader.h"
#include "process_mgr.h"

#include "../common/utils.h"

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string FormatSeconds(double seconds) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds;
    return stream.str();
}


std::string FormatLocalTimestamp(std::chrono::system_clock::time_point time_point) {
    const std::time_t raw_time = std::chrono::system_clock::to_time_t(time_point);
    std::tm local_time{};
    localtime_s(&local_time, &raw_time);
    std::ostringstream stream;
    stream << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}


std::uintmax_t FileSizeOrZero(const std::filesystem::path& path) {
    std::error_code error_code;
    if (!std::filesystem::exists(path, error_code)) {
        return 0;
    }
    const auto size = std::filesystem::file_size(path, error_code);
    return error_code ? 0 : size;
}


std::uintmax_t KbRoundedUp(std::uintmax_t bytes) {
    return bytes == 0 ? 0 : static_cast<std::uintmax_t>(std::ceil(bytes / 1024.0));
}


std::wstring BuildConversionMutexName(const std::filesystem::path& tangent_root) {
    std::wstring normalized = tangent_root.lexically_normal().wstring();
    for (wchar_t& ch : normalized) {
        if (ch >= L'a' && ch <= L'z') {
            ch = static_cast<wchar_t>(ch - L'a' + L'A');
        }
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L' ' || ch == L'.') {
            ch = L'_';
        }
    }
    return L"Global\\T3CONV_CONVERSION_" + normalized;
}


class ConversionMutex {
public:
    ConversionMutex() = default;
    ConversionMutex(const ConversionMutex&) = delete;
    ConversionMutex& operator=(const ConversionMutex&) = delete;

    ~ConversionMutex() {
        if (locked_ && handle_ != nullptr) {
            ReleaseMutex(handle_);
        }
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    bool Acquire(const std::filesystem::path& tangent_root) {
        if (LockAlreadyHeld()) {
            return true;
        }
        handle_ = CreateMutexW(nullptr, FALSE, BuildConversionMutexName(tangent_root).c_str());
        if (handle_ == nullptr) {
            return false;
        }
        const DWORD wait_result = WaitForSingleObject(handle_, INFINITE);
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
            return false;
        }
        locked_ = true;
        SetEnvironmentVariableW(L"T3CONV_CONVERSION_LOCK_HELD", L"1");
        return true;
    }

private:
    bool LockAlreadyHeld() const {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(
                   L"T3CONV_CONVERSION_LOCK_HELD",
                   value,
                   static_cast<DWORD>(std::size(value))
               ) > 0;
    }

    HANDLE handle_ = nullptr;
    bool locked_ = false;
};


bool IsCurrentProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned_size = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned_size) &&
        elevation.TokenIsElevated != 0) {
        CloseHandle(token);
        return true;
    }

    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;
    BOOL is_admin = FALSE;
    if (AllocateAndInitializeSid(
            &nt_authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &admin_group
        )) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    CloseHandle(token);
    return is_admin == TRUE;
}


bool ElevationAttemptAlreadyMade() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"T3CONV_ELEVATION_ATTEMPTED", value, static_cast<DWORD>(std::size(value))) > 0;
}


bool ShouldSelfElevateForConversion(const t3conv::ParseResult& parse_result) {
    if (parse_result.show_help || parse_result.options.dry_run) {
        return false;
    }
    if (ElevationAttemptAlreadyMade()) {
        return false;
    }
    return !IsCurrentProcessElevated();
}


std::wstring CurrentExecutablePath() {
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
    return buffer;
}


bool NeedsWindowsArgumentQuotes(const std::wstring& value) {
    return value.empty() || value.find_first_of(L" \t\n\v\"") != std::wstring::npos;
}


std::wstring QuoteWindowsArgument(const std::wstring& value) {
    if (!NeedsWindowsArgumentQuotes(value)) {
        return value;
    }

    std::wstring quoted = L"\"";
    size_t backslash_count = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslash_count;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslash_count = 0;
            continue;
        }
        quoted.append(backslash_count, L'\\');
        backslash_count = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslash_count * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}


void AppendWindowsArgument(std::wstring& parameters, const std::wstring& value) {
    if (!parameters.empty()) {
        parameters.push_back(L' ');
    }
    parameters += QuoteWindowsArgument(value);
}


void AppendNamedWindowsArgument(
    std::wstring& parameters,
    const wchar_t* name,
    const std::filesystem::path& value
) {
    if (value.empty()) {
        return;
    }
    AppendWindowsArgument(parameters, name);
    AppendWindowsArgument(parameters, value.wstring());
}


void AppendNamedWindowsArgument(
    std::wstring& parameters,
    const wchar_t* name,
    const std::string& value
) {
    if (value.empty()) {
        return;
    }
    AppendWindowsArgument(parameters, name);
    AppendWindowsArgument(parameters, std::filesystem::path(value).wstring());
}


std::filesystem::path BuildElevatedOutputPath(const char* stream_name) {
    const std::string file_name =
        "t3conv-elevated-" +
        std::to_string(GetCurrentProcessId()) +
        "-" +
        std::to_string(GetTickCount64()) +
        "-" +
        stream_name +
        ".txt";
    std::error_code error_code;
    return (std::filesystem::temp_directory_path(error_code) / file_name).lexically_normal();
}


std::optional<std::wstring> BuildRelaunchParameters(
    const t3conv::ParseResult& parse_result,
    const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path
) {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return std::nullopt;
    }

    std::wstring parameters;
    for (int index = 1; index < argument_count; ++index) {
        AppendWindowsArgument(parameters, arguments[index] == nullptr ? L"" : arguments[index]);
    }
    LocalFree(arguments);

    AppendNamedWindowsArgument(parameters, L"--internal-stdout", stdout_path);
    AppendNamedWindowsArgument(parameters, L"--internal-stderr", stderr_path);
    AppendNamedWindowsArgument(
        parameters,
        L"--internal-tangent-root",
        parse_result.config.resolved.tangent_root
    );
    AppendNamedWindowsArgument(
        parameters,
        L"--internal-tangent-sys-dir",
        parse_result.config.resolved.tangent_sys_dir
    );
    AppendNamedWindowsArgument(
        parameters,
        L"--internal-autocad-root",
        parse_result.config.resolved.autocad_root
    );
    AppendNamedWindowsArgument(parameters, L"--internal-font-dir", parse_result.config.resolved.font_dir);
    AppendNamedWindowsArgument(
        parameters,
        L"--internal-autocad-fonts-dir",
        parse_result.config.resolved.autocad_fonts_dir
    );
    AppendNamedWindowsArgument(parameters, L"--internal-font-alt", parse_result.config.font_alt);
    return parameters;
}


void ReplayAndRemoveOneInternalOutput(const std::filesystem::path& path, std::ostream& stream) {
    if (path.empty()) {
        return;
    }

    std::ifstream input(path, std::ios::binary);
    if (input.is_open()) {
        stream << input.rdbuf();
    }
    input.close();

    std::error_code error_code;
    std::filesystem::remove(path, error_code);
}


void ReplayAndRemoveInternalOutput(
    const std::filesystem::path& stdout_path,
    const std::filesystem::path& stderr_path
) {
    ReplayAndRemoveOneInternalOutput(stdout_path, std::cout);
    ReplayAndRemoveOneInternalOutput(stderr_path, std::cerr);
}


bool RedirectOneInternalOutputFile(FILE* stream, const std::filesystem::path& path) {
    if (path.empty()) {
        return true;
    }

    std::error_code error_code;
    std::filesystem::create_directories(path.parent_path(), error_code);
    FILE* redirected = nullptr;
    return _wfreopen_s(&redirected, path.wstring().c_str(), L"wb", stream) == 0;
}


bool RedirectInternalOutputFiles(const t3conv::ParseResult& parse_result) {
    const bool stdout_ok =
        RedirectOneInternalOutputFile(stdout, parse_result.internal_stdout_path);
    const bool stderr_ok =
        RedirectOneInternalOutputFile(stderr, parse_result.internal_stderr_path);
    std::cout.clear();
    std::cerr.clear();
    return stdout_ok && stderr_ok;
}


int RelaunchElevatedAndWait(const t3conv::ParseResult& parse_result) {
    const std::wstring executable = CurrentExecutablePath();
    const std::filesystem::path stdout_path = BuildElevatedOutputPath("stdout");
    const std::filesystem::path stderr_path = BuildElevatedOutputPath("stderr");
    const auto parameters = BuildRelaunchParameters(parse_result, stdout_path, stderr_path);
    if (executable.empty() || !parameters.has_value()) {
        std::cerr << "elevation_error=prepare_failed\n";
        return static_cast<int>(t3conv::ErrorCode::kUnexpectedCrash);
    }

    std::error_code current_path_error;
    const std::wstring current_directory = std::filesystem::current_path(current_path_error).wstring();
    SetEnvironmentVariableW(L"T3CONV_ELEVATION_ATTEMPTED", L"1");

    SHELLEXECUTEINFOW shell_execute_info{};
    shell_execute_info.cbSize = sizeof(shell_execute_info);
    shell_execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
    shell_execute_info.lpVerb = L"runas";
    shell_execute_info.lpFile = executable.c_str();
    shell_execute_info.lpParameters = parameters->c_str();
    shell_execute_info.lpDirectory = current_directory.empty() ? nullptr : current_directory.c_str();
    shell_execute_info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&shell_execute_info)) {
        std::cerr << "elevation_error=" << GetLastError() << "\n";
        return static_cast<int>(t3conv::ErrorCode::kUnexpectedCrash);
    }

    WaitForSingleObject(shell_execute_info.hProcess, INFINITE);
    DWORD exit_code = static_cast<DWORD>(t3conv::ErrorCode::kUnexpectedCrash);
    GetExitCodeProcess(shell_execute_info.hProcess, &exit_code);
    CloseHandle(shell_execute_info.hProcess);
    ReplayAndRemoveInternalOutput(stdout_path, stderr_path);
    return static_cast<int>(exit_code);
}


std::vector<std::string> BuildSingleConversionRecord(
    const t3conv::CliOptions& options,
    const t3conv::ProcessLaunchPlan& plan,
    const t3conv::ConversionResult& result,
    const std::string& started_at,
    const std::string& copyable_command
) {
    std::vector<std::string> lines = {
        copyable_command,
        "name=" + options.paths.source_path.filename().string(),
        "source=" + options.paths.source_path.string(),
        "output=" + plan.target_path.string(),
        std::string("status=") + (result.success ? "success" : "failure"),
        "started_at=" + started_at,
        "duration=" + FormatSeconds(result.elapsed_sec) + "s",
        "size=" + std::to_string(KbRoundedUp(FileSizeOrZero(plan.target_path))) + "kb"
    };
    if (!result.success) {
        lines.push_back("error=" + result.error_message);
    }
    return lines;
}


std::string BuildSingleCopyableCommand(
    const std::filesystem::path& executable,
    const t3conv::CliOptions& options
) {
    std::vector<std::string> arguments = {
        "-s",
        options.paths.source_path.string(),
        "-o",
        options.paths.target_path.string()
    };
    if (!options.overwrite) {
        arguments.push_back("--no-overwrite");
    }
    if (options.timeout_seconds != 120) {
        arguments.push_back("--timeout-seconds");
        arguments.push_back(std::to_string(options.timeout_seconds));
    }
    if (options.tbatsave_bind_mode != t3conv::TbatsaveBindMode::kKeepDefault) {
        arguments.push_back("--tbatsave-bindmode");
        arguments.push_back(std::to_string(static_cast<int>(options.tbatsave_bind_mode)));
    }
    if (options.tbatsave_bind_ref >= 0) {
        arguments.push_back("--tbatsave-bindref");
        arguments.push_back(std::to_string(options.tbatsave_bind_ref));
    }
    return t3conv::JoinLogCommandLine(executable, arguments);
}


void WriteSingleConversionLog(
    const std::filesystem::path& log_path,
    const std::vector<std::string>& lines
) {
    if (log_path.empty()) {
        return;
    }
    const std::filesystem::path append_log_path = t3conv::PrepareAppendLogPath(log_path);
    std::ofstream stream(append_log_path, std::ios::binary | std::ios::app);
    for (const std::string& line : lines) {
        stream << line << "\n";
    }
    stream << "--------------------------------------------------------------------------------\n";
}


void PrintSingleConversionRecord(
    std::ostream& stream,
    const std::vector<std::string>& lines
) {
    for (const std::string& line : lines) {
        stream << line << "\n";
    }
}


void PrintDiagnostics(std::ostream& stream, const t3conv::ConversionResult& result) {
    stream << "debug=true\n";
    stream << "elapsed_seconds=" << result.elapsed_sec << "\n";
    stream << "error_code=" << t3conv::ErrorCodeToString(result.error_code) << "\n";
    for (const std::string& diagnostic : result.diagnostics) {
        stream << diagnostic << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const t3conv::ParseResult parse_result = t3conv::ParseArgs(argc, argv);
    RedirectInternalOutputFiles(parse_result);
    if (!parse_result.ok) {
        std::cerr << "argument_error: " << parse_result.error_message << std::endl;
        std::cerr << t3conv::BuildUsage();
        return static_cast<int>(t3conv::ErrorCode::kArgumentError);
    }

    if (parse_result.show_help) {
        std::cout << t3conv::BuildUsage();
        return static_cast<int>(t3conv::ErrorCode::kSuccess);
    }

    if (ShouldSelfElevateForConversion(parse_result)) {
        return RelaunchElevatedAndWait(parse_result);
    }

    ConversionMutex conversion_mutex;
    if (!parse_result.options.dry_run &&
        !conversion_mutex.Acquire(parse_result.config.resolved.tangent_root)) {
        std::cerr << "conversion_lock_error=acquire_failed\n";
        return static_cast<int>(t3conv::ErrorCode::kUnexpectedCrash);
    }

    if (t3conv::BatchRunner::IsBatchRequest(parse_result.options)) {
        if (parse_result.options.dry_run) {
            std::cout << t3conv::BatchRunner::RenderPlan(
                parse_result.options,
                parse_result.config,
                argv[0]
            );
            return static_cast<int>(t3conv::ErrorCode::kSuccess);
        }
        if (parse_result.options.debug) {
            std::cout << t3conv::BatchRunner::RenderPlan(
                parse_result.options,
                parse_result.config,
                argv[0]
            );
        }
        return t3conv::BatchRunner::Execute(parse_result.options, parse_result.config, argv[0]);
    }

    const t3conv::ProcessLaunchPlan plan =
        t3conv::ProcessManager::BuildLaunchPlan(parse_result.options, parse_result.config);

    if (parse_result.options.dry_run) {
        if (parse_result.options.json) {
            std::cout << t3conv::ProcessManager::RenderLaunchPlanJson(parse_result.options, plan);
        } else {
            std::cout << t3conv::ProcessManager::RenderLaunchPlan(parse_result.options, plan);
        }
        return static_cast<int>(t3conv::ErrorCode::kSuccess);
    }

    if (parse_result.options.debug) {
        if (parse_result.options.json) {
            std::cout << t3conv::ProcessManager::RenderLaunchPlanJson(parse_result.options, plan);
        } else {
            std::cout << t3conv::ProcessManager::RenderLaunchPlan(parse_result.options, plan);
        }
    }

    const std::string started_at = FormatLocalTimestamp(std::chrono::system_clock::now());
    const t3conv::ConversionResult result =
        t3conv::ProcessManager::Execute(parse_result.options, plan);
    const std::string copyable_command =
        BuildSingleCopyableCommand(argv[0] == nullptr ? std::filesystem::path("t3conv.exe")
                                                      : std::filesystem::path(argv[0]),
                                   parse_result.options);
    const std::vector<std::string> record =
        BuildSingleConversionRecord(parse_result.options, plan, result, started_at, copyable_command);
    WriteSingleConversionLog(plan.log_path, record);

    if (result.success) {
        PrintSingleConversionRecord(std::cout, record);
        if (parse_result.options.debug) {
            PrintDiagnostics(std::cout, result);
        }
        return static_cast<int>(result.error_code);
    }

    PrintSingleConversionRecord(std::cerr, record);
    if (parse_result.options.debug) {
        PrintDiagnostics(std::cerr, result);
    }
    return static_cast<int>(result.error_code);
}
