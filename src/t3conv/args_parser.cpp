#include "args_parser.h"

#include "../common/utils.h"

#include <filesystem>
#include <string>

namespace t3conv {

namespace {

bool TrySplitOption(const std::string& arg, const std::string& prefix, std::string& value) {
    if (arg.rfind(prefix + "=", 0) != 0) {
        return false;
    }
    value = arg.substr(prefix.size() + 1);
    return true;
}


bool ConsumeValue(
    int argc,
    char** argv,
    int& index,
    const std::string& option_name,
    std::string& value,
    std::string& error_message
) {
    if (index + 1 >= argc || argv[index + 1] == nullptr) {
        error_message = "missing value for option " + option_name;
        return false;
    }
    ++index;
    value = argv[index];
    return true;
}

bool TryParseInteger(
    const std::string& value,
    const std::string& option_name,
    int& out_value,
    std::string& error_message
) {
    try {
        size_t parsed = 0;
        const int number = std::stoi(value, &parsed, 10);
        if (parsed != value.size()) {
            error_message = "invalid integer for option " + option_name + ": " + value;
            return false;
        }
        out_value = number;
        return true;
    } catch (...) {
        error_message = "invalid integer for option " + option_name + ": " + value;
        return false;
    }
}


bool IsValidTbatsaveBindMode(const int value) {
    switch (static_cast<TbatsaveBindMode>(value)) {
        case TbatsaveBindMode::kKeepDefault:
        case TbatsaveBindMode::kBind:
        case TbatsaveBindMode::kInsert:
        case TbatsaveBindMode::kCustom:
            return true;
    }
    return false;
}


bool TryParseTbatsaveBindMode(
    const std::string& value,
    const std::string& option_name,
    TbatsaveBindMode& out_value,
    std::string& error_message
) {
    int bind_mode = 0;
    if (!TryParseInteger(value, option_name, bind_mode, error_message)) {
        return false;
    }
    if (!IsValidTbatsaveBindMode(bind_mode)) {
        error_message =
            "invalid value for option " + option_name + ": " + value +
            " (expected -1, 0, 1, or 2)";
        return false;
    }
    out_value = static_cast<TbatsaveBindMode>(bind_mode);
    return true;
}


bool TryParsePositiveInteger(
    const std::string& value,
    const std::string& option_name,
    int& out_value,
    std::string& error_message
) {
    if (!TryParseInteger(value, option_name, out_value, error_message)) {
        return false;
    }
    if (out_value <= 0) {
        error_message =
            "invalid value for option " + option_name + ": " + value +
            " (expected a positive integer)";
        return false;
    }
    return true;
}


bool RequireAbsolutePath(
    const std::filesystem::path& path,
    const std::string& field_name,
    std::string& error_message
) {
    if (path.empty()) {
        return true;
    }
    if (!path.is_absolute()) {
        error_message = field_name + " path must be absolute";
        return false;
    }
    return true;
}

}  // namespace


ParseResult ParseArgs(int argc, char** argv) {
    ParseResult result;
    std::filesystem::path explicit_target_path;
    std::filesystem::path explicit_log_path;

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index] == nullptr ? std::string() : std::string(argv[index]);
        std::string value;

        if (arg == "-h" || arg == "--help") {
            result.ok = true;
            result.show_help = true;
            return result;
        }

        if (arg == "--json") {
            result.options.json = true;
            continue;
        }
        if (arg == "-d" || arg == "--debug") {
            result.options.debug = true;
            continue;
        }
        if (arg == "--dry-run") {
            result.options.dry_run = true;
            continue;
        }
        if (arg == "--host-start") {
            result.options.host_control_mode = HostControlMode::kStart;
            continue;
        }
        if (arg == "--host-status") {
            result.options.host_control_mode = HostControlMode::kStatus;
            continue;
        }
        if (arg == "--host-stop") {
            result.options.host_control_mode = HostControlMode::kStop;
            continue;
        }
        if (arg == "--no-overwrite") {
            result.options.overwrite = false;
            continue;
        }

