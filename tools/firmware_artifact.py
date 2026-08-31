#!/usr/bin/env python3
"""Source-tree compatibility adapter for the canonical artifact module.

The canonical implementation is installed as ``hidbot.artifact``.  This
adapter loads its source file directly so existing repository tools remain
stdlib-only and do not execute ``hidbot.__init__`` or import pyserial.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


_CANONICAL_PATH = (
    Path(__file__).resolve().parents[1] / "host" / "src" / "hidbot" / "artifact.py"
)
_SPEC = importlib.util.spec_from_file_location("_s3_hidbot_artifact_canonical", _CANONICAL_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise ImportError("could not load the canonical s3-hidbot artifact module")
_MODULE = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _MODULE
_SPEC.loader.exec_module(_MODULE)

for _name, _value in vars(_MODULE).items():
    if not _name.startswith("__"):
        globals()[_name] = _value
