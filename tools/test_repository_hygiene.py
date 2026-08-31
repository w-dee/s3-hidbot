#!/usr/bin/env python3
"""Focused tests for repository ignore and tracked-path hygiene contracts."""

from __future__ import annotations

import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from repository_hygiene import forbidden_category, forbidden_paths, main


ROOT = Path(__file__).resolve().parents[1]


def _run_git(root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )


@contextlib.contextmanager
def _in_directory(path: Path):
    previous = Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


class RepositoryHygieneTests(unittest.TestCase):
    def test_rejects_all_project_owned_forbidden_path_categories(self) -> None:
        cases = {
            ".envrc": "machine-local configuration",
            "foo/__pycache__/module.cpython-312.pyc": "Python generated cache",
            "random.pyc": "Python generated cache",
            "firmware/build/example.o": "generated/local build output",
            "firmware/artifacts/example.tar.gz": "generated/local build output",
            "firmware/sdkconfig": "ESP-IDF generated/local state",
            "firmware/managed_components/example/file": "generated/local build output",
            "host/build/example": "generated/local build output",
            "host/dist/example.whl": "generated/local build output",
            "host/src/example.egg-info/PKG-INFO": "host packaging metadata",
        }
        for path, expected in cases.items():
            with self.subTest(path=path):
                self.assertEqual(forbidden_category(path), expected)

    def test_accepts_legitimate_repository_paths(self) -> None:
        for path in (
            ".editorconfig",
            ".clang-format",
            "nested/.envrc",
            ".github/workflows/example.yml",
            ".githooks/pre-commit",
            "firmware/main/main.cpp",
            "host/src/hidbot/cli.py",
        ):
            with self.subTest(path=path):
                self.assertIsNone(forbidden_category(path))

    def test_findings_are_sorted_and_path_based(self) -> None:
        self.assertEqual(
            forbidden_paths(["host/dist/example.whl", ".envrc", "host/src/hidbot/cli.py"]),
            [
                (".envrc", "machine-local configuration"),
                ("host/dist/example.whl", "generated/local build output"),
            ],
        )

    def test_staged_mode_rejects_force_added_ignored_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _run_git(root, "init", "--quiet")
            (root / ".gitignore").write_text(".envrc\nfirmware/build/\n", encoding="utf-8")
            (root / ".envrc").write_text("local-only\n", encoding="utf-8")
            generated = root / "firmware/build/example.o"
            generated.parent.mkdir(parents=True)
            generated.write_bytes(b"generated\n")
            _run_git(root, "add", "-f", ".envrc", "firmware/build/example.o")
            with _in_directory(root):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(main(["--staged"]), 1)
            self.assertEqual(
                output.getvalue().splitlines(),
                [
                    "repository hygiene: forbidden staged path: .envrc "
                    "(machine-local configuration)",
                    "repository hygiene: forbidden staged path: firmware/build/example.o "
                    "(generated/local build output)",
                ],
            )

    def test_tracked_mode_rejects_committed_generated_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _run_git(root, "init", "--quiet")
            generated = root / "host/dist/example.whl"
            generated.parent.mkdir(parents=True)
            generated.write_bytes(b"wheel\n")
            _run_git(root, "add", "host/dist/example.whl")
            with _in_directory(root):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    self.assertEqual(main(["--tracked"]), 1)
            self.assertEqual(
                output.getvalue(),
                "repository hygiene: forbidden tracked path: host/dist/example.whl "
                "(generated/local build output)\n",
            )

    def test_repository_ignore_policy_is_explicit_and_narrow(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _run_git(root, "init", "--quiet")
            (root / ".gitignore").write_text((ROOT / ".gitignore").read_text(encoding="utf-8"), encoding="utf-8")

            ignored = (
                ".envrc",
                "nested/__pycache__/module.pyc",
                "random.pyc",
                "firmware/build/example.o",
                "firmware/artifacts/example.tar.gz",
                "firmware/sdkconfig",
                "firmware/managed_components/example/file",
                "host/build/example",
                "host/dist/example.whl",
                "host/src/example.egg-info/PKG-INFO",
            )
            accepted = (
                ".editorconfig",
                ".clang-format",
                "nested/.envrc",
                ".gitignore",
                ".githooks/pre-commit",
                ".github/workflows/example.yml",
                ".vscode/settings.json",
                ".devcontainer/devcontainer.json",
            )
            for path in ignored:
                with self.subTest(path=path):
                    result = subprocess.run(
                        ["git", "check-ignore", "-q", "--no-index", path], cwd=root
                    )
                    self.assertEqual(result.returncode, 0)
            for path in accepted:
                with self.subTest(path=path):
                    result = subprocess.run(
                        ["git", "check-ignore", "-q", "--no-index", path], cwd=root
                    )
                    self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