        if (arg == "-s" || arg == "--source") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            result.options.paths.source_path = value;
            continue;
        } else if (TrySplitOption(arg, "--source", value)) {
            result.options.paths.source_path = value;
            continue;
        } else if (arg == "-o" || arg == "--target") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            explicit_target_path = value;
            continue;
        } else if (TrySplitOption(arg, "--target", value)) {
            explicit_target_path = value;
            continue;
        } else if (arg == "--output-dir") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            result.options.paths.output_dir = value;
            continue;
        } else if (TrySplitOption(arg, "--output-dir", value)) {
            result.options.paths.output_dir = value;
            continue;
        } else if (arg == "--log") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            explicit_log_path = value;
            continue;
        } else if (TrySplitOption(arg, "--log", value)) {
            explicit_log_path = value;
            continue;
        } else if (arg == "--tbatsave-bindmode") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            if (!TryParseTbatsaveBindMode(
                    value,
                    arg,
                    result.options.tbatsave_bind_mode,
                    result.error_message
                )) {
                return result;
            }
            continue;
        } else if (TrySplitOption(arg, "--tbatsave-bindmode", value)) {
            if (!TryParseTbatsaveBindMode(
                    value,
                    "--tbatsave-bindmode",
                    result.options.tbatsave_bind_mode,
                    result.error_message
                )) {
                return result;
            }
            continue;
        } else if (arg == "--tbatsave-bindref") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            int bind_ref = 0;
            if (!TryParseInteger(value, arg, bind_ref, result.error_message)) {
                return result;
            }
            result.options.tbatsave_bind_ref = bind_ref;
            continue;
        } else if (TrySplitOption(arg, "--tbatsave-bindref", value)) {
            int bind_ref = 0;
            if (!TryParseInteger(value, "--tbatsave-bindref", bind_ref, result.error_message)) {
                return result;
            }
            result.options.tbatsave_bind_ref = bind_ref;
            continue;
        } else if (arg == "--timeout-seconds") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            if (!TryParsePositiveInteger(value, arg, result.options.timeout_seconds, result.error_message)) {
                return result;
            }
            continue;
        } else if (TrySplitOption(arg, "--timeout-seconds", value)) {
            if (!TryParsePositiveInteger(
                    value,
                    "--timeout-seconds",
                    result.options.timeout_seconds,
                    result.error_message
                )) {
                return result;
            }
            continue;
        } else if (arg == "--retries") {
            if (!ConsumeValue(argc, argv, index, arg, value, result.error_message)) {
                return result;
            }
            if (!TryParseInteger(value, arg, result.options.retries, result.error_message)) {
                return result;
            }
            continue;
        } else if (TrySplitOption(arg, "--retries", value)) {
            if (!TryParseInteger(value, "--retries", result.options.retries, result.error_message)) {
                return result;
            }
            continue;
        }

        if (!arg.empty() && arg[0] == '-') {
            result.error_message = "unknown option: " + arg;
            return result;
        }

        if (result.options.paths.source_path.empty()) {
            result.options.paths.source_path = arg;
            continue;
        }

        result.error_message = "unexpected positional argument: " + arg;
        return result;
    }

    if (result.options.host_control_mode == HostControlMode::kNone &&
        result.options.paths.source_path.empty()) {
        result.error_message = "missing source path";
        return result;
    }

    if (!ConfigLoader::Load(result.config, result.error_message)) {
        return result;
    }

    if (!RequireAbsolutePath(result.options.paths.source_path, "source", result.error_message)) {
        return result;
    }
    if (!RequireAbsolutePath(result.options.paths.output_dir, "output_dir", result.error_message)) {
        return result;
    }
    if (!RequireAbsolutePath(explicit_target_path, "target", result.error_message)) {
        return result;
    }
    if (!RequireAbsolutePath(explicit_log_path, "log", result.error_message)) {
        return result;
    }

    if (!result.options.paths.source_path.empty()) {
        result.options.paths.source_path = result.options.paths.source_path.lexically_normal();
    }

    if (!result.options.paths.output_dir.empty()) {
        result.options.paths.output_dir = result.options.paths.output_dir.lexically_normal();
    }

    if (!result.options.paths.source_path.empty()) {
        const bool source_is_directory =
            std::filesystem::is_directory(result.options.paths.source_path);
        if (source_is_directory) {
            if (!explicit_target_path.empty()) {
                result.options.paths.output_dir = explicit_target_path.lexically_normal();
                result.options.paths.target_path.clear();
            } else if (result.options.paths.output_dir.empty()) {
                result.options.paths.output_dir = result.options.paths.source_path;
            }
        } else {
            result.options.paths.target_path = explicit_target_path.empty()
                                                   ? BuildDefaultTargetPath(
                                                         result.options.paths.source_path,
                                                         result.options.paths.output_dir
                                                     )
                                                   : explicit_target_path.lexically_normal();
        }
        if (explicit_log_path.empty()) {
            result.options.paths.log_path = source_is_directory
                                                ? result.config.resolved.workspace_root / "t3conv.log"
                                                : result.config.resolved.workspace_root / "t3conv.log";
        } else {
            result.options.paths.log_path = explicit_log_path.lexically_normal();
        }
        result.options.paths.working_dir = result.config.resolved.workspace_root;
    } else {
        result.options.paths.target_path.clear();
        result.options.paths.log_path.clear();
        result.options.paths.working_dir = result.config.resolved.workspace_root;
    }

    result.ok = true;
    return result;
}


std::string BuildUsage() {
    return
        "Usage: t3conv [options] <source.dwg>\n"
        "  -s <path>, --source <path> source DWG path or source directory; may also be the first positional argument\n"
        "  -o <path>, --target <path> target DWG path or batch output directory\n"
        "  -d, --debug                expand batch stdout/stderr diagnostics\n"
        "  --output-dir <dir>         output directory for derived files\n"
        "  --log <path>               explicit log file path\n"
        "  --timeout-seconds <n>      operation timeout in seconds, default 120\n"
        "  --retries <n>              batch retry count, default 1\n"
        "  --no-overwrite             preserve existing targets\n"
        "  --dry-run                  print launch plan without executing\n"
        "  --json                     print structured JSON-like output\n"
        "  --tbatsave-bindmode <n>    reserved reverse-engineering diagnostic option\n"
        "  --tbatsave-bindref <n>     reserved reverse-engineering diagnostic option\n"
        "  --host-status              inspect whether the background TBatSave host is ready\n"
        "  --host-stop                request the background TBatSave host loop to stop\n"
        "  --host-start               optional: pre-warm or reuse a background TGStart/acad host loop\n";
}

}  // namespace t3conv
