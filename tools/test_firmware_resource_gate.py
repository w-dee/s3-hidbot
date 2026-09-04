#!/usr/bin/env python3
"""Focused tests for authoritative firmware resource-gate measurement."""

from __future__ import annotations

import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

from firmware_resource_gate import (
    APPLICATION_LIMIT,
    STATIC_RAM_LIMIT,
    ResourceGateError,
    ResourceUsage,
    enforce_resource_gate,
    measure_and_enforce,
    measure_resources,
    parse_static_ram,
)


def size_document(**updates: object) -> dict[str, object]:
    document: dict[str, object] = {
        "dram_data": 10,
        "dram_bss": 20,
        "dram_rodata": 0,
        "dram_other": 0,
        "used_dram": 30,
        "dram_total": 100,
        "used_dram_ratio": 0.3,
        "dram_remain": 70,
        "iram_vectors": 1,
        "iram_text": 2,
        "iram_other": 0,
        "used_iram": 3,
        "iram_total": 100,
        "used_iram_ratio": 0.03,
        "iram_remain": 97,
        "diram_data": 30,
        "diram_bss": 40,
        "diram_text": 50,
        "diram_vectors": 0,
        "diram_rodata": 0,
        "diram_other": 0,
        "diram_total": 200,
        "used_diram": 120,
        "used_diram_ratio": 0.6,
        "diram_remain": 80,
        "flash_code": 100,
        "flash_rodata": 200,
        "flash_other": 1,
        "used_flash_non_ram": 301,
        "total_size": 424,
    }
    document.update(updates)
    return document


class FirmwareResourceGateTests(unittest.TestCase):
    def test_static_ram_is_dram_and_diram_data_plus_bss(self) -> None:
        self.assertEqual(parse_static_ram(json.dumps(size_document())), 100)

    def test_size_schema_and_values_fail_closed(self) -> None:
        malformed = (
            "not-json",
            "[]",
            json.dumps({}),
            json.dumps(size_document(unexpected=1)),
            json.dumps(size_document(diram_bss=-1)),
            json.dumps(size_document(diram_bss=True)),
            json.dumps(size_document(used_diram_ratio="0.6")),
        )
        for value in malformed:
            with self.subTest(value=value), self.assertRaises(ResourceGateError):
                parse_static_ram(value)

    def test_exact_limits_pass_and_report_zero_margin(self) -> None:
        output = io.StringIO()
        enforce_resource_gate(ResourceUsage(APPLICATION_LIMIT, STATIC_RAM_LIMIT), output)
        text = output.getvalue()
        self.assertIn(f"application measured={APPLICATION_LIMIT} limit={APPLICATION_LIMIT} remaining=0 PASS", text)
        self.assertIn(f"static_ram measured={STATIC_RAM_LIMIT} limit={STATIC_RAM_LIMIT} remaining=0 PASS", text)
        self.assertTrue(text.endswith("RESOURCE_GATE=PASS\n"))

    def test_each_limit_excess_fails_after_reporting_both_metrics(self) -> None:
        for usage in (
            ResourceUsage(APPLICATION_LIMIT + 1, STATIC_RAM_LIMIT),
            ResourceUsage(APPLICATION_LIMIT, STATIC_RAM_LIMIT + 1),
        ):
            output = io.StringIO()
            with self.subTest(usage=usage), self.assertRaises(ResourceGateError):
                enforce_resource_gate(usage, output)
            self.assertEqual(output.getvalue().count("RESOURCE_GATE "), 2)
            self.assertIn(" FAIL\n", output.getvalue())

    def test_measurement_uses_bin_size_and_esp_idf_legacy_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            application = root / "app.bin"
            application.write_bytes(b"x" * 123)
            map_file = root / "app.map"
            map_file.write_text("map fixture\n", encoding="utf-8")
            idf_size = root / "idf_size.py"
            idf_size.write_text(
                "import json\n"
                f"print(json.dumps({size_document()!r}))\n",
                encoding="utf-8",
            )
            usage = measure_resources(application, map_file, idf_size)
            self.assertEqual(usage, ResourceUsage(application=123, static_ram=100))

    def test_missing_measurement_inputs_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / "missing"
            with self.assertRaises(ResourceGateError):
                measure_resources(missing, missing, missing)

    def test_unavailable_measurement_reports_both_limits_and_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing"
            error = io.StringIO()
            with redirect_stderr(error), self.assertRaises(ResourceGateError):
                measure_and_enforce(missing, missing, missing)
            text = error.getvalue()
            self.assertIn(f"application measured=UNAVAILABLE limit={APPLICATION_LIMIT}", text)
            self.assertIn(f"static_ram measured=UNAVAILABLE limit={STATIC_RAM_LIMIT}", text)
            self.assertTrue(text.endswith("RESOURCE_GATE=FAIL\n"))


if __name__ == "__main__":
    result = unittest.main(argv=[__file__], exit=False)
    raise SystemExit(0 if result.result.wasSuccessful() else 1)
