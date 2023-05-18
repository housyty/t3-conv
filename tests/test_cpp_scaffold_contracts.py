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
                self.workspace_root.parent / "TArchT20V9",
            )
        )

    def tearDown(self) -> None:
        self._temp_dir.cleanup()

    def run_exe(self, *args: str) -> subprocess.CompletedProcess[str]:
        command = [str(self.exe), *args]
        try:
            return subprocess.run(
                command,
                capture_output=True,
                text=True,
                check=False,
                timeout=self.EXE_TIMEOUT_SECONDS,
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

    def test_help_output_only_mentions_current_flags(self):
        completed = self.run_exe("--help")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("Usage: t3conv", completed.stdout)
        self.assertIn("-s <path>", completed.stdout)
        self.assertIn("-o <path>", completed.stdout)
        self.assertIn("-d, --debug", completed.stdout)
        self.assertIn("--retries <n>", completed.stdout)
        self.assertIn("--host-start", completed.stdout)
        self.assertIn("--host-status", completed.stdout)
        self.assertIn("--host-stop", completed.stdout)
        self.assertIn("--tbatsave-bindmode", completed.stdout)
        self.assertLess(completed.stdout.index("--json"), completed.stdout.index("--host-start"))
        self.assertIn("--dry-run", completed.stdout)
        self.assertNotIn("--runtime", completed.stdout)
        self.assertNotIn("--tbatsave-experimental", completed.stdout)
        self.assertNotIn("--tbatsave-probe", completed.stdout)
        self.assertNotIn("--batch", completed.stdout)
        self.assertNotIn("accoreconsole", completed.stdout.lower())
        self.assertNotIn("--acad <path>", completed.stdout)
        self.assertNotIn("--arx <path>", completed.stdout)

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
        self.assertIn(f"stage_source={norm(self.workspace_root / '_t3conv_work' / self.sample_source.name)}", completed.stdout)
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

    def test_host_control_dry_runs_render_host_paths(self):
        for flag, state in [
            ("--host-start", "start"),
            ("--host-status", "status"),
            ("--host-stop", "stop"),
        ]:
            with self.subTest(flag=flag):
                completed = self.run_exe("--dry-run", flag)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertIn(f"host_control={state}", completed.stdout)
                self.assertIn(f"host_ready={norm(self.workspace_root / 'var/host/host_ready.txt')}", completed.stdout)
                self.assertIn(f"host_stop={norm(self.workspace_root / 'var/host/host_stop.txt')}", completed.stdout)
                self.assertIn(f'command="{norm(self.expected_tangent_root / "TGStart.exe")}"', completed.stdout)

    def test_host_control_dry_run_script_contents_ends_with_newline(self):
        completed = self.run_exe("--dry-run", "--host-start")

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(completed.stdout.endswith("script_contents:\nBOOTSTRAP\n"), completed.stdout)

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
        self.assertIn("tangent.mnl", source)
        self.assertIn("tangent_mnl_bridge.runtime.lsp", source)
        self.assertIn("IsManagedTbatsaveBridgeLoadLine", source)
        self.assertIn("tangent_mnl_bridge.lsp", source)
        self.assertIn("host_hook=normalized_mnl", source)
        self.assertIn("host_hook=already_normalized", source)

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

    def test_batch_runner_is_integrated_into_t3conv_exe(self):
        cmake = (self.workspace_root / "src/t3conv/CMakeLists.txt").read_text(encoding="utf-8")
        header = (self.workspace_root / "src/t3conv/batch_runner.h").read_text(encoding="utf-8")
        source = (self.workspace_root / "src/t3conv/batch_runner.cpp").read_text(encoding="utf-8")
        main = (self.workspace_root / "src/t3conv/main.cpp").read_text(encoding="utf-8")

        self.assertIn("batch_runner.cpp", cmake)
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

    def test_packaging_script_creates_portable_zip_without_generated_state(self):
        script = self.workspace_root / "tools/package.ps1"
        self.assertTrue(script.exists())
        source = script.read_text(encoding="utf-8")

        self.assertIn('ZipName = "t3-conv.zip"', source)
        self.assertIn('Join-Path $ProjectParent "dist"', source)
        self.assertIn("t3conv.exe", source)
        self.assertIn("t3conv.ini", source)
        self.assertIn("runtime", source)
        self.assertIn("fonts", source)
        self.assertIn("Compress-Archive", source)
        self.assertIn("function Invoke-CmakeChecked", source)
        self.assertIn("${LASTEXITCODE}", source)
        self.assertNotIn("| Write-Host", source)
        self.assertNotIn("var\\runtime", source)
        self.assertNotIn("var\\host", source)
        self.assertNotIn(".pdb", source)
        self.assertNotIn(".obj", source)

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
            "除上表列出的 4 个文件外",
            "CMDECHO=0",
            "ISAVEBAK=0",
            "AutoCAD 2020-2026",
            "错误码",
            "排障",
        ]:
            self.assertIn(token, guide)

        self.assertLess(readme.index('Convert one DWG with the minimum arguments'), readme.index('--host-start'))
        self.assertLess(readme_zh.index('用最少参数转换单个 DWG'), readme_zh.index('--host-start'))
        self.assertIn("positional source path", readme)
        self.assertIn("位置参数", readme_zh)
        self.assertIn("AutoCAD 2020-2026", readme)
        self.assertIn("AutoCAD 2020-2026", readme_zh)
        self.assertIn("started_at", readme)
        self.assertIn("started_at", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-status", readme)
        self.assertNotIn(".\\t3conv.exe --host-stop", readme)
        self.assertNotIn(".\\t3conv.exe --host-start", readme)
        self.assertNotIn(".\\t3conv.exe --host-status", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-stop", readme_zh)
        self.assertNotIn(".\\t3conv.exe --host-start", readme_zh)
        self.assertNotIn("render runtime LSP", readme)
        self.assertNotIn("normalize the minimal SYS/tangent.mnl bridge entry", readme)
        self.assertNotIn("渲染 runtime LSP", readme_zh)
        self.assertNotIn("规范化 SYS/tangent.mnl", readme_zh)
        self.assertNotIn("tgstart_host_runtime_layout.md", readme)
        self.assertNotIn("tgstart_host_runtime_layout.md", readme_zh)

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
