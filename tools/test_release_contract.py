#!/usr/bin/env python3
"""Focused no-network tests for release authority, names, and tag resolution."""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

from build_firmware_artifact import build
from firmware_artifact import ArtifactError
from release_contract import (
    ReleaseContractError,
    read_build_profile,
    read_release_contract,
    release_tag,
    resolve_annotated_tag_commit,
    validate_release_tag,
    validate_release_version,
)


ROOT = Path(__file__).resolve().parents[1]


class ReleaseContractTests(unittest.TestCase):
    def test_authoritative_versions_and_names(self) -> None:
        contract = read_release_contract(ROOT)
        self.assertEqual(contract.version, "0.3.0")
        self.assertEqual(contract.tag, "v0.3.0")
        self.assertEqual(
            contract.firmware_archive,
            "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0099.tar.gz",
        )
        self.assertEqual(contract.build_profile, "freenove-fnk0099")
        self.assertIn("hidbot/legacy_recovery.py", contract.host_modules)
        self.assertEqual(contract.host_wheel, "s3_hidbot_host-0.3.0-py3-none-any.whl")
        self.assertEqual(contract.host_sdist, "s3_hidbot_host-0.3.0.tar.gz")
        self.assertEqual(len(contract.distributable_assets), 6)

    def test_release_versions_reject_development_and_malformed_values(self) -> None:
        for value in ("0.1.0-dev", "0.1.0+build", "v0.1.0", "01.0.0", "1.0", "1.0.0.0"):
            with self.subTest(value=value), self.assertRaises(ReleaseContractError):
                validate_release_version(value)

    def test_authoritative_firmware_and_host_version_mismatch_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "firmware").mkdir()
            (root / "host").mkdir()
            identity = root / "firmware/components/firmware_identity/include/firmware_identity"
            identity.mkdir(parents=True)
            (identity / "firmware_identity.hpp").write_text(
                'inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";\n',
                encoding="utf-8",
            )
            (root / "firmware" / "version.txt").write_text("0.1.0\n", encoding="utf-8")
            (root / "host" / "pyproject.toml").write_text(
                "[project]\nname = 's3-hidbot-host'\nversion = '0.1.1'\n",
                encoding="utf-8",
            )
            with self.assertRaises(ReleaseContractError):
                read_release_contract(root)

    def test_archive_profile_is_derived_from_selected_source_not_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "firmware").mkdir()
            (root / "host").mkdir()
            identity = root / "firmware/components/firmware_identity/include/firmware_identity"
            identity.mkdir(parents=True)
            (root / "firmware/version.txt").write_text("0.3.0\n", encoding="utf-8")
            (root / "host/pyproject.toml").write_text(
                "[project]\nname = 's3-hidbot-host'\nversion = '0.3.0'\n",
                encoding="utf-8",
            )
            header = identity / "firmware_identity.hpp"
            header.write_text(
                'namespace firmware_identity {\n'
                'inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";\n}\n',
                encoding="utf-8",
            )
            historical = read_release_contract(root)
            self.assertEqual(historical.build_profile, "freenove-fnk0085")
            self.assertNotIn("hidbot/legacy_recovery.py", historical.host_modules)
            self.assertEqual(
                historical.firmware_archive,
                "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0085.tar.gz",
            )
            header.write_text(
                'namespace firmware_identity {\n'
                'inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";\n}\n',
                encoding="utf-8",
            )
            current = read_release_contract(root)
            self.assertEqual(
                current.firmware_archive,
                "s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0099.tar.gz",
            )

    def test_profile_parser_ignores_comments_and_unrelated_literals(self) -> None:
        source = """
namespace firmware_identity {
// kBuildProfile = "freenove-fnk0099"
/* inline constexpr std::string_view kBuildProfile = "also-wrong"; */
inline constexpr const char* unrelated = "kBuildProfile = wrong-again";
inline /* placement */ constexpr std::string_view
    kBuildProfile /* active profile */ =
        "freenove-fnk0085";
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = self._write_identity_header(root, source)
            self.assertEqual(read_build_profile(root), "freenove-fnk0085")
            self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")

    def test_current_and_frozen_profiles_match_compiler_semantics(self) -> None:
        current_header = (
            ROOT
            / "firmware/components/firmware_identity/include/firmware_identity/firmware_identity.hpp"
        )
        self.assertEqual(read_build_profile(ROOT), "freenove-fnk0099")
        with tempfile.TemporaryDirectory() as temporary:
            self.assertEqual(
                self._compiled_profile(Path(temporary), current_header),
                "freenove-fnk0099",
            )

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            frozen_header = self._write_identity_header(
                root,
                "namespace firmware_identity {\n"
                'inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";\n'
                "}\n",
            )
            self.assertEqual(read_build_profile(root), "freenove-fnk0085")
            self.assertEqual(self._compiled_profile(root, frozen_header), "freenove-fnk0085")

    def test_profile_parser_rejects_raw_string_bypass_before_authority(self) -> None:
        source = r'''
namespace firmware_identity {
inline constexpr auto example = R"tag("; inline constexpr std::string_view kBuildProfile = "freenove-fnk0099"; )tag"; inline constexpr std::string_view kBuildProfile = "freenove-fnk0085"; // "
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            header = self._write_identity_header(root, source)
            self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
            with self.assertRaisesRegex(ReleaseContractError, "unsupported raw string literal"):
                read_build_profile(root)
            self._assert_authority_rejected(root)

    def test_profile_parser_rejects_all_standard_raw_string_prefixes(self) -> None:
        for prefix in ("R", "u8R", "uR", "UR", "LR"):
            source = f'''
namespace firmware_identity {{
inline constexpr auto example = {prefix}"arb_42(kBuildProfile = fake)arb_42";
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";
}}
'''
            with self.subTest(prefix=prefix), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                header = self._write_identity_header(root, source)
                self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
                with self.assertRaisesRegex(
                    ReleaseContractError, "unsupported raw string literal"
                ):
                    read_build_profile(root)
                self._assert_authority_rejected(root)

    def test_profile_parser_rejects_preprocessing_and_scope_counterexamples(self) -> None:
        cases = {
            "digraph-directives": '''
namespace firmware_identity {
%:if 0
; inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
%:endif
%:define PROFILE_ID kBuildPro %:%: file
inline constexpr std::string_view PROFILE_ID = "freenove-fnk0085";
}
''',
            "local-include": '''
#include "active.hpp"
namespace example {
inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
}
''',
            "pragma-decoy": '''
#pragma once inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
#include "active.hpp"
''',
            "type-rebinding": '''
namespace firmware_identity {
struct std {
    struct string_view {
        constexpr string_view(const char*) {}
        constexpr operator const char*() const { return "freenove-fnk0085"; }
    };
};
inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
}
''',
            "namespace-alias": '''
namespace example {
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";
}
namespace firmware_identity = example;
''',
            "nested-authority": '''
namespace firmware_identity {
namespace example {
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";
}
using namespace example;
}
''',
        }
        for name, source in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                header = self._write_identity_header(root, source)
                (header.parent / "active.hpp").write_text(
                    'namespace firmware_identity {\n'
                    'inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";\n}\n',
                    encoding="utf-8",
                )
                self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
                self._assert_authority_rejected(root)

    def test_profile_parser_rejects_extended_splices_and_preprocessing_operators(self) -> None:
        for whitespace in (" ", "\t"):
            source = (
                'namespace firmware_identity {\n'
                '// comment \\' + whitespace + '\n'
                'inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";\n'
                'inline constexpr std::string_view kBuildPro\\' + whitespace + '\n'
                'file = "freenove-fnk0085";\n}\n'
            )
            with self.subTest(whitespace=repr(whitespace)), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                header = self._write_identity_header(root, source)
                self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
                self._assert_authority_rejected(root)
        for source in (
            'namespace firmware_identity {\n_Pragma("once")\n'
            'inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";\n}\n',
            'namespace firmware_identity {\n'
            'inline constexpr std::string_view kBuildProfile = ("freenove-fnk0085");\n}\n',
            '#define PROFILE "freenove-fnk0085"\nnamespace firmware_identity {\n'
            'inline constexpr std::string_view kBuildProfile = PROFILE;\n}\n',
            '#define IGNORED 1\nnamespace firmware_identity {\n'
            'inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";\n}\n',
        ):
            with self.subTest(source=source), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                header = self._write_identity_header(root, source)
                self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
                self._assert_authority_rejected(root)

    def _assert_authority_rejected(self, root: Path) -> None:
        (root / "firmware/version.txt").write_text("0.3.0\n", encoding="utf-8")
        (root / "host").mkdir()
        (root / "host/pyproject.toml").write_text(
            '[project]\nversion = "0.3.0"\n', encoding="utf-8"
        )
        with self.assertRaises(ReleaseContractError):
            read_build_profile(root)
        with self.assertRaises(ReleaseContractError):
            read_release_contract(root)
        output = root / "output/s3-hidbot-firmware-0.3.0-esp32s3-freenove-fnk0099.tar.gz"
        with self.assertRaisesRegex(ArtifactError, "firmware build profile authority"):
            build(Namespace(
                source_root=root, source_revision="a" * 40, source_date_epoch=0,
                output=output, container_image=None,
            ))
        self.assertFalse(output.parent.exists())

    def test_profile_parser_rejects_physical_line_splicing(self) -> None:
        for newline in ("\n", "\r\n"):
            source = (
                "namespace firmware_identity {" + newline
                + '// inline constexpr std::string_view kBuildProfile = "freenove-fnk0099"; \\'
                + newline
                + 'inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";'
                + newline
                + "inline constexpr std::string_view kBuildPro\\"
                + newline
                + 'file = "freenove-fnk0085";'
                + newline
                + "}"
                + newline
            )
            with self.subTest(newline=repr(newline)), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                header = self._write_identity_header(root, source)
                self.assertEqual(self._compiled_profile(root, header), "freenove-fnk0085")
                with self.assertRaisesRegex(
                    ReleaseContractError, "unsupported physical line splicing"
                ):
                    read_build_profile(root)
                self._assert_authority_rejected(root)

    def test_profile_parser_fails_closed_on_ambiguous_or_unsupported_authority(self) -> None:
        cases = (
            """
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";
inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
""",
            'inline constexpr auto kBuildProfile = "freenove-fnk0099";\n',
            """
