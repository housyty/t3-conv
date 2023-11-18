#include "batch_runner.h"
#include "process_mgr.h"

#include "../common/utils.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace t3conv {

namespace {

constexpr int kMaxHostRestartsPerBatch = 5;

struct ChildRun {
    int exit_code = 99;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
};

struct BatchRow {
    int index = 0;
    int attempt = 0;
    std::string started_at;
    std::filesystem::path source;
    std::filesystem::path target;
    int exit_code = 99;
    std::string internal_status;
    std::string save_result;
    std::string host_action;
    double seconds = 0.0;
    bool target_exists = false;
    std::uintmax_t target_bytes = 0;
    bool success = false;
    std::filesystem::path log_path;
};

std::vector<std::filesystem::path> EnumerateDwgs(const std::filesystem::path& source_dir) {
    std::vector<std::filesystem::path> files;
    std::error_code error_code;
    for (const auto& entry : std::filesystem::directory_iterator(source_dir, error_code)) {
        if (error_code || !entry.is_regular_file(error_code)) {
            continue;
        }
        if (ToLowerAscii(entry.path().extension().string()) == ".dwg") {
            files.push_back(std::filesystem::absolute(entry.path()).lexically_normal());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::filesystem::path OutputDirectory(const CliOptions& options) {
    if (!options.paths.output_dir.empty()) {
        return options.paths.output_dir;
    }
    return options.paths.source_path;
}

std::filesystem::path BatchTargetPath(
    const std::filesystem::path& source,
    const std::filesystem::path& output_dir
) {
    return output_dir / (source.stem().string() + "_t3" + source.extension().string());
}

void AppendLines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    EnsureDirectory(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    for (const std::string& line : lines) {
        stream << line << "\n";
    }
}

std::string LastRegexValue(const std::string& text, const std::string& prefix) {
    std::string value;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) == 0) {
            value = line.substr(prefix.size());
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
        }
    }
    return value;
}

std::string ErrorSummary(
    const ChildRun& child,
    const BatchRow& row
) {
    if (child.timed_out) {
        return "timeout";
    }
    if (!row.target_exists) {
        return "output_missing";
    }
    if (row.internal_status != "success") {
        return "status_not_success";
    }
    if (row.save_result != "5100") {
        return "save_result_not_5100";
    }
    return "unknown_failure";
}

bool ShouldRestartHostAfterFailure(const ChildRun& child, const BatchRow& row) {
    if (row.success) {
        return false;
    }
    if (child.timed_out) {
        return true;
    }
    if (row.exit_code == static_cast<int>(ErrorCode::kLoadTimeout) ||
        row.exit_code == static_cast<int>(ErrorCode::kUnexpectedCrash)) {
        return true;
    }
    if (!row.host_action.empty() &&
        row.host_action != "tbatsave_direct_worker_succeeded") {
        return true;
    }
    return false;
}

bool ChildProducedRecord(const ChildRun& child) {
    return child.stdout_text.find("\nstatus=") != std::string::npos ||
           child.stdout_text.rfind("status=", 0) == 0 ||
           child.stderr_text.find("\nstatus=") != std::string::npos ||
           child.stderr_text.rfind("status=", 0) == 0;
}

std::string ReadPipe(HANDLE pipe) {
    std::string output;
    char buffer[4096];
    DWORD bytes_read = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buffer, bytes_read);
    }
    return output;
}

ChildRun RunChild(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_dir,
    int timeout_seconds
) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    CreatePipe(&stdout_read, &stdout_write, &security_attributes, 0);
    CreatePipe(&stderr_read, &stderr_write, &security_attributes, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = stderr_write;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process_info{};
    std::string command_line = JoinCommandLine(executable, arguments);
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');

    ChildRun result;
    const BOOL created = CreateProcessA(
        nullptr,
        mutable_command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        working_dir.empty() ? nullptr : working_dir.string().c_str(),
        &startup_info,
        &process_info
    );

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (!created) {
        result.stderr_text = "batch_launch_error=" + std::to_string(GetLastError()) + "\n";
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return result;
    }

    std::thread stdout_reader([&result, stdout_read]() {
        result.stdout_text = ReadPipe(stdout_read);
    });
    std::thread stderr_reader([&result, stderr_read]() {
        result.stderr_text = ReadPipe(stderr_read);
    });

    const DWORD wait_ms = static_cast<DWORD>((timeout_seconds + 60) * 1000);
    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, wait_ms);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(process_info.hProcess, 124);
    }
    WaitForSingleObject(process_info.hProcess, 5000);
    DWORD exit_code = 99;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);

    stdout_reader.join();
    stderr_reader.join();
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    return result;
}

void StopTianzhengAcad() {
    ProcessManager::StopTianzhengAcadHosts();
    Sleep(3000);
}

