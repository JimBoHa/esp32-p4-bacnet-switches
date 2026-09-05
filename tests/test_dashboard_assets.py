#!/usr/bin/env python3
"""Static security and structure checks for the embedded diagnostics UI."""

from __future__ import annotations

from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[1]
HTML = ROOT / "main" / "diagnostics_dashboard.html"
CSS = ROOT / "main" / "diagnostics_dashboard.css"
JAVASCRIPT = ROOT / "main" / "diagnostics_dashboard.js"


class DashboardParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: set[str] = set()
        self.scripts: list[dict[str, str | None]] = []
        self.stylesheets: list[dict[str, str | None]] = []
        self.inline_handlers: list[str] = []

    def handle_starttag(
        self,
        tag: str,
        attributes: list[tuple[str, str | None]],
    ) -> None:
        values = dict(attributes)
        if values.get("id"):
            self.ids.add(str(values["id"]))
        self.inline_handlers.extend(
            name for name, _ in attributes if name.lower().startswith("on")
        )
        if tag == "script":
            self.scripts.append(values)
        if tag == "link" and values.get("rel") == "stylesheet":
            self.stylesheets.append(values)


class DashboardAssetTests(unittest.TestCase):
    def test_html_uses_only_same_origin_external_assets(self) -> None:
        source = HTML.read_text(encoding="utf-8")
        parser = DashboardParser()
        parser.feed(source)
        self.assertEqual(
            parser.scripts,
            [{"src": "/diagnostics/app.js", "defer": None}],
        )
        self.assertEqual(
            parser.stylesheets,
            [{"rel": "stylesheet", "href": "/diagnostics/app.css"}],
        )
        self.assertEqual(parser.inline_handlers, [])
        self.assertNotRegex(source, r"https?://")
        self.assertNotIn("<style", source.lower())

    def test_script_references_existing_elements(self) -> None:
        html = HTML.read_text(encoding="utf-8")
        script = JAVASCRIPT.read_text(encoding="utf-8")
        parser = DashboardParser()
        parser.feed(html)
        referenced = set(re.findall(r'element\("([A-Za-z][A-Za-z0-9]*)"\)', script))
        self.assertTrue(referenced)
        self.assertEqual(referenced - parser.ids, set())

    def test_token_and_rendering_stay_on_safe_browser_surfaces(self) -> None:
        script = JAVASCRIPT.read_text(encoding="utf-8")
        self.assertIn('window.fetch("/ota/status"', script)
        self.assertIn('"Authorization": `Bearer ${bearerToken}`', script)
        self.assertIn("textContent", script)
        for forbidden in (
            "localStorage",
            "sessionStorage",
            "document.cookie",
            "innerHTML",
            "outerHTML",
            "eval(",
            "new Function",
            "console.",
        ):
            self.assertNotIn(forbidden, script)

    def test_assets_have_responsive_and_accessible_basics(self) -> None:
        html = HTML.read_text(encoding="utf-8")
        css = CSS.read_text(encoding="utf-8")
        self.assertIn('aria-live="polite"', html)
        self.assertIn('type="password"', html)
        self.assertIn("@media (max-width: 520px)", css)
        self.assertIn("prefers-reduced-motion", css)

    @unittest.skipUnless(shutil.which("node"), "Node.js is unavailable")
    def test_javascript_parses(self) -> None:
        subprocess.run(
            ["node", "--check", str(JAVASCRIPT)],
            check=True,
            capture_output=True,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
