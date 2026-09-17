#!/usr/bin/env python3
"""Single source-of-truth checks for an s3-hidbot public release.

This module is deliberately stdlib-only so Actions jobs, release preparation,
and focused tests all apply the same strict version and tag rules.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import tomllib
from dataclasses import dataclass
from pathlib import Path

from host_artifact import HostArtifactError, REQUIRED_MODULES, required_modules_for_source


PROJECT = "s3-hidbot"
TARGET = "esp32s3"
DISTRIBUTION = "s3-hidbot-host"
_RELEASE_VERSION = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
_SOURCE_REVISION = re.compile(r"^[0-9a-f]{40}$")
_BUILD_PROFILE = re.compile(r"^[a-z0-9][a-z0-9-]{0,30}$")
_RAW_STRING_LITERAL = re.compile(r'(?:u8|u|U|L)?R"')
_PHYSICAL_SPLICE = re.compile(r"\\[ \t\v\f]*\r?\n")
_AUTHORITY_DIRECTIVE = re.compile(
    r"(?:pragma[ \t]+once|include[ \t]*<[ \t]*(?:array|cstddef|cstdint|span|string_view)[ \t]*>)[ \t]*"
)


class ReleaseContractError(ValueError):
    """A requested release cannot be identified unambiguously."""


@dataclass(frozen=True)
class _CppToken:
    kind: str
    value: str


def _cpp_tokens(text: str) -> tuple[_CppToken, ...]:
    """Tokenize the bounded header grammar, rejecting unmodeled preprocessing.

    Only the current/frozen standard includes and pragma once are permitted;
    directives are checked as whole lines, never mined for declarations.
    """

    if _RAW_STRING_LITERAL.search(text) is not None:
        raise ReleaseContractError(
            "firmware build profile authority uses an unsupported raw string literal"
        )
    if _PHYSICAL_SPLICE.search(text) is not None:
        raise ReleaseContractError(
            "firmware build profile authority uses unsupported physical line splicing"
        )

    tokens: list[_CppToken] = []
    cursor = 0
    line_start = True
    while cursor < len(text):
        character = text[cursor]
        if character.isspace():
            if character in "\r\n":
                line_start = True
            cursor += 1
            continue
        if text.startswith("//", cursor):
            newline = text.find("\n", cursor + 2)
            cursor = len(text) if newline < 0 else newline
            continue
        if text.startswith("/*", cursor):
            end = text.find("*/", cursor + 2)
            if end < 0:
                raise ReleaseContractError("firmware build profile authority has an unterminated comment")
            line_start = line_start or "\n" in text[cursor : end + 2]
            cursor = end + 2
            continue
        if character == "#":
            end = text.find("\n", cursor)
            end = len(text) if end < 0 else end
            directive = text[cursor + 1 : end].strip()
            if not line_start or _AUTHORITY_DIRECTIVE.fullmatch(directive) is None:
                raise ReleaseContractError(
                    "firmware build profile authority uses unsupported preprocessing syntax"
                )
            tokens.append(_CppToken("directive", directive))
            cursor = end
            line_start = False
            continue
        if not character.isascii() or character in {"%", "\\"}:
            raise ReleaseContractError("firmware build profile authority uses unsupported token syntax")
        line_start = False
        if character.isalpha() or character == "_":
            start = cursor
            cursor += 1
            while cursor < len(text) and (text[cursor].isalnum() or text[cursor] == "_"):
                cursor += 1
            tokens.append(_CppToken("identifier", text[start:cursor]))
            continue
        if character in {'"', "'"}:
            quote = character
            start = cursor + 1
            cursor += 1
            escaped = False
            while cursor < len(text):
                current = text[cursor]
                if current in "\r\n":
                    raise ReleaseContractError(
                        "firmware build profile authority has an unsupported multiline literal"
                    )
                if not escaped and current == quote:
                    break
                if not escaped and current == "\\":
                    escaped = True
                else:
                    escaped = False
                cursor += 1
            if cursor >= len(text):
                raise ReleaseContractError("firmware build profile authority has an unterminated literal")
            tokens.append(_CppToken("string" if quote == '"' else "character", text[start:cursor]))
            cursor += 1
            continue
        tokens.append(_CppToken("symbol", character))
        cursor += 1
    return tuple(tokens)


def _profile_namespace(tokens: tuple[_CppToken, ...], declaration: int) -> None:
    """Require the literal in the one supported namespace, with standard type lookup.

    This is a scope boundary, not a C++ evaluator. Other namespaces, aliases,
    preprocessing operators and rebinding of std are outside the contract.
    """

    opening = (
        _CppToken("identifier", "namespace"),
        _CppToken("identifier", "firmware_identity"),
        _CppToken("symbol", "{"),
    )
    cursor = 0
    while cursor < len(tokens) and tokens[cursor].kind == "directive":
        cursor += 1
    if tokens[cursor : cursor + 3] != opening:
        raise ReleaseContractError("firmware build profile authority requires its direct namespace")
    depth = 1
    for index in range(cursor + 3, len(tokens)):
        token = tokens[index]
        if depth == 0 or token.kind == "directive":
            raise ReleaseContractError("firmware build profile authority has unsupported scope syntax")
        if token.kind == "identifier":
            if token.value in {"namespace", "using", "typedef", "_Pragma", "__pragma", "asm", "__asm", "__asm__"}:
                raise ReleaseContractError("firmware build profile authority has unsupported scope syntax")
            if token.value == "std" and tokens[index + 1 : index + 3] != (
                _CppToken("symbol", ":"), _CppToken("symbol", ":")
            ):
                raise ReleaseContractError("firmware build profile authority cannot rebind std")
        if index == declaration and (
            depth != 1 or tokens[index - 1] not in {_CppToken("symbol", ";"), _CppToken("symbol", "{")}
        ):
            raise ReleaseContractError("firmware build profile authority requires a direct declaration")
        if token == _CppToken("symbol", "{"):
            depth += 1
        elif token == _CppToken("symbol", "}"):
            depth -= 1
    if depth != 0:
        raise ReleaseContractError("firmware build profile authority has unbalanced scope")


def validate_release_version(value: str) -> str:
    if _RELEASE_VERSION.fullmatch(value) is None:
        raise ReleaseContractError(
            "release version must be strict X.Y.Z without prerelease or build metadata"
        )
    return value


def validate_source_revision(value: str) -> str:
    if _SOURCE_REVISION.fullmatch(value) is None:
        raise ReleaseContractError("source revision must be exactly 40 lowercase hexadecimal characters")
    return value


def read_build_profile(source_root: Path) -> str:
    """Read a literal profile under the bounded identity-header grammar."""

    header = (
        source_root.resolve()
        / "firmware/components/firmware_identity/include/firmware_identity/firmware_identity.hpp"
    )
    try:
        text = header.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise ReleaseContractError("could not read firmware build profile authority") from exc
    tokens = _cpp_tokens(text)
    declarations: list[str] = []
    expected_prefix = (
        _CppToken("identifier", "inline"),
        _CppToken("identifier", "constexpr"),
        _CppToken("identifier", "std"),
        _CppToken("symbol", ":"),
        _CppToken("symbol", ":"),
        _CppToken("identifier", "string_view"),
        _CppToken("identifier", "kBuildProfile"),
        _CppToken("symbol", "="),
    )
    occurrences = [
        index
        for index, token in enumerate(tokens)
        if token == _CppToken("identifier", "kBuildProfile")
    ]
    for index in occurrences:
        start = index - 6
        if (
            start >= 0
            and tokens[start : index + 2] == expected_prefix
            and index + 3 < len(tokens)
            and tokens[index + 2].kind == "string"
            and tokens[index + 3] == _CppToken("symbol", ";")
        ):
            declarations.append(tokens[index + 2].value)
    if (
        len(occurrences) != 1
        or len(declarations) != 1
        or _BUILD_PROFILE.fullmatch(declarations[0]) is None
    ):
        raise ReleaseContractError("firmware build profile authority is missing or invalid")
    _profile_namespace(tokens, occurrences[0] - 6)
    return declarations[0]


def release_tag(version: str) -> str:
    return f"v{validate_release_version(version)}"


def validate_release_tag(tag: str, version: str) -> str:
    expected = release_tag(version)
    if tag != expected:
        raise ReleaseContractError(f"release tag must be {expected}, got {tag!r}")
    return tag


@dataclass(frozen=True)
class ReleaseContract:
    """Names and package requirements derived from one selected source tree."""

    version: str
    firmware_version: str
    host_version: str
    build_profile: str
    host_modules: frozenset[str] = REQUIRED_MODULES

    @property
    def tag(self) -> str:
        return release_tag(self.version)

    @property
    def firmware_archive(self) -> str:
        return f"{PROJECT}-firmware-{self.version}-{TARGET}-{self.build_profile}.tar.gz"

    @property
    def host_wheel(self) -> str:
        return f"s3_hidbot_host-{self.version}-py3-none-any.whl"

    @property
    def host_sdist(self) -> str:
        return f"s3_hidbot_host-{self.version}.tar.gz"

    @property
    def distributable_assets(self) -> tuple[str, ...]:
        primary = (self.firmware_archive, self.host_wheel, self.host_sdist)
        return tuple(name for item in primary for name in (item, f"{item}.sha256"))

    @property
    def legal_assets(self) -> tuple[str, ...]:
        primary = ("LICENSE", "THIRD_PARTY_NOTICES.md")
        return tuple(name for item in primary for name in (item, f"{item}.sha256"))

    @property
    def release_assets(self) -> tuple[str, ...]:
        return self.distributable_assets + self.legal_assets


def read_release_contract(source_root: Path) -> ReleaseContract:
    root = source_root.resolve()
    try:
        firmware_version = (root / "firmware" / "version.txt").read_text(encoding="utf-8").strip()
        host_data = tomllib.loads((root / "host" / "pyproject.toml").read_text(encoding="utf-8"))
        host_version = host_data["project"]["version"]
    except (FileNotFoundError, KeyError, TypeError, tomllib.TOMLDecodeError) as exc:
        raise ReleaseContractError("could not read authoritative firmware and host versions") from exc
    if not isinstance(host_version, str):
        raise ReleaseContractError("host project version must be a string")
    firmware_version = validate_release_version(firmware_version)
    host_version = validate_release_version(host_version)
    if firmware_version != host_version:
        raise ReleaseContractError(
            f"combined release requires matching firmware and host versions, got "
            f"{firmware_version} and {host_version}"
        )
    try:
        host_modules = required_modules_for_source(root)
    except HostArtifactError as exc:
        raise ReleaseContractError("could not read selected-source host module authority") from exc
    return ReleaseContract(
        version=firmware_version,
        firmware_version=firmware_version,
        host_version=host_version,
        build_profile=read_build_profile(root),
        host_modules=host_modules,
    )


def _git(repository: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ReleaseContractError(f"Git command failed: {' '.join(arguments)}") from exc
    return result.stdout.strip()


def resolve_checkout_commit(repository: Path) -> str:
    return validate_source_revision(_git(repository, "rev-parse", "--verify", "HEAD"))


def source_date_epoch(repository: Path, commit: str) -> int:
    revision = validate_source_revision(commit)
    try:
        value = int(_git(repository, "show", "-s", "--format=%ct", revision))
    except ValueError as exc:
        raise ReleaseContractError("commit timestamp is not an integer") from exc
    if value < 0:
        raise ReleaseContractError("commit timestamp must not be negative")
    return value


def resolve_annotated_tag_commit(repository: Path, tag: str, version: str) -> str:
    """Resolve an annotated release tag to its peeled commit, never its tag object."""

    validate_release_tag(tag, version)
    tag_ref = f"refs/tags/{tag}"
    object_type = _git(repository, "cat-file", "-t", tag_ref)
    if object_type != "tag":
        raise ReleaseContractError("release tag must be annotated; lightweight tags are rejected")
    commit = validate_source_revision(_git(repository, "rev-parse", "--verify", f"{tag_ref}^{{commit}}"))
    if _git(repository, "cat-file", "-t", commit) != "commit":
        raise ReleaseContractError("annotated release tag does not resolve to a commit")
    return commit


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--tag", help="validate and resolve an annotated vX.Y.Z tag")
    parser.add_argument("--repository", type=Path, help="Git repository used with --tag")
    parser.add_argument("--expected-commit", help="require a particular checked-out commit")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        contract = read_release_contract(args.source_root)
        result: dict[str, object] = {
            "version": contract.version,
            "tag": contract.tag,
            "firmware_archive": contract.firmware_archive,
            "build_profile": contract.build_profile,
            "host_wheel": contract.host_wheel,
            "host_sdist": contract.host_sdist,
            "release_assets": list(contract.release_assets),
        }
        if args.tag is not None:
            if args.repository is None:
                raise ReleaseContractError("--repository is required with --tag")
            commit = resolve_annotated_tag_commit(args.repository, args.tag, contract.version)
            result["source_revision"] = commit
            result["source_date_epoch"] = source_date_epoch(args.repository, commit)
        if args.expected_commit is not None:
            expected = validate_source_revision(args.expected_commit)
            repository = args.repository or args.source_root
            actual = resolve_checkout_commit(repository)
            if actual != expected:
                raise ReleaseContractError(f"checkout commit mismatch: expected {expected}, got {actual}")
            result["checkout_commit"] = actual
    except ReleaseContractError as exc:
        print(f"release contract error: {exc}")
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
