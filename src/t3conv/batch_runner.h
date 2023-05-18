#pragma once

#include "../common/types.h"

#include <string>

namespace t3conv {

class BatchRunner {
public:
    static bool IsBatchRequest(const CliOptions& options);
    static std::string RenderPlan(const CliOptions& options, const AppConfig& config, const char* executable_path);
    static int Execute(const CliOptions& options, const AppConfig& config, const char* executable_path);
};

}  // namespace t3conv
