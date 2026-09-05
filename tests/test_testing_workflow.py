"""Keep the portable test entry points and firmware/host boundary discoverable."""

from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
GUIDES = ("AGENTS.md", "README.md", "docs/TESTING.md", "docs/VALIDATION_HISTORY.md",
          "docs/DEVELOPMENT.md", "docs/HARDWARE_TESTING.md", "docs/SOAK_TESTING.md",
          ".github/pull_request_template.md")


class TestingWorkflowTests(unittest.TestCase):
    def test_relative_guide_links_resolve(self):
        for filename in GUIDES:
            path = ROOT / filename
            for target in re.findall(r"\[[^\]]+\]\(([^)\s]+)\)", path.read_text()):
                parsed = urlsplit(target.strip("<>"))
                if parsed.scheme or parsed.netloc or not parsed.path:
                    continue
                with self.subTest(guide=filename, target=target):
                    self.assertTrue((path.parent / unquote(parsed.path)).is_file())

    def test_agent_readme_and_ci_share_required_runner(self):
        command = "python3 tools/run_host_tests.py"
        for filename in ("AGENTS.md", "README.md", "docs/TESTING.md",
                         "docs/DEVELOPMENT.md", ".github/pull_request_template.md",
                         ".github/workflows/build.yml"):
            with self.subTest(filename=filename):
                self.assertIn(command, (ROOT / filename).read_text())
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("tests/sdkconfig.no_ota.defaults", workflow)
        self.assertNotIn("upload-artifact", workflow)

    def test_runbook_inventory_points_to_real_scripts(self):
        runbook = (ROOT / "docs/TESTING.md").read_text()
        scripts = set(re.findall(r"(?:tools|tests)/[a-z_]+\.(?:py|c|mjs)", runbook))
        self.assertGreaterEqual(len(scripts), 20)
        for script in scripts:
            with self.subTest(script=script):
                self.assertTrue((ROOT / script).is_file())

    def test_handoff_files_are_not_registered_in_firmware_build(self):
        manifests = "\n".join((ROOT / path).read_text()
                              for path in ("CMakeLists.txt", "main/CMakeLists.txt"))
        for name in ("AGENTS.md", "TESTING.md", "VALIDATION_HISTORY.md",
                     "pull_request_template.md", "run_host_tests.py",
                     "verify_live_readonly.py", "test_host_runner.py",
                     "test_live_readonly.py", "test_testing_workflow.py"):
            with self.subTest(name=name):
                self.assertNotIn(name, manifests)
        self.assertNotRegex(manifests, r"(?i)add_subdirectory\s*\([^)]*tests")


if __name__ == "__main__":
    unittest.main()