std::vector<std::string> BuildChildArgs(
    const CliOptions& options,
    const std::filesystem::path& source,
    const std::filesystem::path& target
) {
    std::vector<std::string> child_args = {
        "--timeout-seconds",
        std::to_string(options.timeout_seconds),
        "-s",
        source.string(),
        "-o",
        target.string()
    };
    if (!options.overwrite) {
        child_args.push_back("--no-overwrite");
    }
    if (options.tbatsave_bind_mode != TbatsaveBindMode::kKeepDefault) {
        child_args.push_back("--tbatsave-bindmode");
        child_args.push_back(std::to_string(static_cast<int>(options.tbatsave_bind_mode)));
    }
    if (options.tbatsave_bind_ref >= 0) {
        child_args.push_back("--tbatsave-bindref");
        child_args.push_back(std::to_string(options.tbatsave_bind_ref));
    }
    return child_args;
}

std::string JoinDisplayArgs(const std::vector<std::string>& arguments) {
    std::ostringstream stream;
    for (size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) {
            stream << ' ';
        }
        stream << arguments[index];
    }
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

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::filesystem::path WriteBatchLog(
    const CliOptions& options,
    const std::string& copyable,
    const BatchRow& row,
    const ChildRun& child
) {
    const std::filesystem::path log_path = PrepareAppendLogPath(options.paths.log_path);
    std::vector<std::string> lines = {
        copyable,
        "name=" + row.source.filename().string(),
        "source=" + row.source.string(),
        "output=" + row.target.string(),
        std::string("status=") + (row.success ? "success" : "failure"),
        "started_at=" + row.started_at,
        "duration=" + FormatSeconds(row.seconds) + "s",
        "size=" + std::to_string(KbRoundedUp(row.target_bytes)) + "kb"
    };
    if (!row.success) {
        lines.push_back("error=" + ErrorSummary(child, row));
    }
    if (options.debug) {
        lines.push_back("debug=true");
        lines.push_back("index=" + std::to_string(row.index));
        lines.push_back("attempt=" + std::to_string(row.attempt));
        lines.push_back("exit_code=" + std::to_string(row.exit_code));
        lines.push_back("internal_status=" + row.internal_status);
        lines.push_back("save_result=" + row.save_result);
        lines.push_back(std::string("target_exists=") + (row.target_exists ? "true" : "false"));
        lines.push_back("target_bytes=" + std::to_string(row.target_bytes));
        lines.push_back("host_action=" + row.host_action);
        lines.push_back("--- debug stdout ---");
        const auto stdout_lines = SplitLines(child.stdout_text);
        lines.insert(lines.end(), stdout_lines.begin(), stdout_lines.end());
        lines.push_back("--- debug stderr ---");
        const auto stderr_lines = SplitLines(child.stderr_text);
        lines.insert(lines.end(), stderr_lines.begin(), stderr_lines.end());
    }
    lines.push_back("--------------------------------------------------------------------------------");
    AppendLines(log_path, lines);
    return log_path;
}

void AppendBatchSummaryToLog(
    const CliOptions& options,
    const std::vector<BatchRow>& rows,
    double total_seconds,
    int host_restart_count,
    bool batch_aborted
) {
    int success_count = 0;
    for (const BatchRow& row : rows) {
        if (row.success) {
            ++success_count;
        }
    }
    const int failed_count = static_cast<int>(rows.size()) - success_count;
    const double avg_seconds = rows.empty() ? 0.0 : total_seconds / rows.size();

    const std::filesystem::path log_path = PrepareAppendLogPath(options.paths.log_path);
    std::vector<std::string> lines = {
        "================================================================================",
        "batch_summary",
        "source_dir=" + options.paths.source_path.string(),
        "output_dir=" + OutputDirectory(options).string(),
        "total=" + std::to_string(rows.size()),
        "success=" + std::to_string(success_count),
        "failed=" + std::to_string(failed_count),
        "total_seconds=" + FormatSeconds(total_seconds),
        "avg_seconds=" + FormatSeconds(avg_seconds),
        "host_restart_count=" + std::to_string(host_restart_count),
        "host_restart_limit=" + std::to_string(kMaxHostRestartsPerBatch),
        std::string("host_restart_limit_exceeded=") +
            (batch_aborted ? "true" : "false"),
        std::string("batch_aborted=") + (batch_aborted ? "true" : "false"),
        "================================================================================"
    };
    AppendLines(log_path, lines);
}

}  // namespace

bool BatchRunner::IsBatchRequest(const CliOptions& options) {
    return DirectoryExists(options.paths.source_path);
}

