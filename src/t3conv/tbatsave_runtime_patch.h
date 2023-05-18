#pragma once

#include "../common/types.h"

#include <vector>
#include <string>

namespace t3conv {

bool TryInstallTbatsaveRuntimePatch(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
);

bool EnsureTbatsaveRuntimePatchSession(std::vector<std::string>& diagnostics);

std::vector<std::string> ReadTbatsaveRuntimePatchTelemetry();

bool TryRunTbatsaveDirectWorker(
    const ProcessLaunchPlan& plan,
    std::vector<std::string>& diagnostics
);

}  // namespace t3conv
