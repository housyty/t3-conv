#pragma once

#include "../common/types.h"

#include "config_loader.h"

#include <string>

namespace t3conv {

struct ParseResult {
    bool ok = false;
    bool show_help = false;
    CliOptions options;
    AppConfig config;
    std::string error_message;
};

ParseResult ParseArgs(int argc, char** argv);
std::string BuildUsage();

}  // namespace t3conv
