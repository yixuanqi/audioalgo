from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .validation import validate_dataset


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="audioalgo-eval")
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate a dataset manifest and its payloads")
    validate.add_argument("--dataset", required=True, type=Path, help="path to manifest.json")
    return parser


def main(arguments: list[str] | None = None) -> int:
    options = _parser().parse_args(arguments)
    if options.command == "validate":
        result = validate_dataset(options.dataset)
        print(json.dumps(result, ensure_ascii=False, sort_keys=True, allow_nan=False))
        return 0 if result["status"] == "passed" else 2
    return 3


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
