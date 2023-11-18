#pragma once

#include "types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace t3conv {

ConversionResult MakeSuccessResult(double elapsed_sec);
ConversionResult MakeFailureResult(
    ErrorCode error_code,
    const std::string& error_message,
    std::vector<std::string> diagnostics = {}
);

std::filesystem::path BuildDefaultTargetPath(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_dir = {}
);
std::filesystem::path BuildDefaultLogPath(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_dir = {}
);

bool PathExists(const std::filesystem::path& path);
bool DirectoryExists(const std::filesystem::path& path);
bool EnsureDirectory(const std::filesystem::path& path);
std::filesystem::path PrepareAppendLogPath(const std::filesystem::path& base_log_path);
std::string ToLowerAscii(std::string value);
std::string ErrorCodeToString(ErrorCode error_code);
std::string QuoteCommandArg(const std::string& value);
std::string QuoteCommandArg(const std::filesystem::path& value);
std::string JoinCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments
);
std::string JoinLogCommandLine(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments
);

}  // namespace t3conv
