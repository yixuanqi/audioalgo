from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


MAX_RAW_PAYLOAD_BYTES = 8 * 1024 * 1024 * 1024
HASH_CHUNK_BYTES = 1024 * 1024


def _diagnostic(code: str, message: str, **details: Any) -> dict[str, Any]:
    return {"code": code, "message": message, **details}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(HASH_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def _inside_dataset(root: Path, relative_path: str) -> Path | None:
    candidate = Path(relative_path)
    if candidate.is_absolute():
        return None
    try:
        resolved = (root / candidate).resolve(strict=True)
        resolved.relative_to(root)
        return resolved
    except (OSError, ValueError):
        return None


def validate_dataset(manifest_path: str | Path) -> dict[str, Any]:
    """Validate the v1 raw float32 dataset contract without modifying its files."""
    source = Path(manifest_path)
    diagnostics: list[dict[str, Any]] = []
    try:
        manifest_file = source.resolve(strict=True)
        raw = manifest_file.read_text(encoding="utf-8")
        manifest = json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return {
            "status": "failed",
            "diagnostics": [
                _diagnostic("manifest_unreadable", "dataset manifest could not be read", error=str(error))
            ],
        }

    if not isinstance(manifest, dict):
        return {
            "status": "failed",
            "diagnostics": [_diagnostic("manifest_invalid", "manifest root must be a JSON object")],
        }
    if type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1:
        diagnostics.append(
            _diagnostic(
                "unsupported_schema",
                "schema_version must be the supported integer version 1",
                field="schema_version",
            )
        )

    assets = manifest.get("assets")
    if not isinstance(assets, list):
        diagnostics.append(_diagnostic("manifest_invalid", "assets must be an array", field="assets"))
        assets = []

    asset_ids: set[str] = set()
    dataset_root = manifest_file.parent.resolve()
    for asset_index, asset in enumerate(assets):
        prefix = f"assets[{asset_index}]"
        if not isinstance(asset, dict):
            diagnostics.append(_diagnostic("manifest_invalid", "asset must be an object", asset_index=asset_index))
            continue

        asset_id = asset.get("asset_id")
        if not isinstance(asset_id, str) or not asset_id:
            diagnostics.append(_diagnostic("manifest_invalid", "asset_id must be a non-empty string", field=f"{prefix}.asset_id"))
        elif asset_id in asset_ids:
            diagnostics.append(_diagnostic("duplicate_asset_id", "asset_id must be unique", asset_id=asset_id))
        else:
            asset_ids.add(asset_id)

        frame_count = asset.get("frame_count")
        channel_ids = asset.get("channel_ids")
        if type(frame_count) is not int or frame_count < 0:
            diagnostics.append(_diagnostic("manifest_invalid", "frame_count must be a non-negative integer", field=f"{prefix}.frame_count"))
            continue
        if not isinstance(channel_ids, list) or not channel_ids or any(not isinstance(item, str) or not item for item in channel_ids):
            diagnostics.append(_diagnostic("manifest_invalid", "channel_ids must contain non-empty stable IDs", field=f"{prefix}.channel_ids"))
            continue
        if len(set(channel_ids)) != len(channel_ids):
            diagnostics.append(_diagnostic("duplicate_channel_id", "channel_ids must be unique", asset_id=asset_id))
            continue

        dtype = asset.get("dtype")
        container = asset.get("container")
        if container != "raw" or dtype != "float32_le":
            diagnostics.append(
                _diagnostic(
                    "unsupported_payload_format",
                    "this validator currently accepts raw float32_le assets",
                    asset_id=asset_id,
                    container=container,
                    dtype=dtype,
                )
            )
            continue
        if asset.get("layout") not in ("planar", "interleaved"):
            diagnostics.append(_diagnostic("manifest_invalid", "layout must be planar or interleaved", field=f"{prefix}.layout"))
            continue

        relative_path = asset.get("path")
        if not isinstance(relative_path, str) or not relative_path:
            diagnostics.append(_diagnostic("manifest_invalid", "path must be a non-empty relative path", field=f"{prefix}.path"))
            continue
        payload_path = _inside_dataset(dataset_root, relative_path)
        if payload_path is None or not payload_path.is_file():
            diagnostics.append(_diagnostic("payload_missing", "payload is missing or escapes the dataset directory", asset_id=asset_id, path=relative_path))
            continue

        bytes_per_frame = len(channel_ids) * 4
        expected_bytes = frame_count * bytes_per_frame
        if expected_bytes > MAX_RAW_PAYLOAD_BYTES:
            diagnostics.append(
                _diagnostic(
                    "payload_resource_limit",
                    "declared payload exceeds the configured validation limit",
                    asset_id=asset_id,
                    expected_bytes=expected_bytes,
                    max_bytes=MAX_RAW_PAYLOAD_BYTES,
                )
            )
            continue
        actual_bytes = payload_path.stat().st_size
        if actual_bytes != expected_bytes:
            details: dict[str, Any] = {
                "asset_id": asset_id,
                "path": relative_path,
                "expected_bytes": expected_bytes,
                "actual_bytes": actual_bytes,
                "expected_frames": frame_count,
                "bytes_per_frame": bytes_per_frame,
            }
            if actual_bytes % bytes_per_frame == 0:
                details["actual_frames"] = actual_bytes // bytes_per_frame
            diagnostics.append(
                _diagnostic("payload_size_mismatch", "payload byte count does not match declared frames and channels", **details)
            )
            continue

        expected_hash = asset.get("sha256")
        if not isinstance(expected_hash, str) or len(expected_hash) != 64:
            diagnostics.append(_diagnostic("manifest_invalid", "sha256 must be a 64-character hex digest", field=f"{prefix}.sha256"))
            continue
        try:
            actual_hash = _sha256(payload_path)
        except OSError as error:
            diagnostics.append(_diagnostic("payload_unreadable", "payload could not be hashed", asset_id=asset_id, error=str(error)))
            continue
        if actual_hash.lower() != expected_hash.lower():
            diagnostics.append(
                _diagnostic(
                    "payload_hash_mismatch",
                    "payload SHA-256 does not match the manifest",
                    asset_id=asset_id,
                    expected_sha256=expected_hash.lower(),
                    actual_sha256=actual_hash,
                )
            )

    return {"status": "passed" if not diagnostics else "failed", "diagnostics": diagnostics}
