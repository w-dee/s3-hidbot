#!/usr/bin/env python3
"""Static contract checks for the U6.4A host-package Actions artifact."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github/workflows/nonhardware.yml"


def job_block(text: str, name: str, next_name: str) -> str:
    marker = f"  {name}:\n"
    next_marker = f"  {next_name}:\n"
    return text.split(marker, 1)[1].split(next_marker, 1)[0]


def required(text: str, pattern: str, description: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE) is None:
        raise AssertionError(f"missing U6.4A workflow contract: {description}")


def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8")
    producer = job_block(text, "host-package-producer", "host-package-consumer")
    consumer = job_block(text, "host-package-consumer", "host-package")
    if "matrix:" in producer:
        raise AssertionError("host artifact producer must not be matrixed")
    required(producer, r"python-version:\s*[\"']3\.12[\"']", "fixed producer Python 3.12")
    required(producer, r"tools/build_host_artifact\.py", "canonical producer helper")
    required(producer, r"tools/test_host_artifact\.py", "artifact validation")
    required(producer, r"actions/upload-artifact@v4", "upload-artifact v4")
    required(producer, r"name:\s*host-package", "stable artifact name")
    required(producer, r"retention-days:\s*14", "14-day retention")
    if ".tar.gz" in producer or "--sdist" in producer:
        raise AssertionError("producer must not distribute an sdist")

    required(consumer, r"needs:\s*host-package-producer", "consumer depends on producer")
    required(consumer, r"python-version:\s*\[\"3\.11\",\s*\"3\.12\"\]", "consumer Python matrix")
    required(consumer, r"actions/download-artifact@v4", "download-artifact v4")
    required(consumer, r"name:\s*host-package", "same artifact download name")
    required(consumer, r"sha256sum\s+--check\s+--strict", "checksum verification before install")
    required(consumer, r"python\s+-m\s+venv", "fresh consumer virtual environment")
    required(consumer, r"pip\s+install[^\n]*\$wheel", "normal wheel install")
    required(consumer, r"pip\s+check", "consumer pip check")
    required(consumer, r"site-packages", "installed import origin proof")
    required(consumer, r"consumer_cli.*--help", "installed CLI smoke")
    required(consumer, r"!\s+-e\s+\"\$GITHUB_WORKSPACE/host\"", "no source checkout proof")
    if "actions/checkout@" in consumer:
        raise AssertionError("artifact consumer must not checkout repository source")
    if "tools/" in consumer:
        raise AssertionError("artifact consumer must not invoke repository helpers")
    checksum_position = consumer.index("sha256sum --check --strict")
    install_position = consumer.index('"$consumer_python" -m pip install')
    if checksum_position >= install_position:
        raise AssertionError("artifact consumer must verify checksum before wheel installation")
    print("PASS: non-hardware canonical host artifact CI static contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
