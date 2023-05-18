#pragma once

#include "../common/types.h"

#include <string>

namespace t3conv {

class ConfigLoader {
public:
    static bool Load(AppConfig& config, std::string& error_message);
};

}  // namespace t3conv
