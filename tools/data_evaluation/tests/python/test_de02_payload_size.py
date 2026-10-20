from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[4]
PYTHON_PACKAGE_ROOT = REPOSITORY_ROOT / "tools" / "data_evaluation" / "python"


class PayloadSizeContractTests(unittest.TestCase):
    def test_declared_513_frames_with_512_frame_payload_is_rejected(self) -> None:
        # All values have equal magnitude, so the declared frame count cannot be
        # inferred from peak or RMS. The payload's byte count is the authority.
        samples = [0.25 if index % 2 == 0 else -0.25 for index in range(512)]
        payload = struct.pack("<512f", *samples)

        with tempfile.TemporaryDirectory(
            prefix="de02b-", dir=REPOSITORY_ROOT / "build" / "data_evaluation"
        ) as temporary_directory:
            dataset_dir = Path(temporary_directory)
            payload_path = dataset_dir / "payload.f32le"
            payload_path.write_bytes(payload)
            manifest = {
                "schema_version": 1,
                "dataset_id": "de02b_declared_513_actual_512",
                "recipe_version": "1.0.0",
                "assets": [
                    {
                        "asset_id": "input",
                        "role": "input",
                        "path": "payload.f32le",
                        "container": "raw",
                        "dtype": "float32_le",
                        "layout": "planar",
                        "sample_rate_hz": 48000,
                        "frame_count": 513,
                        "channel_ids": ["mono"],
                        "logical_start_frame": 0,
                        "clock_domain": "de02b-test-clock",
                        "valid_frame_range": [0, 513],
                        "sha256": hashlib.sha256(payload).hexdigest(),
                    }
                ],
            }
            manifest_path = dataset_dir / "manifest.json"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            environment = os.environ.copy()
            old_python_path = environment.get("PYTHONPATH")
            environment["PYTHONPATH"] = str(PYTHON_PACKAGE_ROOT) + (
                os.pathsep + old_python_path if old_python_path else ""
            )
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            completed = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "audioalgo_eval",
                    "validate",
                    "--dataset",
                    str(manifest_path),
                ],
                cwd=REPOSITORY_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(
                completed.returncode,
                2,
                msg=f"stdout={completed.stdout!r}\nstderr={completed.stderr!r}",
            )
            response = json.loads(completed.stdout)
            self.assertEqual(response["status"], "failed")
            diagnostic = next(
                item
                for item in response["diagnostics"]
                if item["code"] == "payload_size_mismatch"
            )
            self.assertEqual(diagnostic["expected_bytes"], 513 * 4)
            self.assertEqual(diagnostic["actual_bytes"], 512 * 4)
            self.assertFalse((dataset_dir / "dataset_complete.json").exists())
            self.assertFalse((dataset_dir / "evaluation_complete.json").exists())


if __name__ == "__main__":
    unittest.main()
