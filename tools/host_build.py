"""Shared isolated source staging and distribution build boundary."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HOST_INPUT_FILES = ("LICENSE", "MANIFEST.in", "README.md", "pyproject.toml")


def copy_host_source(source_root: Path, stage_root: Path) -> Path:
    source_host = source_root / "host"
    destination = stage_root / "host"
    destination.mkdir()
    for name in HOST_INPUT_FILES:
        shutil.copy2(source_host / name, destination / name)
    shutil.copytree(
        source_host / "src" / "hidbot",
        destination / "src" / "hidbot",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    shutil.copytree(
        source_host / "tests",
        destination / "tests",
        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pyo"),
    )
    return destination


def build_host_distributions(
    source_root: Path,
    output_directory: Path,
    *,
    wheel: bool,
    sdist: bool,
    python: str = sys.executable,
) -> tuple[Path, ...]:
    """Build requested host distributions once from an isolated source copy."""

    if not wheel and not sdist:
        raise ValueError("at least one host distribution must be requested")
    output_directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="s3-hidbot-host-build-") as temporary:
        temporary_root = Path(temporary)
        stage = copy_host_source(source_root.resolve(), temporary_root)
        command = [python, "-m", "build", "--outdir", str(temporary_root / "dist")]
        if wheel and not sdist:
            command.append("--wheel")
        elif sdist and not wheel:
            command.append("--sdist")
        command.append(str(stage))
        subprocess.run(command, check=True)
        built = sorted((temporary_root / "dist").iterdir(), key=lambda path: path.name)
        if len(built) != int(wheel) + int(sdist):
            raise RuntimeError("build did not produce the requested number of host distributions")
        copied: list[Path] = []
        for artifact in built:
            if not artifact.is_file():
                raise RuntimeError("host build output is not a regular file")
            destination = output_directory / artifact.name
            if destination.exists():
                raise RuntimeError(f"refusing to overwrite host artifact {artifact.name}")
            shutil.copy2(artifact, destination)
            copied.append(destination)
    return tuple(copied)
