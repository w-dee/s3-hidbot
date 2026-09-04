#!/usr/bin/env python3
"""Static and unit checks for the narrowly scoped release workflow contract."""

from __future__ import annotations

import copy
import re
import unittest
from pathlib import Path

from release_contract import read_release_contract
from validate_release_build_run import (
    ReleaseBuildRunError,
    validate_release_build_run,
    validate_release_candidate_run,
    validate_release_recovery_run,
)


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".github" / "workflows" / "release-build.yml"
DRAFT = ROOT / ".github" / "workflows" / "release-draft.yml"
FIRMWARE_ARTIFACT = ROOT / ".github" / "workflows" / "firmware-artifact.yml"
NONHARDWARE = ROOT / ".github" / "workflows" / "nonhardware.yml"
NOTICES = ROOT / "THIRD_PARTY_NOTICES.md"
PIN = re.compile(r"uses:\s+actions/[A-Za-z0-9_.-]+@([0-9a-f]{40})")
WORKFLOW_PATH = ".github/workflows/release-build.yml"


def run_document(
    *,
    run_id: int,
    event: str,
    head_sha: str,
    head_branch: str = "main",
    conclusion: str = "success",
) -> dict:
    """Return a realistic subset of a GitHub REST workflow-run document."""

    return {
        "id": run_id,
        "name": "Release build",
        "event": event,
        "head_branch": head_branch,
        "head_sha": head_sha,
        "status": "completed",
        "conclusion": conclusion,
        "path": WORKFLOW_PATH,
        "repository": {
            "id": 123456,
            "node_id": "R_fixture",
            "name": "s3-hidbot",
            "full_name": "w-dee/s3-hidbot",
            "private": False,
            "html_url": "https://github.com/w-dee/s3-hidbot",
        },
    }


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
        self.assertNotIn("recovery_workflow_commit:", self.draft)
        self.assertIn('test "$GITHUB_REF" = "refs/heads/main"', self.draft)
        self.assertIn("ref: ${{ github.sha }}", self.draft)
        self.assertIn("ref: refs/tags/${{ inputs.tag }}", self.draft)
        self.assertIn("path: control", self.draft)
        self.assertIn("path: release-source", self.draft)
        self.assertGreaterEqual(self.draft.count("persist-credentials: false"), 2)
        self.assertIn('test "$control_head" = "$GITHUB_SHA"', self.draft)
        self.assertIn('"$control_root/tools/validate_release_build_run.py"', self.draft)
        self.assertIn('"$control_root/tools/verify_release_assets.py"', self.draft)
        self.assertIn('"$control_root/tools/compare_release_firmware.py"', self.draft)
        self.assertIn('"$control_root/tools/render_release_notes.py"', self.draft)
        self.assertIn('--source-root "$source_root"', self.draft)
        self.assertIn('"$source_root/docs/development/release-notes-v0.1.0.md"', self.draft)
        self.assertIn("EXPECTED_TAG_OBJECT_SHA: 0180ab16241814b6b7d8cbd45878cf3ae5ddfeee", self.draft)
        self.assertIn("EXPECTED_RELEASE_SOURCE: 9f80ace9e1f7112b8b12a3984a21522c9d4fa1a5", self.draft)
        self.assertIn("RECOVERY_WORKFLOW_COMMIT: f01bcded0fe3851ce53d10462714afc03cfa0871", self.draft)
        self.assertIn("EXPECTED_FIRMWARE_SHA256: 861e2ead3673e620526ebfaea82a987da6ecfb3777613af9e9df46e70530dbde", self.draft)
        self.assertIn("EXPECTED_APP_ELF_SHA256: fdd9277138c8aacf62a517b9823f58d3a4f0ebc4668caf7941b1ea675024f4f8", self.draft)
        self.assertIn('test "$tag_object" = "$EXPECTED_TAG_OBJECT_SHA"', self.draft)
        self.assertIn('test "$commit" = "$EXPECTED_RELEASE_SOURCE"', self.draft)
        self.assertIn('--workflow-commit "$RECOVERY_WORKFLOW_COMMIT"', self.draft)
        self.assertIn('sha256sum "$recovery_firmware"', self.draft)
        self.assertIn('sha256sum "$candidate_firmware"', self.draft)
        self.assertIn('"$EXPECTED_APP_ELF_SHA256"', self.draft)
        self.assertIn("--kind recovery", self.draft)
        self.assertIn("--kind candidate", self.draft)
        self.assertIn("run-id:", self.draft)
        self.assertIn('cmp -- "$recovery_assets/LICENSE" "$source_root/LICENSE"', self.draft)
        self.assertIn('cmp -- "$recovery_assets/THIRD_PARTY_NOTICES.md"', self.draft)
        self.assertIn('cd "$source_root"', self.draft)
        self.assertIn('"$control_root/tools/privacy_lint.py" --tracked', self.draft)
        self.assertNotIn("safe.directory=*", self.draft)
        self.assertNotIn("safe.directory: *", self.draft)
        self.assertIn("gh release view", self.draft)
        self.assertIn("refusing to overwrite", self.draft)
        self.assertIn("gh release create", self.draft)
        self.assertLess(self.draft.index("gh release view"), self.draft.index("gh release create"))
        create_step = self.draft.split(
            "      - name: Create one GitHub draft release, never publish it\n", 1
        )[1]
        self.assertNotIn("\n      - name:", create_step)
        self.assertIn('"$RUNNER_TEMP/recovery-release-assets/"*', create_step)
        self.assertNotIn("candidate-release-assets", create_step)
        self.assertIn("--draft", self.draft)
        self.assertNotIn("--latest", self.draft)
        self.assertNotIn("--prerelease", self.draft)
        self.assertNotIn("pypi", self.draft.lower())

    def test_release_actions_are_full_sha_pinned(self) -> None:
        for workflow in (self.build, self.draft):
            actions = PIN.findall(workflow)
            self.assertTrue(actions)
            self.assertNotRegex(workflow, r"actions/[A-Za-z0-9_.-]+@v[0-9]")

    def test_generic_workflows_derive_product_names_without_version_literals(self) -> None:
        for path in (BUILD, FIRMWARE_ARTIFACT, NONHARDWARE):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("0.1.0", text, str(path.relative_to(ROOT)))
        self.assertIn("tools/release_contract.py", self.build)
        self.assertIn("product-version:", self.build)
        self.assertIn("firmware-name:", self.build)
        self.assertIn("wheel-name:", self.build)
        self.assertIn("sdist-name:", self.build)
        self.assertIn("PRODUCT_VERSION", self.build)

    def test_v010_recovery_workflow_remains_explicitly_version_locked(self) -> None:
        self.assertIn("release-notes-v0.1.0.md", self.draft)
        self.assertIn("EXPECTED_TAG_OBJECT_SHA", self.draft)
        self.assertIn("RECOVERY_WORKFLOW_COMMIT", self.draft)

    def test_selected_tag_build_metadata_must_match_every_immutable_field(self) -> None:
        contract = read_release_contract(ROOT)
        document = run_document(
            run_id=123,
            event="push",
            head_branch=contract.tag,
            head_sha="a" * 40,
        )
        validate_release_build_run(
            document,
            repository="w-dee/s3-hidbot",
            tag=contract.tag,
            version=contract.version,
            commit="a" * 40,
            run_id="123",
        )
        document["head_sha"] = "b" * 40
        with self.assertRaises(ReleaseBuildRunError):
            validate_release_build_run(
                document,
                repository="w-dee/s3-hidbot",
                tag=contract.tag,
                version=contract.version,
                commit="a" * 40,
                run_id="123",
            )

    def test_selected_candidate_build_must_match_the_tagged_commit(self) -> None:
        document = run_document(run_id=456, event="workflow_dispatch", head_sha="a" * 40)
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

    def test_candidate_rejects_wrong_source_and_workflow_path(self) -> None:
        baseline = run_document(run_id=456, event="workflow_dispatch", head_sha="a" * 40)
        for field, value in (("head_sha", "b" * 40), ("path", ".github/workflows/other.yml")):
            with self.subTest(field=field):
                document = copy.deepcopy(baseline)
                document[field] = value
                with self.assertRaises(ReleaseBuildRunError):
                    validate_release_candidate_run(
                        document,
                        repository="w-dee/s3-hidbot",
                        commit="a" * 40,
                        run_id="456",
                    )

    def test_repository_full_name_is_required_inside_a_realistic_object(self) -> None:
        baseline = run_document(run_id=456, event="workflow_dispatch", head_sha="a" * 40)
        validate_release_candidate_run(
            baseline,
            repository="w-dee/s3-hidbot",
            commit="a" * 40,
            run_id="456",
        )
        invalid_repositories = (
            None,
            "w-dee/s3-hidbot",
            {},
            {"full_name": "w-dee/not-s3-hidbot", "id": 123456},
        )
        for repository_document in invalid_repositories:
            with self.subTest(repository=repository_document):
                document = copy.deepcopy(baseline)
                if repository_document is None:
                    document.pop("repository")
                else:
                    document["repository"] = repository_document
                with self.assertRaises(ReleaseBuildRunError):
                    validate_release_candidate_run(
                        document,
                        repository="w-dee/s3-hidbot",
                        commit="a" * 40,
                        run_id="456",
                    )

    def test_selected_recovery_run_uses_exact_reviewed_workflow(self) -> None:
        workflow_commit = "f" * 40
        baseline = run_document(
            run_id=789,
            event="workflow_dispatch",
            head_branch="main",
            head_sha=workflow_commit,
        )
        validate_release_recovery_run(
            baseline,
            repository="w-dee/s3-hidbot",
            workflow_commit=workflow_commit,
            run_id="789",
        )
        mutations = (
            ("event", "push"),
            ("head_branch", "feature/not-main"),
            ("head_sha", "e" * 40),
            ("path", ".github/workflows/other.yml"),
            ("status", "in_progress"),
            ("conclusion", "failure"),
            ("repository", {"full_name": "w-dee/not-s3-hidbot", "id": 123456}),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                document = copy.deepcopy(baseline)
                document[field] = value
                with self.assertRaises(ReleaseBuildRunError):
                    validate_release_recovery_run(
                        document,
                        repository="w-dee/s3-hidbot",
                        workflow_commit=workflow_commit,
                        run_id="789",
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
