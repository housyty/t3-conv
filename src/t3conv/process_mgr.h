#pragma once

#include "../common/types.h"

#include <filesystem>
#include <string>

namespace t3conv {

class ProcessManager {
public:
    static ProcessLaunchPlan BuildLaunchPlan(const CliOptions& options, const AppConfig& config);
    static ConversionResult Execute(const CliOptions& options, const ProcessLaunchPlan& plan);
    static std::string RenderLaunchPlan(const CliOptions& options, const ProcessLaunchPlan& plan);
    static std::string RenderLaunchPlanJson(const CliOptions& options, const ProcessLaunchPlan& plan);
    static size_t StopTianzhengAcadHosts();
};

}  // namespace t3conv
