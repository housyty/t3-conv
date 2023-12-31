import subprocess
import unittest
import os
import tempfile
from pathlib import Path


def norm(path: Path) -> str:
    return str(path)


class CurrentTbatsaveNoUiContractsTests(unittest.TestCase):
    EXE_TIMEOUT_SECONDS = 60

    def setUp(self) -> None:
        self._temp_dir = tempfile.TemporaryDirectory(prefix="t3conv-tests-")
        self.test_temp_root = Path(self._temp_dir.name)
        self.workspace_root = Path(__file__).resolve().parents[1]
        self.build_root = Path(
            os.environ.get(
                "T3CONV_TEST_BUILD_ROOT",
                self.workspace_root.parent / "_build" / "t3-conv-clean",
            )
        )
        self.exe = Path(
            os.environ.get(
                "T3CONV_TEST_EXE",
                self.build_root / "src" / "t3conv" / "Debug" / "t3conv.exe",
            )
        )
        sample_source_env = os.environ.get("T3CONV_TEST_SAMPLE_DWG")
        self.sample_source = Path(sample_source_env) if sample_source_env else self.test_temp_root / "batch-in" / "Drawing1.dwg"
        self.sample_source.parent.mkdir(parents=True, exist_ok=True)
        self.sample_source.touch()
        self.expected_tangent_root = Path(
            os.environ.get(
                "T3CONV_TEST_TANGENT_ROOT",
                self.test_temp_root / "TArchT20V9",
            )
        )
        self.expected_autocad_root = Path(
            os.environ.get(
                "T3CONV_TEST_AUTOCAD_ROOT",
                self.test_temp_root / "AutoCAD 2020",
            )
        )
        (self.expected_tangent_root / "SYS").mkdir(parents=True, exist_ok=True)
        (self.expected_tangent_root / "sys23x64").mkdir(parents=True, exist_ok=True)
        (self.expected_tangent_root / "TGStart.exe").touch()
        (self.expected_tangent_root / "sys23x64" / "tch_kernal.arx").touch()
        (self.expected_autocad_root / "Fonts").mkdir(parents=True, exist_ok=True)

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def run_exe(self, *args: str) -> subprocess.CompletedProcess[str]:
        return self.run_exe_with_env({}, *args)

    def run_exe_with_env(self, env_overrides: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
        command = [str(self.exe), *args]
        env = os.environ.copy()
        env.setdefault("T3CONV_TANGENT_ROOT", str(self.expected_tangent_root))
        env.setdefault("T3CONV_AUTOCAD_ROOT", str(self.expected_autocad_root))
        env.update(env_overrides)
        try:
            return subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                timeout=self.EXE_TIMEOUT_SECONDS,
                env=env,
            )
        except subprocess.TimeoutExpired as exc:
            self.fail(
                "t3conv command timed out after "
                f"{self.EXE_TIMEOUT_SECONDS}s: {' '.join(command)}\n"
                f"stdout={exc.stdout or ''}\n"
                f"stderr={exc.stderr or ''}"
            )

    def test_ini_is_the_single_configuration_entry(self):
        config = self.workspace_root / "t3conv.ini"
        self.assertTrue(config.exists())

        source = config.read_text(encoding="utf-8")
        self.assertIn("[paths]", source)
        self.assertIn("; tangent_root=", source)
        self.assertIn("; autocad_root=", source)
        self.assertIn("[fonts]", source)
        self.assertIn("; fontalt=", source)
        self.assertNotIn("font_dir=", source)

        active_settings = [
            line for line in source.splitlines()
            if line.strip() and not line.lstrip().startswith(";") and "=" in line
        ]
        self.assertEqual([], active_settings)

    def test_tangent_root_auto_detection_checks_common_valid_install_locations(self):
        config_loader = (self.workspace_root / "src/t3conv/config_loader.cpp").read_text(encoding="utf-8")

        self.assertIn("T3CONV_TANGENT_ROOT", config_loader)
        self.assertIn("CommonTangentRootCandidates", config_loader)
        self.assertIn("FindFirstValidTangentRoot", config_loader)
        self.assertIn("IsValidTangentRootCandidate", config_loader)
        self.assertIn("ProgramFiles", config_loader)
        self.assertIn("ProgramFiles(x86)", config_loader)
        self.assertIn("SystemDrive", config_loader)
        self.assertIn('"TGStart.exe"', config_loader)
        self.assertIn('"SYS"', config_loader)
        self.assertIn('"TArchT20V9"', config_loader)
        self.assertIn('"TArchT20V9.0"', config_loader)
        self.assertIn('"T20V9"', config_loader)
        self.assertIn("AddDriveTangentRootCandidates", config_loader)
        self.assertIn("AddWorkspaceTangentCandidates", config_loader)
        self.assertIn("AddDriveRootTangentCandidates", config_loader)
        self.assertIn("AddProgramFilesTangentCandidates", config_loader)
        self.assertLess(
            config_loader.index("AddDriveTangentRootCandidates(candidates, drive_roots)"),
            config_loader.index("AddWorkspaceTangentCandidates(candidates, workspace_root)")
        )
        self.assertLess(
            config_loader.index("AddWorkspaceTangentCandidates(candidates, workspace_root)"),
            config_loader.index("AddDriveRootTangentCandidates(candidates, drive_roots)")
        )
        self.assertLess(
            config_loader.index("AddDriveRootTangentCandidates(candidates, drive_roots)"),
            config_loader.index("AddProgramFilesTangentCandidates(candidates)")
        )
        self.assertNotIn("FindFirstExistingDirectory(candidates)", config_loader)

    def test_missing_autocad_reports_install_error_before_version_mapping_error(self):
        missing_autocad_root = self.test_temp_root / "Missing AutoCAD 2020"
        completed = self.run_exe_with_env(
            {"T3CONV_AUTOCAD_ROOT": str(missing_autocad_root)},
            "--dry-run",
            "-s",
            str(self.sample_source),
        )

        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("AutoCAD is not installed or not found", completed.stderr)
        self.assertNotIn("AutoCAD version is not mapped", completed.stderr)

    def test_help_output_only_mentions_current_flags(self):
        completed = self.run_exe("--help")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Usage: t3conv", completed.stdout)
        self.assertIn("-s <path>", completed.stdout)
        self.assertIn("-o <path>", completed.stdout)
        self.assertIn("-d, --debug", completed.stdout)
        self.assertIn("--retries <n>", completed.stdout)
        self.assertIn("--tbatsave-bindmode", completed.stdout)
        self.assertNotIn("--host-start", completed.stdout)
        self.assertNotIn("--host-stop", completed.stdout)
        self.assertNotIn("--host-status", completed.stdout)
        self.assertLess(completed.stdout.index("--json"), completed.stdout.index("--tbatsave-bindmode"))
        self.assertIn("--dry-run", completed.stdout)
        self.assertNotIn("--runtime", completed.stdout)
        self.assertNotIn("--tbatsave-experimental", completed.stdout)
        self.assertNotIn("--tbatsave-probe", completed.stdout)
        self.assertNotIn("--batch", completed.stdout)
        self.assertNotIn("accoreconsole", completed.stdout.lower())
        self.assertNotIn("--acad <path>", completed.stdout)
        self.assertNotIn("--arx <path>", completed.stdout)

    def test_release_build_uses_static_msvc_runtime(self):
        cmake = (self.workspace_root / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("CMAKE_MSVC_RUNTIME_LIBRARY", cmake)
        self.assertIn("MultiThreaded", cmake)
        self.assertNotIn("MultiThreadedDLL", cmake)

    def test_dry_run_renders_single_tgstart_tbatsave_plan(self):
        completed = self.run_exe("--dry-run", "--source", str(self.sample_source))

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("mode=tgstart_tbatsave_no_ui", completed.stdout)
        self.assertIn(f'command="{norm(self.expected_tangent_root / "TGStart.exe")}"', completed.stdout)
        self.assertNotIn(r"\var\jobs", completed.stdout)
        self.assertNotIn("t3conv_job.txt", completed.stdout)
        self.assertNotIn("t3conv_result.txt", completed.stdout)
        self.assertNotIn("TBATSAVE_EXPERIMENTAL", completed.stdout)
        self.assertIn("host_strategy=tgstart-reuse-or-launch", completed.stdout)
        self.assertIn("reuse_check=acad+tch_kernal", completed.stdout)
        self.assertIn(f"font_dir={norm(self.workspace_root / 'fonts')}", completed.stdout)
        self.assertIn(f"font_map={norm(self.workspace_root / 'var/runtime/fontmap.fmp')}", completed.stdout)
        self.assertNotIn("t3conv_fontmap.fmp", completed.stdout)
        self.assertNotIn("t3-converter", completed.stdout)
        self.assertNotIn("TBatchNoUI", completed.stdout)
        self.assertNotIn("accoreconsole", completed.stdout.lower())
        self.assertNotIn("runtime=", completed.stdout)
        self.assertNotIn("probe_mode=", completed.stdout)
        self.assertIn("timeout_seconds=120", completed.stdout)

    def test_short_source_and_output_flags_select_explicit_target(self):
        explicit_target = self.test_temp_root / "batch-out/FVilla-02_custom_t3.dwg"
        completed = self.run_exe(
            "--dry-run",
            "-s",
            str(self.sample_source),
            "-o",
            str(explicit_target),
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn(f"source={self.sample_source}", completed.stdout)
        self.assertIn(f"target={explicit_target}", completed.stdout)

    def test_short_source_without_output_uses_source_folder_default_target(self):
        completed = self.run_exe("--dry-run", "-s", str(self.sample_source))

        self.assertEqual(completed.returncode, 0, completed.stderr)
        expected_target = self.sample_source.with_name(f"{self.sample_source.stem}_t3{self.sample_source.suffix}")
        self.assertIn(f"target={norm(expected_target)}", completed.stdout)
        self.assertIn(f"log={norm(self.workspace_root / 't3conv.log')}", completed.stdout)
        self.assertIn(f"stage_source={norm(self.sample_source)}", completed.stdout)
        self.assertIn(f"batch_output={norm(self.workspace_root / '_t3conv_work/batch_output')}", completed.stdout)

    def test_directory_source_dry_run_is_batch_parent_without_batch_flag(self):
        source_dir = self.test_temp_root / "batch-in"
        output_dir = self.test_temp_root / "batch-out"
        source_dir.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "Drawing1.dwg").touch()
        completed = self.run_exe(
            "--dry-run",
            "-s",
            str(source_dir),
            "-o",
            str(output_dir),
            "-d",
            "--retries",
            "2",
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("mode=t3conv_batch_parent", completed.stdout)
        self.assertIn(f"source_dir={source_dir}", completed.stdout)
        self.assertIn(f"output_dir={output_dir}", completed.stdout)
        self.assertIn(f"log={norm(self.workspace_root / 't3conv.log')}", completed.stdout)
        self.assertIn("debug=true", completed.stdout)
        self.assertIn("retries=2", completed.stdout)
        self.assertIn('child_command=', completed.stdout)
        self.assertNotIn("--batch", completed.stdout)

    def test_directory_source_child_command_forwards_conversion_options(self):
        source_dir = self.test_temp_root / "batch-in"
        output_dir = self.test_temp_root / "batch-out"
        source_dir.mkdir(parents=True, exist_ok=True)
        output_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "Drawing1.dwg").touch()
        completed = self.run_exe(
            "--dry-run",
            "-s",
            str(source_dir),
            "-o",
            str(output_dir),
            "--no-overwrite",
            "--tbatsave-bindmode",
            "1",
            "--tbatsave-bindref",
            "2",
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("child_command=", completed.stdout)
        self.assertIn("--no-overwrite", completed.stdout)
        self.assertIn("--tbatsave-bindmode 1", completed.stdout)
        self.assertIn("--tbatsave-bindref 2", completed.stdout)

    def test_invalid_integer_options_return_argument_error_without_crashing(self):
        for args in [
            ("--dry-run", "--timeout-seconds", "abc", "-s", str(self.sample_source)),
            ("--dry-run", "--tbatsave-bindmode", "99", "-s", str(self.sample_source)),
        ]:
            with self.subTest(args=args):
                completed = self.run_exe(*args)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("argument_error:", completed.stderr)
                self.assertIn("Usage: t3conv", completed.stderr)
                self.assertNotIn("terminate called", completed.stderr.lower())

    def test_single_file_real_run_defaults_to_compact_output_and_log_contract(self):
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")
        args_parser = (self.workspace_root / "src/t3conv/args_parser.cpp").read_text(encoding="utf-8")
        tests = (self.workspace_root / "tests/test_cpp_scaffold_contracts.py").read_text(encoding="utf-8")

        self.assertEqual(self.EXE_TIMEOUT_SECONDS, 60)
        self.assertLess(main.index("if (parse_result.options.dry_run)"), main.index("ProcessManager::Execute"))
        self.assertIn("WriteSingleConversionLog", main)
        self.assertIn("PrintSingleConversionRecord", main)
        self.assertIn("BuildSingleCopyableCommand", main)
        self.assertIn("JoinLogCommandLine", main)
        self.assertNotIn("JoinCopyableCommandLine", main)
        self.assertNotIn("NeedsCommandQuotes", main)
        self.assertIn("JoinLogCommandLine", (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8"))
        self.assertIn("timeout=self.EXE_TIMEOUT_SECONDS", tests)
        self.assertIn("CleanupWorkingDirectory", (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8"))
        self.assertIn("if (parse_result.options.debug)", main)
        self.assertNotIn('stream << "log=" << log_path.string()', main)
        self.assertIn("std::filesystem::remove_all(plan.working_dir", (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8"))
        self.assertIn('"name="', main)
        self.assertIn('"source="', main)
        self.assertIn('"output="', main)
        self.assertIn('"started_at="', main)
        self.assertIn('"duration="', main)
        self.assertIn('"size="', main)
        self.assertIn('"error="', main)
        self.assertIn('"t3conv.log"', args_parser)
        self.assertIn("result.config.resolved.workspace_root / \"t3conv.log\"", args_parser)

    def test_real_conversion_self_elevates_without_java_caller_changes(self):
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")
        cmake = (self.workspace_root / "src/t3conv/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("IsCurrentProcessElevated", main)
        self.assertIn("RelaunchElevatedAndWait", main)
        self.assertIn("ShellExecuteExW", main)
        self.assertIn("shell_execute_info.lpVerb = L\"runas\"", main)
        self.assertIn("SEE_MASK_NOCLOSEPROCESS", main)
        self.assertIn("shell_execute_info.nShow = SW_HIDE", main)
        self.assertIn("WaitForSingleObject(shell_execute_info.hProcess, INFINITE)", main)
        self.assertIn("GetExitCodeProcess(shell_execute_info.hProcess", main)
        self.assertIn("OpenProcessToken(GetCurrentProcess()", main)
        self.assertIn("CheckTokenMembership", main)
        self.assertIn("T3CONV_ELEVATION_ATTEMPTED", main)
        self.assertIn("--internal-stdout", main)
        self.assertIn("--internal-stderr", main)
        self.assertIn("--internal-tangent-root", main)
        self.assertIn("--internal-autocad-root", main)
        self.assertIn("RedirectInternalOutputFiles(parse_result)", main)
        self.assertIn("ReplayAndRemoveInternalOutput", main)
        self.assertIn("ShouldSelfElevateForConversion(parse_result)", main)
        self.assertLess(
            main.index("if (parse_result.show_help)"),
            main.index("ShouldSelfElevateForConversion(parse_result)")
        )
        self.assertLess(
            main.index("if (parse_result.options.dry_run)"),
            main.index("ProcessManager::Execute")
        )
        self.assertLess(
            main.index("ShouldSelfElevateForConversion(parse_result)"),
            main.index("BatchRunner::Execute")
        )
        self.assertLess(
            main.index("ShouldSelfElevateForConversion(parse_result)"),
            main.index("ProcessManager::Execute")
        )
        self.assertIn("shell32", cmake)
        self.assertIn("advapi32", cmake)
        self.assertNotIn("MANIFESTUAC:level='requireAdministrator'", cmake)

    def test_single_and_batch_logs_roll_under_log_folder_with_fixed_retention(self):
        utils_h = (self.workspace_root / "src/common/utils.h").read_text(encoding="utf-8")
        utils_cpp = (self.workspace_root / "src/common/utils.cpp").read_text(encoding="utf-8")
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")
        batch = (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8")

        self.assertIn("PrepareAppendLogPath", utils_h)
        self.assertIn("PrepareAppendLogPath", utils_cpp)
        self.assertIn("kLogRollBytes = 20ULL * 1024ULL * 1024ULL", utils_cpp)
        self.assertIn("kMaxRolledLogs = 20", utils_cpp)
        self.assertIn('base_log_path.parent_path() / "log"', utils_cpp)
        self.assertIn("RemoveRolledLogs", utils_cpp)
        self.assertIn("PrepareAppendLogPath(log_path)", main)
        self.assertIn("PrepareAppendLogPath(options.paths.log_path)", batch)
        self.assertNotIn('output_dir / ("t3conv.log." + std::to_string(index))', batch)

    def test_json_dry_run_renders_single_mode(self):
        completed = self.run_exe("--dry-run", "--json", "--source", str(self.sample_source))

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn('"mode": "tgstart_tbatsave_no_ui"', completed.stdout)
        self.assertIn('"host_strategy": "tgstart-reuse-or-launch"', completed.stdout)
        self.assertIn('"reuse_check": "acad+tch_kernal"', completed.stdout)
        self.assertIn('"script_contents": ""', completed.stdout)
        self.assertNotIn('"runtime"', completed.stdout)
        self.assertNotIn('"invocation"', completed.stdout)
        self.assertNotIn('"probe_mode"', completed.stdout)

    def test_host_control_is_not_public_cli(self):
        args_parser = (self.workspace_root / "src/t3conv/args_parser.cpp").read_text(encoding="utf-8")
        types = (self.workspace_root / "src/common/types.h").read_text(encoding="utf-8")
        readme = (self.workspace_root / "README.md").read_text(encoding="utf-8")
        readme_zh = (self.workspace_root / "README.zh-CN.md").read_text(encoding="utf-8")

        for text in [args_parser, types, readme, readme_zh]:
            self.assertNotIn("--host-start", text)
            self.assertNotIn("--host-stop", text)
            self.assertNotIn("--host-status", text)
        self.assertNotIn("HostControlMode::kStart", args_parser)
        self.assertNotIn("HostControlMode::kStop", args_parser)
        self.assertNotIn("HostControlMode::kStatus", args_parser)
        self.assertNotIn("kStart", types)
        self.assertNotIn("kStop", types)
        self.assertNotIn("kStatus", types)

    def test_source_directory_and_build_path_use_t3conv_name(self):
        self.assertTrue((self.workspace_root / "src/t3conv").exists())
        self.assertFalse((self.workspace_root / "src/t3conv_exe").exists())

        cmake = (self.workspace_root / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("add_subdirectory(src/t3conv)", cmake)
        self.assertNotIn("src/t3conv_exe", cmake)
        self.assertFalse((self.workspace_root / "scripts/t3conv_batch.ps1").exists())

    def test_runtime_state_does_not_keep_legacy_job_protocol(self):
        types = (self.workspace_root / "src/common/types.h").read_text(encoding="utf-8")
        config_loader = (self.workspace_root / "src/t3conv/config_loader.cpp").read_text(encoding="utf-8")
        process_mgr = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        docs = "\n".join(path.read_text(encoding="utf-8", errors="ignore") for path in (self.workspace_root / "docs").glob("*.md"))

        for text in [types, config_loader, process_mgr, docs]:
            self.assertNotIn("t3conv_job", text)
            self.assertNotIn("t3conv_result", text)
            self.assertNotIn("jobs_dir", text)
            self.assertNotIn("patch_ready", text)
        self.assertFalse((self.workspace_root / "var/jobs").exists())

    def test_process_manager_self_heals_minimal_tangent_mnl_bridge(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("EnsureTbatsaveHostStartupHook", source)
        self.assertIn("NormalizeTbatsaveHostStartupHook", source)
        self.assertIn("EnsureTbatsaveStartupState", source)
        self.assertIn("tangent.mnl", source)
        self.assertIn("tangent_mnl_bridge.runtime.lsp", source)
        self.assertIn("IsManagedTbatsaveBridgeLoadLine", source)
        self.assertIn("tangent_mnl_bridge.lsp", source)
        self.assertIn("host_hook=normalized_mnl", source)
        self.assertIn("host_hook=already_normalized", source)
        self.assertIn("host_hook=mnl_created", source)
        self.assertNotIn('diagnostics.push_back("host_hook=mnl_missing");\n        return false;', source)

    def test_tangent_mnl_trusts_runtime_directory_before_loading_bridge(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        startup_body = source[source.index("bool EnsureTbatsaveHostStartupHook"):]
        startup_body = startup_body[:startup_body.index("bool EnsureTbatsaveStartupState")]

        self.assertIn("BuildTrustedPathsBootstrapLine", source)
        self.assertIn("TRUSTEDPATHS", source)
        self.assertIn("SECURELOAD", source)
        self.assertLess(startup_body.index("trusted_paths_line"), startup_body.index("bridge_load_line"))

    def test_conversion_startup_self_check_repairs_host_markers(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        execute_body = source[source.index("ConversionResult ProcessManager::Execute"):]

        self.assertIn("EnsureTbatsaveStartupState(plan, pre_launch_diagnostics)", source)
        self.assertIn("RemoveFileIfExists(plan.host_ready_path)", source)
        self.assertIn("RemoveFileIfExists(plan.worker_status_path)", source)
        self.assertIn("WriteTextFile(plan.host_bootstrap_path, kTbatsaveHostBootstrapMarker)", source)
        self.assertIn("host_startup=bootstrap_written", source)
        self.assertLess(
            execute_body.index("EnsureTbatsaveStartupState(plan, pre_launch_diagnostics)"),
            execute_body.index("if (IsTbatsaveHostReady(plan))")
        )

    def test_conversion_startup_self_check_disables_tianzheng_start_dialog(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        startup_body = source[source.index("bool EnsureTbatsaveStartupState"):]
        startup_body = startup_body[:startup_body.index("std::optional<uintptr_t> FindModuleBaseAddress")]

        self.assertIn("EnsureTianzhengStartupDialogDisabled(plan, diagnostics)", startup_body)
        self.assertIn("NormalizeTianzhengStartupIni", source)
        self.assertIn("EnsureIniSection", source)
        self.assertIn('ReadWholeFile(ini_path).value_or("")', source)
        self.assertIn("ShowStartDlg=0", source)
        self.assertIn('"Startup"', source)
        self.assertIn('"tch.ini"', source)
        self.assertIn('lines.push_back("[" + section + "]")', source)
        self.assertIn('lines.push_back("ShowStartDlg=0")', source)
        self.assertIn('lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), "ShowStartDlg=0")', source)
        self.assertIn("host_startup_dialog=already_disabled", source)
        self.assertIn("host_startup_dialog=disabled", source)
        self.assertIn("host_startup_dialog=write_failed", source)
        self.assertLess(
            startup_body.index("EnsureTianzhengStartupDialogDisabled(plan, diagnostics)"),
            startup_body.index("WriteTextFile(plan.host_bootstrap_path")
        )

    def test_conversion_launch_waits_for_live_tianzheng_host_not_only_ready_file(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("WaitForTbatsaveHostReady(plan, start_time, host_deadline, post_launch_diagnostics)", source)
        self.assertIn("host_ready_file=present_without_tianzheng_process", source)
        self.assertIn("host_ready_file=present_with_tianzheng_process", source)
        self.assertIn("CountAcadProcessesByName", source)
        self.assertIn("HasAcadProcessDenyingDirectWorkerAccess", source)
        self.assertIn("host_process_access=denied", source)
        self.assertNotIn("while (std::chrono::steady_clock::now() < host_deadline) {\n        if (IsTbatsaveHostReady(plan))", source)

    def test_startup_bridge_immediately_attempts_runtime_load(self):
        bridge = (self.workspace_root / "runtime/tgstart_host/tangent_mnl_bridge.lsp").read_text(
            encoding="utf-8"
        )

        self.assertIn("t3conv:tbx-host-loader", bridge)
        self.assertIn("TBatSave tangent.mnl bridge immediate self-check", bridge)
        self.assertIn("(t3conv:tbx-host-loader)", bridge)
        self.assertLess(
            bridge.rindex("TBatSave tangent.mnl bridge immediate self-check"),
            bridge.rindex("(t3conv:tbx-host-loader)")
        )

    def test_process_manager_uses_direct_worker_without_ui_command_fallback(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("TryRunTbatsaveDirectWorker(plan, post_launch_diagnostics)", source)
        self.assertIn("host_action=tbatsave_direct_worker_succeeded", source)
        self.assertIn("host_action=tbatsave_direct_worker_failed_no_ui_fallback", source)
        self.assertNotIn("host_action=tbatsave_direct_worker_fallback_lsp", source)
        self.assertNotIn("DispatchRunningAcadSendCommandAsync", source)
        self.assertNotIn("host_action=ready_host_sendcommand_triggered", source)

    def test_process_manager_recovers_hung_tianzheng_acad_before_reuse(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("EnumTianzhengAcadProcessIds", source)
        self.assertIn("IsHungAppWindow", source)
        self.assertIn("KillTianzhengAcadHosts", source)
        self.assertIn("RecoverHungTianzhengAcadHosts", source)
        self.assertIn("host_recovery=hung_tianzheng_acad_killed", source)
        self.assertIn("host_action=launch_tgstart_tbatsave_after_hung_recovery", source)
        self.assertIn("host_action=launch_tgstart_host_ready", source)
        self.assertIn("RemoveFileIfExists(plan.worker_status_path)", source)

    def test_process_manager_restarts_non_ready_tianzheng_host(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("RestartNonReadyTianzhengAcadHost", source)
        self.assertIn("host_startup=non_ready_tianzheng_acad_restarted", source)
        self.assertIn("host_action=launch_tgstart_tbatsave_after_non_ready_recovery", source)
        self.assertNotIn("host_action=reuse_acad_tbatsave", source)

    def test_runtime_patch_exposes_verified_direct_worker_contract(self):
        header = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.h").read_text(encoding="utf-8")
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")
        types = (self.workspace_root / "src/common/types.h").read_text(encoding="utf-8")
        process_mgr = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("TryRunTbatsaveDirectWorker", header)
        self.assertIn("TryRunTbatsaveDirectWorker", source)
        self.assertIn("tbatsave_direct_worker=started", source)
        self.assertIn("tbatsave_direct_worker=readDwgFile", source)
        self.assertIn("tbatsave_direct_worker=SaveAsTArch3", source)
        self.assertIn("tbatsave_telemetry_worker_t3_hit_count=", source)
        self.assertIn("tbatsave_telemetry_worker_general_hit_count=", source)
        self.assertIn("int timeout_seconds = 120;", types)
        self.assertIn("plan.timeout_seconds = options.timeout_seconds;", process_mgr)
        self.assertIn("plan.timeout_seconds", source)
        self.assertNotIn("kTbatsaveDirectWorkerWaitMilliseconds = 60000", source)

    def test_direct_worker_selects_tianzheng_acad_process(self):
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")
        worker_body = source[source.index("bool TryRunTbatsaveDirectWorker"):]
        worker_body = worker_body[:worker_body.index("bool EnsureTbatsaveRuntimePatchSession")]

        self.assertIn("FindNewestTianzhengAcadProcess", source)
        self.assertIn("CountAcadProcesses()", source)
        self.assertIn("CountAcadProcessesDenyingDirectWorkerAccess()", source)
        self.assertIn("tbatsave_direct_worker=acad_access_denied", source)
        self.assertIn("tbatsave_direct_worker_hint=run_t3conv_elevated", source)
        self.assertIn("tbatsave_direct_worker=acad_not_found", source)
        self.assertIn("tbatsave_direct_worker=tch_kernal_not_loaded", source)
        self.assertIn("tbatsave_direct_worker_selected_pid=", worker_body)
        self.assertIn("tbatsave_direct_worker_tch_kernal=", worker_body)
        self.assertNotIn('FindNewestProcessIdByName(L"acad.exe")', worker_body)

    def test_runtime_patch_keeps_verified_direct_worker_rvas(self):
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")

        for token in [
            "0x027310",
            "0x64A8D0",
            "0x64A912",
            "0x64A8D6",
            "0x01B310",
            "0x01E850",
            "0x9E6C7C",
        ]:
            self.assertIn(token, source)

    def test_direct_worker_uses_versioned_tch_kernal_runtime_layouts(self):
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")
        worker_body = source[source.index("bool TryRunTbatsaveDirectWorker"):]
        worker_body = worker_body[:worker_body.index("bool EnsureTbatsaveRuntimePatchSession")]

        self.assertIn("struct TchKernelRuntimeLayout", source)
        self.assertIn("kTchKernelSys23x64Layout", source)
        self.assertIn("kTchKernelSys24x64Layout", source)
        self.assertIn("ResolveTchKernelRuntimeLayout", source)
        self.assertIn("ValidateTchKernelRuntimeLayout", source)
        self.assertIn("tbatsave_direct_worker=unsupported_tch_kernal_layout", source)
        self.assertIn("tbatsave_direct_worker=layout_signature_mismatch", source)
        self.assertIn("tbatsave_direct_worker_layout=", worker_body)
        self.assertIn("BuildTbatsaveDirectWorkerStub(", worker_body)
        self.assertIn("*layout", worker_body)

    def test_autocad_2021_direct_worker_layout_does_not_reuse_sys23_rvas(self):
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")

        def extract_layout(name: str) -> str:
            marker = f"constexpr TchKernelRuntimeLayout {name}"
            self.assertIn(marker, source)
            start = source.index(marker)
            end = source.index("};", start)
            return source[start:end]

        sys23 = extract_layout("kTchKernelSys23x64Layout")
        sys24 = extract_layout("kTchKernelSys24x64Layout")

        for field in [
            "acdb_database_ctor_rva",
            "acdb_database_dtor_rva",
            "acdb_database_read_dwg_file_rva",
            "preprocess_groups_rva",
            "preprocess_blocks_rva",
            "save_as_tarch3_rva",
            "selector_base_rva",
            "non_ui_flag_rva",
            "ui_state_flag_rva",
        ]:
            self.assertIn(field, sys23)
            self.assertIn(field, sys24)

        self.assertNotIn("0x027310", sys24)
        self.assertNotIn("0x64A8D0", sys24)
        self.assertNotIn("0x64A912", sys24)
        self.assertNotIn("0x9E6C7C", sys24)

    def test_runtime_templates_use_placeholders_not_absolute_paths(self):
        bridge = (self.workspace_root / "runtime/tgstart_host/tangent_mnl_bridge.lsp").read_text(
            encoding="utf-8"
        )
        trigger = (self.workspace_root / "runtime/tgstart_host/tbatsave_experimental_trigger.lsp").read_text(
            encoding="utf-8"
        )

        self.assertIn("__TBX_HOST_READY__", bridge)
        self.assertIn("__TBX_HOST_RUNTIME_SCRIPT__", bridge)
        self.assertIn("__TBX_FONT_MAP__", trigger)
        self.assertIn("__TBX_FONT_ALT__", trigger)
        self.assertIn("__TBX_FONT_SEARCH_PATH__", trigger)
        self.assertIn('"cmdecho" 0', trigger)
        self.assertIn('"isavebak" 0', trigger)
        self.assertIn("DownGradeSaveFlag", trigger)
        self.assertIn("g_bSaveProxyGraph", trigger)
        self.assertNotIn("C:/Tangent", bridge)
        self.assertNotIn("C:/Tangent", trigger)

    def test_startup_bridge_does_not_auto_run_tbatsave_job(self):
        bridge = (self.workspace_root / "runtime/tgstart_host/tangent_mnl_bridge.lsp").read_text(
            encoding="utf-8"
        )
        trigger = (self.workspace_root / "runtime/tgstart_host/tbatsave_experimental_trigger.lsp").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("__TBX_JOB_PATH__", bridge)
        self.assertNotIn("__TBX_JOB_PATH__", trigger)
        self.assertNotIn("__TBX_RESULT_PATH__", trigger)
        self.assertNotIn('(if (findfile *tbx-job-path*)', trigger)
        self.assertIn("_tbx-start-host-loop", trigger)
        self.assertNotIn("TBatSaveFolder", trigger)
        self.assertNotIn("TBatSave-num", trigger)
        self.assertNotIn("PLDC", trigger)

    def test_host_runtime_waits_for_tch_kernal_before_ready_marker(self):
        trigger = (self.workspace_root / "runtime/tgstart_host/tbatsave_experimental_trigger.lsp").read_text(
            encoding="utf-8"
        )

        self.assertIn("_tbx-tch-kernal-loaded-p", trigger)
        self.assertIn("_tbx-wait-for-tch-kernal", trigger)
        self.assertIn("tch_kernal.arx", trigger)
        self.assertIn("host ready deferred until tch_kernal", trigger)
        self.assertLess(
            trigger.index("(_tbx-wait-for-tch-kernal)"),
            trigger.index("(_tbx-write-host-ready)")
        )

    def test_host_runtime_marks_ready_and_returns_without_blocking_tianzheng_ui(self):
        trigger = (self.workspace_root / "runtime/tgstart_host/tbatsave_experimental_trigger.lsp").read_text(
            encoding="utf-8"
        )

        self.assertIn("host ready; returning control to AutoCAD", trigger)
        self.assertIn("_tbx-host-stop-requested-p", trigger)
        self.assertNotIn("(while (not (_tbx-host-stop-requested-p))", trigger)
        self.assertNotIn("host loop stop marker detected", trigger)
        self.assertNotIn("_tbx-host-keepalive-requested-p", trigger)
        self.assertNotIn("host loop skipped for one-shot startup", trigger)
        self.assertLess(
            trigger.index("(_tbx-write-host-ready)"),
            trigger.index("host ready; returning control to AutoCAD")
        )

    def test_ordinary_conversion_cleanup_distinguishes_permission_from_host_faults(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        ordinary_start = source.index("if (!EnsureTbatsaveStartupState(plan, pre_launch_diagnostics))")
        ordinary_body = source[ordinary_start:source.index("std::string ProcessManager::RenderLaunchPlan", ordinary_start)]

        self.assertIn("KillAllAcadProcesses", source)
        self.assertIn("RequestTbatsaveHostStop", source)
        self.assertIn("ShouldPreserveAcadAfterPermissionDenied", source)
        self.assertIn("host_cleanup=kill_all_acad_on_problem", source)
        self.assertIn("host_cleanup=stop_requested_after_permission_denied", source)
        self.assertIn("host_cleanup_killed_count=", source)
        self.assertGreaterEqual(ordinary_body.count("KillAllAcadProcesses("), 3)
        self.assertIn("if (ShouldPreserveAcadAfterPermissionDenied(post_launch_diagnostics))", ordinary_body)
        self.assertNotIn("} else {\n        KillAllAcadProcesses(post_launch_diagnostics);\n    }", ordinary_body)
        self.assertLess(
            ordinary_body.index("RequestTbatsaveHostStop(plan, post_launch_diagnostics)"),
            ordinary_body.index("BuildDirectWorkerNoUiFallbackFailureResult")
        )

    def test_batch_runner_is_integrated_into_t3conv_exe(self):
        cmake = (self.workspace_root / "src/t3conv/CMakeLists.txt").read_text(encoding="utf-8")
        header = (self.workspace_root / "src/t3conv/batch_runner.h").read_text(encoding="utf-8")
        source = (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8")
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")

        self.assertIn("batch_runner.cpp", cmake)
        self.assertNotIn("MANIFESTUAC:level='requireAdministrator'", cmake)
        self.assertIn("class BatchRunner", header)
        self.assertIn("IsBatchRequest", header)
        self.assertIn("BatchRunner::Execute", source)
        self.assertIn("BatchRunner::RenderPlan", source)
        self.assertIn("BatchRunner::IsBatchRequest", main)
        self.assertIn("BatchRunner::Execute", main)
        self.assertIn("options.paths.log_path", source)
        self.assertIn("PrepareAppendLogPath", source)
        self.assertNotIn("t3conv_batch_{0:000}.log", source)
        self.assertIn("options.debug", source)
        self.assertNotIn("taskkill /F /IM", source)
        self.assertNotIn('{"acad.exe", "TGStart.exe"}', source)
        self.assertIn("StopTianzhengAcadHosts", source)
        self.assertIn('"name="', source)
        self.assertIn('"output="', source)
        self.assertIn('"status="', source)
        self.assertIn('"started_at="', source)
        self.assertIn('"duration="', source)
        self.assertIn('"size="', source)
        self.assertIn("--------------------------------------------------------------------------------", source)
        self.assertIn("internal_status=", source)
        self.assertIn("target_bytes=", source)
        self.assertIn("--- debug stdout ---", source)
        self.assertIn("--- debug stderr ---", source)
        self.assertIn("StopTianzhengAcad", source)
        self.assertIn("ShouldRestartHostAfterFailure", source)
        self.assertIn("ChildProducedRecord", source)
        self.assertIn("#include <thread>", source)
        self.assertIn("stdout_reader", source)
        self.assertIn("stderr_reader", source)
        self.assertLess(source.index("std::thread stdout_reader"), source.index("WaitForSingleObject(process_info.hProcess"))
        self.assertIn("internal_status.empty()", source)
        self.assertIn("row.success = !child.timed_out", source)
        self.assertIn("last_row.log_path = options.paths.log_path", source)
        self.assertIn("if (!ChildProducedRecord(child))", source)
        self.assertLess(source.index("if (!ChildProducedRecord(child))"), source.index("WriteBatchLog(options, copyable, last_row, child)"))
        self.assertIn("AppendBatchSummaryToLog", source)
        self.assertIn('"batch_summary"', source)
        self.assertIn('"total="', source)
        self.assertIn('"success="', source)
        self.assertIn('"failed="', source)
        self.assertIn('"total_seconds="', source)
        self.assertIn('"avg_seconds="', source)
        self.assertNotIn("t3conv_batch_stats.csv", source)
        self.assertNotIn("t3conv_batch_summary.txt", source)
        self.assertNotIn("stats_csv=", source)
        self.assertNotIn("_t3conv_stdout", source)
        self.assertNotIn("_t3conv_stderr", source)
        self.assertNotIn("C:\\Program Files\\Autodesk\\AutoCAD 2020", source)
        self.assertNotIn("C:\\Tangent\\TArchT20V9", source)

    def test_batch_runner_caps_host_restarts_per_run(self):
        source = (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8")

        self.assertIn("kMaxHostRestartsPerBatch = 5", source)
        self.assertIn("host_restart_count", source)
        self.assertIn("host_restart_limit=", source)
        self.assertIn("host_restart_limit_exceeded=true", source)
        self.assertIn("batch_aborted=", source)
        self.assertIn("if (host_restart_count >= kMaxHostRestartsPerBatch)", source)
        self.assertLess(
            source.index("if (host_restart_count >= kMaxHostRestartsPerBatch)"),
            source.index("StopTianzhengAcad();")
        )
        self.assertNotIn("--max-host-restarts", source)

    def test_periodic_host_restart_runs_after_fifty_successful_conversions(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("kPeriodicHostRestartInterval = 50;", source)
        self.assertNotIn("kPeriodicHostRestartInterval = 5;", source)
        self.assertIn("conversion_count=", source)
        self.assertIn("host_periodic_restart=killed", source)
        self.assertNotIn("host_restarting", source)
        self.assertNotIn("CreateThread", source)

    def test_real_conversions_are_serialized_by_tangent_root_mutex(self):
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")
        args_parser = (self.workspace_root / "src/t3conv/args_parser.cpp").read_text(encoding="utf-8")

        self.assertIn("BuildConversionMutexName", main)
        self.assertIn("T3CONV_CONVERSION_LOCK_HELD", main)
        self.assertIn("CreateMutexW", main)
        self.assertIn("WaitForSingleObject(handle_, INFINITE)", main)
        self.assertIn("ReleaseMutex(handle_)", main)
        self.assertIn("parse_result.config.resolved.tangent_root", main)
        self.assertIn("Acquire(parse_result.config.resolved.tangent_root)", main)
        self.assertLess(
            main.index("ShouldSelfElevateForConversion(parse_result)"),
            main.index("Acquire(parse_result.config.resolved.tangent_root)")
        )
        self.assertLess(
            main.index("Acquire(parse_result.config.resolved.tangent_root)"),
            main.index("BatchRunner::Execute")
        )
        self.assertLess(
            main.index("Acquire(parse_result.config.resolved.tangent_root)"),
            main.index("ProcessManager::Execute")
        )
        self.assertNotIn("--lock", args_parser)
        self.assertNotIn("--mutex", args_parser)

    def test_font_logic_syncs_only_project_fonts_to_autocad_fonts(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("BuildTbatsaveFontMapContents", source)
        self.assertIn("SyncProjectFontsToAutocadFonts", source)
        self.assertIn("ManagedAutocadFontTarget", source)
        self.assertIn("font_sync=copied", source)
        self.assertIn("font_sync=skipped_existing", source)
        self.assertIn("font_sync=skipped_conflict", source)
        self.assertIn("font_sync=conflict_copied_as", source)
        self.assertIn('"fontmap.fmp"', source)
        self.assertNotIn('"t3conv_fontmap.fmp"', source)
        self.assertNotIn("paths.push_back(plan.font_dir)", source)
        self.assertNotIn('paths.push_back(plan.tarch_root / "SYS")', source)

    def test_direct_worker_relocation_creates_target_parent_directory(self):
        source = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")

        self.assertIn("EnsureDirectory(plan.target_path.parent_path())", source)
        self.assertLess(
            source.index("EnsureDirectory(plan.target_path.parent_path())"),
            source.index("std::filesystem::rename(batch_target, plan.target_path")
        )

    def test_config_loader_derives_paths_from_ini(self):
        source = (self.workspace_root / "src/t3conv/config_loader.cpp").read_text(encoding="utf-8")

        self.assertLess(
            source.index("FindWorkspaceRootFrom(CurrentExecutableDirectory())"),
            source.index("FindWorkspaceRootFrom(std::filesystem::current_path())")
        )
        self.assertIn("ResolveConfiguredOrDetectedTangentRoot", source)
        self.assertIn("ResolveConfiguredOrDetectedAutocadRoot", source)
        self.assertIn("ReadIniValue(config_path, \"fonts\", \"fontalt\")", source)
        self.assertIn("BuildResolvedPaths", source)
        self.assertIn("TGStart.exe", source)
        self.assertIn("tangent.mnl", source)
        self.assertIn("ValidateInstallRoots", source)
        self.assertIn('"T3CONV_TANGENT_ROOT"', source)
        self.assertIn('"T3CONV_AUTOCAD_ROOT"', source)
        self.assertNotIn('"T3CONV_FONT_DIR"', source)
        self.assertIn('"t3-conv"', source)
        self.assertNotIn('"t3-converter"', source)

    def test_autocad_year_selects_matching_tianzheng_sys_directory(self):
        config_loader = (self.workspace_root / "src/t3conv/config_loader.cpp").read_text(encoding="utf-8")
        types = (self.workspace_root / "src/common/types.h").read_text(encoding="utf-8")
        process_mgr = (self.workspace_root / "src/t3conv/process_mgr.cpp").read_text(encoding="utf-8")
        docs = (self.workspace_root / "docs/t3conv-developer-guide.md").read_text(encoding="utf-8")
        protocol = (self.workspace_root / "docs/tbatsave-direct-worker-protocol.md").read_text(encoding="utf-8")

        self.assertIn("tangent_sys_dir", types)
        self.assertIn("BuildTangentSysDirectoryNameForAutocadYear", config_loader)
        self.assertIn("ResolveTangentSysDir", config_loader)
        self.assertIn("DetectAutocadYearFromRoot", config_loader)
        self.assertIn('case 2020:\n            return "sys23x64";', config_loader)
        self.assertIn('case 2021:\n            return "sys24x64";', config_loader)
        self.assertIn('candidate / "tch_kernal.arx"', config_loader)
        self.assertIn("paths.tangent_sys_dir = tangent_sys_dir;", config_loader)
        self.assertIn('paths.tangent_mnl = tangent_root / "SYS" / "tangent.mnl";', config_loader)
        self.assertIn("plan.tangent_sys_dir = resolved.tangent_sys_dir;", process_mgr)
        self.assertIn("plan.tangent_sys_dir / \"HZTXT.SHX\"", process_mgr)
        self.assertIn("tangent_sys_dir=", process_mgr)
        self.assertIn('\\"tangent_sys_dir\\"', process_mgr)
        self.assertIn("--internal-tangent-sys-dir", (self.workspace_root / "src/t3conv/args_parser.cpp").read_text(encoding="utf-8"))
        self.assertIn("config.resolved.tangent_sys_dir.string()", (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8"))
        patcher = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")
        self.assertIn('plan.tangent_sys_dir / "tch_kernal.arx"', patcher)
        self.assertIn("tbatsave_direct_worker_tch_kernal_path=", patcher)
        self.assertIn("SamePathCaseInsensitive", patcher)
        self.assertIn("AutoCAD 2020 -> `sys23x64`", docs)
        self.assertIn("AutoCAD 2021 -> `sys24x64`", docs)
        self.assertIn("AutoCAD 2020 -> `sys23x64`", protocol)
        self.assertIn("AutoCAD 2021 -> `sys24x64`", protocol)

    def test_packaging_script_creates_portable_zip_without_generated_state(self):
        script = self.workspace_root / "tools/package.ps1"
        self.assertTrue(script.exists())
        source = script.read_text(encoding="utf-8")

        self.assertIn('ZipName = "t3-conv.zip"', source)
        self.assertIn('Join-Path $ProjectRoot "release"', source)
        self.assertNotIn('Join-Path $ProjectParent "dist"', source)
        self.assertIn("t3conv.exe", source)
        self.assertIn("t3conv.ini", source)
        self.assertIn("runtime", source)
        self.assertIn("fonts", source)
        self.assertIn("Compress-Archive", source)
        self.assertIn("function Invoke-CmakeChecked", source)
        self.assertIn("function Remove-DirectoryIfSafe", source)
        self.assertIn("Remove-DirectoryIfSafe -Path $StageRoot", source)
        self.assertIn("Remove-DirectoryIfSafe -Path $BuildDir", source)
        self.assertIn("if (-not $SkipBuild)", source)
        self.assertIn("${LASTEXITCODE}", source)
        self.assertNotIn("| Write-Host", source)
        self.assertNotIn("var\\runtime", source)
        self.assertNotIn("var\\host", source)
        self.assertNotIn(".pdb", source)
        self.assertNotIn(".obj", source)

    def test_restore_autocad_settings_helper_is_direct_and_standalone(self):
        cmd = self.workspace_root / "tools/restore-autocad-settings.cmd"
        ps1 = self.workspace_root / "tools/restore-autocad-settings.ps1"
        self.assertTrue(cmd.exists())
        self.assertFalse(ps1.exists())

        cmd_source = cmd.read_text(encoding="utf-8")
        package_source = (self.workspace_root / "tools/package.ps1").read_text(encoding="utf-8")
        readme = (self.workspace_root / "README.md").read_text(encoding="utf-8")
        readme_zh = (self.workspace_root / "README.zh-CN.md").read_text(encoding="utf-8")

        self.assertIn("powershell.exe", cmd_source)
        self.assertIn("-ExecutionPolicy Bypass", cmd_source)
        self.assertIn("-EncodedCommand", cmd_source)
        self.assertNotIn("restore-autocad-settings.ps1", cmd_source)
        self.assertIn("Usage: restore-autocad-settings.cmd", cmd_source)
        self.assertIn('/?', cmd_source)
        self.assertIn("--help", cmd_source)
        self.assertIn("GetActiveObject", cmd_source)
        self.assertIn("New-Object -ComObject", cmd_source)
        self.assertIn("SetVariable", cmd_source)
        self.assertIn("COMMANDLINE", cmd_source)
        for token in [
            "FILEDIA",
            "CMDDIA",
            "CMDECHO",
            "EXPERT",
            "PROXYNOTICE",
            "PROXYSHOW",
            "PROXYWEBSEARCH",
            "XREFNOTIFY",
            "XLOADCTL",
            "XREFCTL",
            "ISAVEBAK",
            "FONTMAP",
            "FONTALT",
        ]:
            self.assertIn(token, cmd_source)

        for forbidden in ["tangent.mnl", "host_bootstrap", "tbatsave", "var\\runtime"]:
            self.assertNotIn(forbidden, cmd_source)

        self.assertIn("restore-autocad-settings.cmd", package_source)
        self.assertNotIn("restore-autocad-settings.ps1", package_source)
        self.assertIn("restore-autocad-settings.cmd", readme)
        self.assertIn("restore-autocad-settings.cmd", readme_zh)
        self.assertNotIn("restore-autocad-settings.ps1", readme)
        self.assertNotIn("restore-autocad-settings.ps1", readme_zh)

    def test_docs_cover_modal_risks_and_current_calling_contract(self):
        source = (self.workspace_root / "docs/tbatsave-direct-worker-protocol.md").read_text(encoding="utf-8")

        for token in ["字体缺失", "FONTMAP", "FONTALT", "教育版", "代理对象", "外部参照", "确认对话框", "文件破坏"]:
            self.assertIn(token, source)
        self.assertIn("TGStart.exe", source)
        self.assertIn("SaveAsTArch3", source)
        self.assertIn("CreateRemoteThread", source)

    def test_project_docs_are_consolidated_without_duplicate_runtime_layout_doc(self):
        docs_dir = self.workspace_root / "docs"
        self.assertTrue((docs_dir / "t3conv-developer-guide.md").exists())
        self.assertTrue((docs_dir / "tbatsave-direct-worker-protocol.md").exists())
        self.assertFalse((docs_dir / "tbatsave_no_ui_developer_guide.md").exists())
        self.assertFalse((docs_dir / "tbatsave_native_no_ui_protocol.md").exists())
        self.assertFalse((docs_dir / "tgstart_host_runtime_layout.md").exists())

        guide = (docs_dir / "t3conv-developer-guide.md").read_text(encoding="utf-8")
        readme = (self.workspace_root / "README.md").read_text(encoding="utf-8")
        readme_zh = (self.workspace_root / "README.zh-CN.md").read_text(encoding="utf-8")

        for token in [
            "运行态文件布局",
            "tangent_mnl_bridge.runtime.lsp",
            "host_bootstrap.txt",
            "worker_status.txt",
            "timeout_seconds=120",
            "started_at=<local time, yyyy-MM-dd HH:mm:ss>",
            "除上表列出的 3 个文件外",
            "CMDECHO=0",
            "ISAVEBAK=0",
            "AutoCAD 2021 / 2020",
            "错误码",
            "排障",
        ]:
            self.assertIn(token, guide)

        self.assertIn("positional source path", readme)
        self.assertIn("位置参数", readme_zh)
        self.assertIn("AutoCAD 2020 or 2021", readme)
        self.assertIn("AutoCAD 2020 或 2021", readme_zh)
        self.assertIn("started_at", readme)
        self.assertIn("started_at", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-status", readme)
        self.assertNotIn(".\\t3conv.exe --host-stop", readme)
        self.assertNotIn(".\\t3conv.exe --host-start", readme)
        self.assertNotIn(".\\t3conv.exe --host-status", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-stop", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-start", readme_zh)
        self.assertNotIn("--host-stop", readme)
        self.assertNotIn("--host-start", readme)
        self.assertNotIn("--host-status", readme)
        self.assertNotIn("--host-stop", readme_zh)
        self.assertNotIn("--host-start", readme_zh)
        self.assertNotIn("--host-status", readme_zh)
        self.assertNotIn("render runtime LSP", readme)
        self.assertNotIn("normalize the minimal SYS/tangent.mnl bridge entry", readme)
        self.assertNotIn("渲染 runtime LSP", readme_zh)
        self.assertNotIn("规范化 SYS/tangent.mnl", readme_zh)
        self.assertNotIn("tgstart_host_runtime_layout.md", readme)
        self.assertNotIn("tgstart_host_runtime_layout.md", readme_zh)

    def test_direct_worker_fails_fast_when_acad_process_access_is_denied(self):
        source = (self.workspace_root / "src/t3conv/tbatsave_runtime_patch.cpp").read_text(encoding="utf-8")
        wait_start = source.index("std::optional<TianzhengAcadProcess> WaitForNewestTianzhengAcadProcess")
        wait_body = source[wait_start:source.index("\n}\n\n}  // namespace", wait_start)]

        self.assertIn("TryDiagnoseDirectWorkerAccessDenied", source)
        self.assertIn("const bool should_fail_fast", source)
        self.assertIn("return std::nullopt;", wait_body)
        self.assertLess(
            wait_body.index("TryDiagnoseDirectWorkerAccessDenied(diagnostics)"),
            wait_body.index("Sleep(100);")
        )
        self.assertLess(
            wait_body.index("TryDiagnoseDirectWorkerAccessDenied(diagnostics)"),
            wait_body.index("diagnostics.push_back(\"tbatsave_direct_worker=tch_kernal_not_loaded\")")
        )

    def test_production_sources_do_not_reference_removed_paths(self):
        source_roots = [
            self.workspace_root / "src",
            self.workspace_root / "runtime",
        ]
        forbidden = [
            "accoreconsole",
            "TBatchNoUI",
            "--runtime",
            "--tbatsave-experimental",
            "--tbatsave-probe",
            "--batch",
            "--allow-ui-fallback",
            "t3conv_arx",
            "t3conv_py",
            "reverse/lib",
            "t3-converter",
        ]

        for root in source_roots:
            for path in root.rglob("*"):
                if not path.is_file() or path.suffix.lower() not in {".cpp", ".h", ".lsp", ".txt"}:
                    continue
                text = path.read_text(encoding="utf-8", errors="ignore")
                for token in forbidden:
                    self.assertNotIn(token, text, f"{token} leaked in {path}")


if __name__ == "__main__":
    unittest.main()
