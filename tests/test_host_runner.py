from contextlib import redirect_stderr, redirect_stdout
import io
import subprocess
from types import SimpleNamespace
import unittest
from unittest import mock

from tools import run_host_tests


class HostRunnerTests(unittest.TestCase):
    def test_required_commands_are_host_only_and_keep_assertions(self):
        commands = run_host_tests.commands()
        self.assertEqual(len(commands), 7)
        self.assertIn("--history", commands[0])
        self.assertIn("-DCMAKE_BUILD_TYPE=Debug", commands[1])
        self.assertIn("--output-on-failure", commands[3])
        self.assertEqual(commands[4][-1], "test_*.py")
        self.assertIn("--cached", commands[-1])
        self.assertFalse(any("idf.py" in item or "--host" == item for command in commands for item in command))

    def test_missing_node_fails_instead_of_skipping_browser_tests(self):
        with mock.patch.object(run_host_tests.shutil, "which", side_effect=lambda name: None if name == "node" else "/tool"):
            with self.assertRaisesRegex(ValueError, "node"):
                run_host_tests.preflight()

    def test_old_node_fails(self):
        with mock.patch.object(run_host_tests.shutil, "which", return_value="/tool"), \
             mock.patch.object(run_host_tests.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "v16.0.0")):
            with self.assertRaisesRegex(ValueError, "18"):
                run_host_tests.preflight()

    def test_optimized_python_is_rejected(self):
        with mock.patch.object(run_host_tests.shutil, "which", return_value="/tool"), \
             mock.patch.object(run_host_tests.sys, "flags", SimpleNamespace(optimize=1)), \
             mock.patch.object(run_host_tests.subprocess, "run") as execute:
            with self.assertRaisesRegex(ValueError, "assertions"):
                run_host_tests.preflight()
            execute.assert_not_called()

    def test_stops_at_first_failing_stage(self):
        with mock.patch.object(run_host_tests, "preflight"), \
             mock.patch.object(run_host_tests.subprocess, "run", side_effect=[subprocess.CompletedProcess([], 0), subprocess.CompletedProcess([], 7)]) as execute, \
             redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(run_host_tests.main([]), 7)
            self.assertEqual(execute.call_count, 2)
            self.assertEqual(execute.call_args.kwargs["cwd"], run_host_tests.ROOT)
            self.assertNotIn("shell", execute.call_args.kwargs)

    def test_list_does_not_execute(self):
        with mock.patch.object(run_host_tests.subprocess, "run") as execute, redirect_stdout(io.StringIO()):
            self.assertEqual(run_host_tests.main(["--list"]), 0)
            execute.assert_not_called()

    def test_complete_success(self):
        with mock.patch.object(run_host_tests, "preflight"), \
             mock.patch.object(run_host_tests.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as execute, \
             redirect_stdout(io.StringIO()):
            self.assertEqual(run_host_tests.main([]), 0)
            self.assertEqual(execute.call_count, 7)