std::string BatchRunner::RenderPlan(
    const CliOptions& options,
    const AppConfig& config,
    const char* executable_path
) {
    const std::filesystem::path output_dir = OutputDirectory(options);
    const auto files = EnumerateDwgs(options.paths.source_path);
    const std::filesystem::path child_source =
        files.empty() ? std::filesystem::path("<source.dwg>") : files.front();
    const std::filesystem::path child_target =
        files.empty() ? output_dir / "<source>_t3.dwg" : BatchTargetPath(files.front(), output_dir);
    const std::vector<std::string> child_args = BuildChildArgs(options, child_source, child_target);

    std::ostringstream stream;
    stream << "mode=t3conv_batch_parent\n";
    stream << "source_dir=" << options.paths.source_path.string() << "\n";
    stream << "output_dir=" << output_dir.string() << "\n";
    stream << "log=" << options.paths.log_path.string() << "\n";
    stream << "bootstrap_state=" << config.resolved.bootstrap_state_path.string() << "\n";
    stream << "file_count=" << files.size() << "\n";
    stream << "debug=" << (options.debug ? "true" : "false") << "\n";
    stream << "retries=" << options.retries << "\n";
    stream << "child_options=" << JoinDisplayArgs(child_args) << "\n";
    stream << "child_command=" << JoinCommandLine(executable_path, child_args) << "\n";
    return stream.str();
}

int BatchRunner::Execute(
    const CliOptions& options,
    const AppConfig&,
    const char* executable_path
) {
    const std::filesystem::path output_dir = OutputDirectory(options);
    EnsureDirectory(output_dir);
    const auto files = EnumerateDwgs(options.paths.source_path);
    std::vector<BatchRow> final_rows;
    const auto total_start = std::chrono::steady_clock::now();
    int host_restart_count = 0;
    bool batch_aborted = false;

    for (size_t index = 0; index < files.size() && !batch_aborted; ++index) {
        const std::filesystem::path source = files[index];
        const std::filesystem::path target = BatchTargetPath(source, output_dir);
        BatchRow last_row;

        for (int attempt = 1; attempt <= options.retries + 1; ++attempt) {
            const std::vector<std::string> child_args = BuildChildArgs(options, source, target);
            const std::string copyable = JoinLogCommandLine(executable_path, child_args);
            const auto wall_start = std::chrono::system_clock::now();
            const auto start = std::chrono::steady_clock::now();
            const ChildRun child = RunChild(
                executable_path,
                child_args,
                output_dir,
                options.timeout_seconds
            );
            const auto stop = std::chrono::steady_clock::now();
            const std::string all_text = child.stdout_text + "\n" + child.stderr_text;

            last_row.index = static_cast<int>(index + 1);
            last_row.attempt = attempt;
            last_row.started_at = FormatLocalTimestamp(wall_start);
            last_row.source = source;
            last_row.target = target;
            last_row.exit_code = child.exit_code;
            last_row.internal_status = LastRegexValue(all_text, "status=");
            last_row.save_result = LastRegexValue(all_text, "tbatsave_direct_worker_save_result=");
            last_row.host_action = LastRegexValue(all_text, "host_action=");
            last_row.seconds = std::chrono::duration<double>(stop - start).count();
            last_row.target_exists = PathExists(target);
            last_row.target_bytes = FileSizeOrZero(target);
            last_row.success = !child.timed_out &&
                               last_row.exit_code == 0 &&
                               last_row.target_exists &&
                               last_row.target_bytes > 0 &&
                               (last_row.internal_status.empty() ||
                                last_row.internal_status == "success") &&
                               (last_row.save_result.empty() ||
                                last_row.save_result == "5100");
            last_row.log_path = options.paths.log_path;
            if (!ChildProducedRecord(child)) {
                last_row.log_path = WriteBatchLog(options, copyable, last_row, child);
            }

            std::cout << last_row.index << "/" << files.size()
                      << " attempt=" << attempt
                      << " status=" << (last_row.success ? "success" : "failure")
                      << " file=" << source.filename().string()
                      << "\n";

            if (last_row.success) {
                break;
            }
            if (ShouldRestartHostAfterFailure(child, last_row)) {
                if (host_restart_count >= kMaxHostRestartsPerBatch) {
                    batch_aborted = true;
                    std::cout << "host_restart_limit_exceeded=true\n";
                    break;
                }
                StopTianzhengAcad();
                ++host_restart_count;
            }
        }

        final_rows.push_back(last_row);
    }

    const auto total_stop = std::chrono::steady_clock::now();
    const double total_seconds = std::chrono::duration<double>(total_stop - total_start).count();
    AppendBatchSummaryToLog(
        options,
        final_rows,
        total_seconds,
        host_restart_count,
        batch_aborted
    );

    const bool all_success = std::all_of(final_rows.begin(), final_rows.end(), [](const BatchRow& row) {
        return row.success;
    });
    std::cout << "status=" << (all_success ? "success" : "failure") << "\n";
    std::cout << "total=" << final_rows.size() << "\n";
    if (batch_aborted) {
        std::cout << "batch_aborted=host_restart_limit_exceeded\n";
    }
    return all_success ? 0 : static_cast<int>(ErrorCode::kSaveFailed);
}

}  // namespace t3conv
