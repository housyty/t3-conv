#include "args_parser.h"
#include "batch_runner.h"
#include "config_loader.h"
#include "process_mgr.h"

#include "../common/utils.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
    if (!parse_result.ok) {
        std::cerr << "argument_error: " << parse_result.error_message << std::endl;
        std::cerr << t3conv::BuildUsage();
        return static_cast<int>(t3conv::ErrorCode::kArgumentError);
    }

    if (parse_result.show_help) {
        std::cout << t3conv::BuildUsage();
        return static_cast<int>(t3conv::ErrorCode::kSuccess);
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
