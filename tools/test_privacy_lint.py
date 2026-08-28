#!/usr/bin/env python3
"""Focused tests for the repository privacy lint."""

from __future__ import annotations

import contextlib
import os
import pathlib
import subprocess
import tempfile
import unittest

from privacy_lint import main, scan_bytes


def _linux_path(user: str) -> str:
    return "/" + "home" + "/" + user + "/project/file"


def _macos_path(user: str) -> str:
    return "/" + "Users" + "/" + user + "/project/file"


def _windows_path(user: str) -> str:
    return "C:" + "\\" + "Users" + "\\" + user + "\\project\\file"


def _run_git(root: pathlib.Path, *arguments: str) -> None:
    subprocess.run(["git", *arguments], cwd=root, check=True, stdout=subprocess.PIPE)


@contextlib.contextmanager
def _in_directory(path: pathlib.Path):
    previous = pathlib.Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


class PrivacyLintTests(unittest.TestCase):
    def test_rejects_developer_specific_linux_macos_and_windows_paths(self) -> None:
        for path in (
            _linux_path("alice"),
            _macos_path("bob"),
            _windows_path("charlie"),
        ):
            with self.subTest(path=path):
                self.assertEqual(len(scan_bytes("fixture.txt", path.encode())), 1)

    def test_rejects_profile_components_containing_spaces(self) -> None:
        for path in (
            _linux_path("Alice Smith"),
            _macos_path("Jane Doe"),
            _windows_path("John Smith"),
        ):
            with self.subTest(path=path):
                self.assertEqual(len(scan_bytes("fixture.txt", path.encode())), 1)

    def test_allows_neutral_home_forms(self) -> None:
        allowed = (
            "$HOME/project/file",
            "${HOME}/project/file",
            "~/project/file",
            _linux_path("USER"),
            _linux_path("<user>"),
            _macos_path("USER"),
            _macos_path("<user>"),
            _windows_path("USER"),
            _windows_path("<user>"),
        )
        for path in allowed:
            with self.subTest(path=path):
                self.assertEqual(scan_bytes("fixture.txt", path.encode()), [])

    def test_non_utf8_binary_content_does_not_crash(self) -> None:
        self.assertEqual(scan_bytes("binary.dat", b"\x00\x80\xff\x00binary"), [])

    def test_clean_input_has_no_findings(self) -> None:
        self.assertEqual(scan_bytes("clean.txt", b"relative/project/file\n"), [])

    def test_staged_mode_reads_index_not_unstaged_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _run_git(root, "init", "--quiet")
            _run_git(root, "config", "user.email", "privacy-lint@example.invalid")
            _run_git(root, "config", "user.name", "Privacy Lint Test")
            target = root / "fixture.txt"
            target.write_text("relative/project/file\n", encoding="utf-8")
            _run_git(root, "add", "fixture.txt")
            target.write_text(_linux_path("alice") + "\n", encoding="utf-8")
            with _in_directory(root):
                self.assertEqual(main(["--staged"]), 0)

            _run_git(root, "add", "fixture.txt")
            with _in_directory(root):
                self.assertEqual(main(["--staged"]), 1)

    def test_tracked_mode_scans_tracked_worktree_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            _run_git(root, "init", "--quiet")
            _run_git(root, "config", "user.email", "privacy-lint@example.invalid")
            _run_git(root, "config", "user.name", "Privacy Lint Test")
            (root / "fixture.txt").write_text(_macos_path("bob") + "\n", encoding="utf-8")
            _run_git(root, "add", "fixture.txt")
            _run_git(root, "commit", "--quiet", "-m", "fixture")
            with _in_directory(root):
                self.assertEqual(main(["--tracked"]), 1)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
