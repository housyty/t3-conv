#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace t3conv {

enum class ErrorCode : int {
    kSuccess = 0,
    kFileNotFound = 1,
    kFileCorrupt = 2,
    kLoadTimeout = 3,
    kTchObjectMissing = 4,
    kArxLoadFailed = 5,
    kFunctionCallFailed = 6,
    kSaveFailed = 7,
    kOutOfMemory = 8,
    kArgumentError = 9,
    kUnexpectedCrash = 99,
};

enum class TbatsaveBindMode : int {
    kKeepDefault = -1,
    kBind = 0,
    kInsert = 1,
    kCustom = 2,
};

enum class HostControlMode {
    kNone,
    kStart,
    kStatus,
    kStop,
};

struct ConversionPaths {
    std::filesystem::path source_path;
    std::filesystem::path target_path;
    std::filesystem::path log_path;
    std::filesystem::path output_dir;
    std::filesystem::path working_dir;
};

struct ResolvedPaths {
    std::filesystem::path workspace_root;
    std::filesystem::path config_path;
    std::filesystem::path tangent_root;
    std::filesystem::path autocad_root;
    std::filesystem::path tgstart_exe;
    std::filesystem::path tangent_mnl;
    std::filesystem::path runtime_root;
    std::filesystem::path bridge_template_path;
    std::filesystem::path host_template_path;
    std::filesystem::path bridge_runtime_path;
    std::filesystem::path host_runtime_path;
    std::filesystem::path font_map_path;
    std::filesystem::path font_dir;
    std::filesystem::path autocad_fonts_dir;
    std::filesystem::path var_root;
    std::filesystem::path host_dir;
    std::filesystem::path logs_dir;
    std::filesystem::path runtime_dir;
    std::filesystem::path host_ready_path;
    std::filesystem::path host_stop_path;
    std::filesystem::path host_bootstrap_path;
    std::filesystem::path bootstrap_state_path;
    std::filesystem::path worker_status_path;
    std::filesystem::path trigger_log_path;
};

struct AppConfig {
    std::filesystem::path config_path;
    std::filesystem::path tangent_root;
    std::filesystem::path autocad_root;
    std::filesystem::path font_dir;
    std::filesystem::path autocad_fonts_dir;
    std::string font_alt;
    ResolvedPaths resolved;
};

struct CliOptions {
    ConversionPaths paths;
    HostControlMode host_control_mode = HostControlMode::kNone;
    TbatsaveBindMode tbatsave_bind_mode = TbatsaveBindMode::kKeepDefault;
    int tbatsave_bind_ref = -1;
    int timeout_seconds = 120;
    int retries = 1;
    bool overwrite = true;
    bool dry_run = false;
    bool json = false;
    bool debug = false;
};

struct ProcessLaunchPlan {
    HostControlMode host_control_mode = HostControlMode::kNone;
    std::filesystem::path executable;
    std::filesystem::path target_path;
    std::filesystem::path log_path;
    std::filesystem::path script_path;
    std::filesystem::path worker_status_path;
    std::filesystem::path host_ready_path;
    std::filesystem::path host_stop_path;
    std::filesystem::path host_bootstrap_path;
    std::filesystem::path bootstrap_state_path;
    std::filesystem::path host_runtime_script_path;
    std::filesystem::path host_bridge_script_path;
    std::filesystem::path font_map_path;
    std::filesystem::path font_dir;
    std::filesystem::path autocad_fonts_dir;
    std::filesystem::path host_runtime_template_path;
    std::filesystem::path host_bridge_template_path;
    std::filesystem::path host_mnl_path;
    std::filesystem::path trigger_log_path;
    std::filesystem::path config_path;
    std::filesystem::path workspace_root;
    std::filesystem::path var_root;
    std::filesystem::path host_dir;
    std::filesystem::path logs_dir;
    std::filesystem::path runtime_dir;
    std::filesystem::path stage_source_path;
    std::filesystem::path batch_output_dir;
    std::filesystem::path tarch_root;
    std::filesystem::path autocad_root;
    std::filesystem::path working_dir;
    std::vector<std::string> arguments;
    std::string script_contents;
    std::string font_alt;
    TbatsaveBindMode tbatsave_bind_mode = TbatsaveBindMode::kKeepDefault;
    int tbatsave_bind_ref = -1;
    int timeout_seconds = 120;
};

struct ConversionResult {
    bool success = false;
    ErrorCode error_code = ErrorCode::kUnexpectedCrash;
    double elapsed_sec = 0.0;
    std::string error_message;
    std::vector<std::string> diagnostics;
};

}  // namespace t3conv
