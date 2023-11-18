#include "utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace t3conv {

namespace {

constexpr std::uintmax_t kLogRollBytes = 20ULL * 1024ULL * 1024ULL;
constexpr int kMaxRolledLogs = 20;

std::uintmax_t FileSizeOrZero(const std::filesystem::path& path) {
    std::error_code error_code;
    if (!std::filesystem::exists(path, error_code)) {
        return 0;
    }
    const auto size = std::filesystem::file_size(path, error_code);
    return error_code ? 0 : size;
}

void RemoveRolledLogs(const std::filesystem::path& base_log_path) {
    const std::filesystem::path roll_dir = base_log_path.parent_path() / "log";
    std::error_code error_code;
    for (int index = 1; index <= kMaxRolledLogs; ++index) {
        std::filesystem::remove(
            roll_dir / (base_log_path.filename().string() + "." + std::to_string(index)),
            error_code
        );
        error_code.clear();
    }
}

}  // namespace

ConversionResult MakeSuccessResult(double elapsed_sec) {
    ConversionResult result;
    result.success = true;
    result.error_code = ErrorCode::kSuccess;
    result.elapsed_sec = elapsed_sec;
    return result;
}


ConversionResult MakeFailureResult(
    ErrorCode error_code,
    const std::string& error_message,
    std::vector<std::string> diagnostics
) {
    ConversionResult result;
    result.success = false;
    result.error_code = error_code;
    result.error_message = error_message;
    result.diagnostics = std::move(diagnostics);
    return result;
}


std::filesystem::path BuildDefaultTargetPath(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_dir
) {
    const std::filesystem::path target_dir =
        output_dir.empty() ? source_path.parent_path() : output_dir;
    return target_dir / (source_path.stem().string() + "_t3" + source_path.extension().string());
}


std::filesystem::path BuildDefaultLogPath(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_dir
) {
    const std::filesystem::path target_dir =
        output_dir.empty() ? source_path.parent_path() : output_dir;
    return target_dir / (source_path.stem().string() + "_t3.log");
}


bool PathExists(const std::filesystem::path& path) {
    std::error_code error_code;
    return !path.empty() && std::filesystem::exists(path, error_code);
}


bool DirectoryExists(const std::filesystem::path& path) {
    std::error_code error_code;
    return !path.empty() &&
           std::filesystem::exists(path, error_code) &&
           std::filesystem::is_directory(path, error_code);
}


bool EnsureDirectory(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error_code;
    std::filesystem::create_directories(path, error_code);
    return !error_code;
}


std::filesystem::path PrepareAppendLogPath(const std::filesystem::path& base_log_path) {
    if (base_log_path.empty()) {
        return {};
    }
    if (FileSizeOrZero(base_log_path) < kLogRollBytes) {
        EnsureDirectory(base_log_path.parent_path());
        return base_log_path;
    }

    const std::filesystem::path roll_dir = base_log_path.parent_path() / "log";
    EnsureDirectory(roll_dir);
    const std::string rolled_name_prefix = base_log_path.filename().string() + ".";
    for (int index = 1; index <= kMaxRolledLogs; ++index) {
        const std::filesystem::path rolled_path = roll_dir / (rolled_name_prefix + std::to_string(index));
        if (FileSizeOrZero(rolled_path) < kLogRollBytes) {
            return rolled_path;
        }
    }

    RemoveRolledLogs(base_log_path);
    return roll_dir / (rolled_name_prefix + "1");
}


std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}


std::string ErrorCodeToString(ErrorCode error_code) {
    switch (error_code) {
        case ErrorCode::kSuccess:
            return "success";
        case ErrorCode::kFileNotFound:
            return "file_not_found";
        case ErrorCode::kFileCorrupt:
            return "file_corrupt";
        case ErrorCode::kLoadTimeout:
            return "load_timeout";
        case ErrorCode::kTchObjectMissing:
            return "tch_object_missing";
        case ErrorCode::kArxLoadFailed:
            return "arx_load_failed";
        case ErrorCode::kFunctionCallFailed:
            return "function_call_failed";
        case ErrorCode::kSaveFailed:
            return "save_failed";
        case ErrorCode::kOutOfMemory:
            return "out_of_memory";
        case ErrorCode::kArgumentError:
            return "argument_error";
        case ErrorCode::kUnexpectedCrash:
            return "unexpected_crash";
    }

    return "unknown";
}


std::string QuoteCommandArg(const std::string& value) {
    std::string escaped = value;
    std::string::size_type pos = 0;
    while ((pos = escaped.find('"', pos)) != std::string::npos) {
        escaped.insert(pos, "\\");
        pos += 2;
    }
    return "\"" + escaped + "\"";
}


std::string QuoteCommandArg(const std::filesystem::path& value) {
    return QuoteCommandArg(value.string());
}


std::string JoinCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments
) {
    std::ostringstream stream;
    stream << QuoteCommandArg(executable);
    for (const std::string& arg : arguments) {
        stream << ' ' << QuoteCommandArg(arg);
    }
    return stream.str();
}


bool IsPathLikeArgument(const std::string& value) {
    return value.find('\\') != std::string::npos ||
           value.find('/') != std::string::npos ||
           ToLowerAscii(std::filesystem::path(value).extension().string()) == ".dwg";
}


std::string JoinLogCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments
) {
    std::ostringstream stream;
    stream << executable.string();
    for (const std::string& arg : arguments) {
        stream << ' ' << (IsPathLikeArgument(arg) ? QuoteCommandArg(arg) : arg);
    }
    return stream.str();
}

}  // namespace t3conv
