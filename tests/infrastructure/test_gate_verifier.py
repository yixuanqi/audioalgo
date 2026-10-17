#!/usr/bin/env python3
"""Small infrastructure contract checks using only the Python standard library."""

import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("audioalgo_verify_gate", ROOT / "scripts" / "verify_gate.py")
verify_gate = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(verify_gate)


class GateInventoryContract(unittest.TestCase):
    def test_G0_required_ids_match_the_acceptance_document(self):
        self.assertEqual(
            verify_gate.required_test_ids("G0"),
            {"BLD-01", "BLD-02", "BLD-03", "BLD-04", "LOOP-01"},
        )

    def test_G1_is_cumulative_and_includes_its_gate_ids(self):
        actual = verify_gate.required_test_ids("G1")
        self.assertEqual(len(actual), 15)
        self.assertTrue({"BLD-01", "LOOP-01", "CORE-01", "DSP-01", "E2E-01"}.issubset(actual))

    def test_unknown_gate_is_rejected(self):
        with self.assertRaises(ValueError):
            verify_gate.required_test_ids("G7")


if __name__ == "__main__":
    unittest.main(verbosity=2)
