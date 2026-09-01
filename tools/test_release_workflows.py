#!/usr/bin/env python3
"""Static and unit checks for the narrowly scoped release workflow contract."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from release_contract import read_release_contract
from validate_release_build_run import (
    ReleaseBuildRunError,
    validate_release_build_run,
    validate_release_candidate_run,
)


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".github" / "workflows" / "release-build.yml"
DRAFT = ROOT / ".github" / "workflows" / "release-draft.yml"
NOTICES = ROOT / "THIRD_PARTY_NOTICES.md"
PIN = re.compile(r"uses:\s+actions/[A-Za-z0-9_.-]+@([0-9a-f]{40})")


class ReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = BUILD.read_text(encoding="utf-8")
        cls.draft = DRAFT.read_text(encoding="utf-8")
        cls.selector = cls.build.split(
            "      - name: Select exact immutable checkout ref and release mode\n", 1
        )[1].split("      - name: Checkout selected immutable revision\n", 1)[0]
        cls.resolver = cls.build.split(
            "      - name: Resolve exact candidate or annotated tag commit\n", 1
        )[1].split("      - name: Enforce release version authority and static guards\n", 1)[0]

    def test_release_build_is_read_only_temporary_asset_producer(self) -> None:
        self.assertIn("workflow_dispatch:", self.build)
        self.assertIn("tags:", self.build)
        self.assertIn('      - "v*"', self.build)
        self.assertRegex(self.build, r"permissions:\n\s+contents: read\n")
        self.assertNotIn("contents: write", self.build)
        self.assertIn("name: release-assets", self.build)
        self.assertIn("retention-days: 14", self.build)
        self.assertIn("tools/build_firmware_artifact.py", self.build)
        self.assertIn("tools/compare_release_firmware.py", self.build)
        self.assertIn("tools/prepare_release_assets.py", self.build)
        self.assertNotIn("gh release", self.build)
        self.assertNotIn("git tag", self.build)
        self.assertNotIn("idf.py flash", self.build)
        self.assertNotIn("--hardware", self.build)

    def test_dispatch_inputs_and_checkout_selector_are_fail_closed(self) -> None:
        self.assertIn("expected_commit:", self.build)
        self.assertRegex(
            self.build,
            r"expected_tag:\n\s+description:.*\n\s+required: false\n\s+default: \"\"\n\s+type: string",
        )
        self.assertIn("^[0-9a-f]{40}$", self.selector)
        self.assertIn("^v(0|[1-9][0-9]*)", self.selector)
        self.assertIn('mode="candidate"', self.selector)
        self.assertIn('checkout_ref="$INPUT_COMMIT"', self.selector)
        self.assertIn('mode="existing-tag-recovery"', self.selector)
        self.assertIn('checkout_ref="refs/tags/$INPUT_TAG"', self.selector)
        self.assertIn('mode="tag-push"', self.selector)
        self.assertIn('checkout_ref="$EVENT_REF"', self.selector)
        self.assertIn("ref: ${{ steps.select.outputs.checkout_ref }}", self.build)

    def test_candidate_recovery_and_tag_paths_are_immutable(self) -> None:
        self.assertIn('if [[ "$RELEASE_MODE" == "candidate" ]]', self.resolver)
        self.assertIn('expected_sha="$INPUT_COMMIT"', self.resolver)
        self.assertIn('release_tag="$INPUT_TAG"', self.resolver)
        self.assertIn('release_tag="$TAG_NAME"', self.resolver)
        self.assertIn("tools/release_contract.py", self.resolver)
        self.assertIn('--tag "$release_tag"', self.resolver)
        self.assertIn('test "$expected_sha" = "$INPUT_COMMIT"', self.resolver)
        self.assertIn('actual_sha=$(git -c "safe.directory=$GITHUB_WORKSPACE" rev-parse HEAD)', self.resolver)
        self.assertIn('test "$actual_sha" = "$expected_sha"', self.resolver)
        self.assertIn("show -s --format=%ct", self.resolver)
        self.assertIn("S3_HIDBOT_SOURCE_REVISION", self.resolver)

    def test_tag_resolver_inherits_only_workspace_git_trust(self) -> None:
        self.assertIn('GIT_CONFIG_COUNT: "1"', self.resolver)
        self.assertIn("GIT_CONFIG_KEY_0: safe.directory", self.resolver)
        self.assertIn("GIT_CONFIG_VALUE_0: ${{ github.workspace }}", self.resolver)
        self.assertNotIn("safe.directory=*", self.build)
        self.assertNotIn("safe.directory: *", self.build)

    def test_tag_contract_failure_is_visible_without_changing_the_helper(self) -> None:
        self.assertIn("if ! python3 tools/release_contract.py", self.resolver)
        self.assertIn("sed -n '1,20p'", self.resolver)
        self.assertIn('"$RUNNER_TEMP/release-contract.json" >&2', self.resolver)
        self.assertIn("exit 1", self.resolver)

    def test_checkout_free_consumers_cover_both_supported_pythons(self) -> None:
        consumer = self.build.split("  consume:\n", 1)[1]
        self.assertIn('python-version: ["3.11", "3.12"]', consumer)
        self.assertNotIn("actions/checkout@", consumer)
        self.assertIn("sha256sum --check --strict", consumer)
        self.assertIn("verify-artifact", consumer)
        self.assertIn('"$asset_directory/$wheel[flash]"', consumer)
        self.assertIn("-m esptool version", consumer)
        self.assertIn('"$asset_directory/$sdist"', consumer)

    def test_release_draft_is_manual_and_fail_closed(self) -> None:
        self.assertIn("on:\n  workflow_dispatch:", self.draft)
        self.assertNotIn("\n  push:", self.draft)
        self.assertNotIn("\n  pull_request:", self.draft)
        self.assertRegex(self.draft, r"permissions:\n\s+contents: write\n\s+actions: read\n")
        self.assertIn("release_build_run_id:", self.draft)
        self.assertIn("candidate_release_build_run_id:", self.draft)
        self.assertIn("tools/validate_release_build_run.py", self.draft)
        self.assertIn("tools/compare_release_firmware.py", self.draft)
        self.assertIn("--kind tag", self.draft)
        self.assertIn("--kind candidate", self.draft)
        self.assertIn("run-id:", self.draft)
        self.assertIn("gh release view", self.draft)
        self.assertIn("refusing to overwrite", self.draft)
        self.assertIn("gh release create", self.draft)
        self.assertIn("--draft", self.draft)
        self.assertNotIn("--latest", self.draft)
        self.assertNotIn("--prerelease", self.draft)

    def test_release_actions_are_full_sha_pinned(self) -> None:
        for workflow in (self.build, self.draft):
            actions = PIN.findall(workflow)
            self.assertTrue(actions)
            self.assertNotRegex(workflow, r"actions/[A-Za-z0-9_.-]+@v[0-9]")

    def test_selected_tag_build_metadata_must_match_every_immutable_field(self) -> None:
        contract = read_release_contract(ROOT)
        document = {
            "id": 123,
            "name": "Release build",
            "event": "push",
            "head_branch": "v0.1.0",
            "head_sha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "repository": {"full_name": "w-dee/s3-hidbot"},
        }
        validate_release_build_run(
            document,
            repository="w-dee/s3-hidbot",
            tag="v0.1.0",
            version=contract.version,
            commit="a" * 40,
            run_id="123",
        )
        document["head_sha"] = "b" * 40
        with self.assertRaises(ReleaseBuildRunError):
            validate_release_build_run(
                document,
                repository="w-dee/s3-hidbot",
                tag="v0.1.0",
                version=contract.version,
                commit="a" * 40,
                run_id="123",
            )

    def test_selected_candidate_build_must_match_the_tagged_commit(self) -> None:
        document = {
            "id": 456,
            "name": "Release build",
            "event": "workflow_dispatch",
            "head_sha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "repository": {"full_name": "w-dee/s3-hidbot"},
        }
        validate_release_candidate_run(
            document,
            repository="w-dee/s3-hidbot",
            commit="a" * 40,
            run_id="456",
        )
        document["event"] = "push"
        with self.assertRaises(ReleaseBuildRunError):
            validate_release_candidate_run(
                document,
                repository="w-dee/s3-hidbot",
                commit="a" * 40,
                run_id="456",
            )

    def test_release_docs_and_third_party_outcome_are_explicit(self) -> None:
        documents = {
            path: (ROOT / path).read_text(encoding="utf-8")
            for path in (
                "README.md",
                "host/README.md",
                "docs/operator/README.md",
                "docs/operator/quick-start.md",
                "docs/operator/automation.md",
                "docs/development/firmware-artifacts.md",
            )
        }
        combined = "\n".join(documents.values()).lower()
        self.assertIn("when a published version is available", combined)
        self.assertIn("development", combined)
        self.assertIn("not published on pypi", combined)
        self.assertNotIn("v0.1.0 is published", combined)
        notices = NOTICES.read_text(encoding="utf-8")
        for required in (
            "ESP TinyUSB 2.2.1",
            "TinyUSB 0.21.0~1",
            "FreeRTOS",
            "cJSON",
            "Mbed TLS",
            "Xtensa ESP ELF",
            "not a legal opinion",
        ):
            self.assertIn(required, notices)


if __name__ == "__main__":
    result = unittest.main(argv=[__file__], exit=False)
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