#if 0
inline constexpr std::string_view kBuildProfile = "freenove-fnk0085";
#endif
inline constexpr std::string_view kBuildProfile = "freenove-fnk0099";
""",
        )
        for source in cases:
            with self.subTest(source=source), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self._write_identity_header(root, source)
                with self.assertRaises(ReleaseContractError):
                    read_build_profile(root)

    def test_tag_must_match_version_exactly(self) -> None:
        self.assertEqual(release_tag("0.1.0"), "v0.1.0")
        self.assertEqual(validate_release_tag("v0.1.0", "0.1.0"), "v0.1.0")
        for tag in ("0.1.0", "v0.1.1", "v0.1.0-dev", "release-0.1.0"):
            with self.subTest(tag=tag), self.assertRaises(ReleaseContractError):
                validate_release_tag(tag, "0.1.0")

    def test_annotated_tag_resolves_to_peeled_commit_not_tag_object(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            self._git(repository, "init")
            self._git(repository, "config", "user.name", "Release test")
            self._git(repository, "config", "user.email", "release-test@example.invalid")
            (repository / "payload").write_text("release fixture\n", encoding="utf-8")
            self._git(repository, "add", "payload")
            self._git(repository, "commit", "-m", "fixture")
            commit = self._git(repository, "rev-parse", "HEAD")
            self._git(repository, "tag", "-a", "v0.1.0", "-m", "annotated fixture")
            tag_object = self._git(repository, "rev-parse", "refs/tags/v0.1.0")
            self.assertNotEqual(tag_object, commit)
            self.assertEqual(
                resolve_annotated_tag_commit(repository, "v0.1.0", "0.1.0"),
                commit,
            )

    def test_lightweight_tag_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary)
            self._git(repository, "init")
            self._git(repository, "config", "user.name", "Release test")
            self._git(repository, "config", "user.email", "release-test@example.invalid")
            (repository / "payload").write_text("release fixture\n", encoding="utf-8")
            self._git(repository, "add", "payload")
            self._git(repository, "commit", "-m", "fixture")
            self._git(repository, "tag", "v0.1.0")
            with self.assertRaises(ReleaseContractError):
                resolve_annotated_tag_commit(repository, "v0.1.0", "0.1.0")

    @staticmethod
    def _git(repository: Path, *arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    @staticmethod
    def _write_identity_header(root: Path, declaration: str) -> Path:
        header = (
            root
            / "firmware/components/firmware_identity/include/firmware_identity/firmware_identity.hpp"
        )
        header.parent.mkdir(parents=True)
        header.write_text(
            "#pragma once\n#include <string_view>\n" + declaration,
            encoding="utf-8",
        )
        return header

    @staticmethod
    def _compiled_profile(root: Path, header: Path) -> str:
        source = root / "profile_oracle.cpp"
        executable = root / "profile_oracle"
        source.write_text(
            '#include <iostream>\n#include "firmware_identity/firmware_identity.hpp"\n'
            "int main() { std::cout << firmware_identity::kBuildProfile; }\n",
            encoding="utf-8",
        )
        subprocess.run(
            [
                "c++",
                "-std=c++20",
                f"-I{header.parent.parent}",
                str(source),
                "-o",
                str(executable),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        return subprocess.run(
            [str(executable)], check=True, capture_output=True, text=True
        ).stdout


if __name__ == "__main__":
    result = unittest.main(argv=[__file__], exit=False)
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
