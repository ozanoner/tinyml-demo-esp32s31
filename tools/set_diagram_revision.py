#!/usr/bin/env python3
"""Pin an archify diagram to a repository revision.

archify validates every cited `path:line` against `meta.repository.revision`, so
a diagram is honest only while that pin points at a commit whose sources match.
The pin therefore goes stale as soon as `main` moves. The release gate calls this
to move it to the commit being released, then lets `archify finalize` re-resolve
and re-validate every citation against it.

Only the `revision` value is touched: the rest of the file stays byte-for-byte as
archify wrote it, so the regenerated JSON diffs to one line per diagram.

Usage:
    set_diagram_revision.py --revision <sha> <artifact.json> [<artifact.json> ...]

    # report drift instead of writing (exit 1 if any pin differs)
    set_diagram_revision.py --revision <sha> --check <artifact.json> ...
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REGEX = re.compile(r'("revision"\s*:\s*")([^"]*)(")')
SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")


def pin(path: Path, revision: str, check: bool) -> bool:
    """Return True if the file already carried `revision`."""
    text = path.read_text(encoding="utf-8")

    matches = REGEX.findall(text)
    if len(matches) != 1:
        raise SystemExit(
            f"{path}: expected exactly one \"revision\" key, found {len(matches)}"
        )

    current = matches[0][1]
    if current == revision:
        return True
    if check:
        print(f"{path}: pinned to {current}, expected {revision}")
        return False

    updated = REGEX.sub(lambda m: f"{m.group(1)}{revision}{m.group(3)}", text, count=1)
    # Fail loudly rather than write a file we cannot parse back.
    parsed = json.loads(updated)
    if parsed["meta"]["repository"]["revision"] != revision:
        raise SystemExit(f"{path}: revision did not take")

    path.write_text(updated, encoding="utf-8")
    print(f"{path}: {current} -> {revision}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifacts", nargs="+", type=Path, help="archify .json artifacts")
    parser.add_argument("--revision", required=True, help="commit the diagrams should cite")
    parser.add_argument("--check", action="store_true",
                        help="report drift and exit 1 instead of writing")
    args = parser.parse_args()

    if not SHA_RE.match(args.revision):
        raise SystemExit(f"not a commit sha: {args.revision!r}")

    unchanged = [pin(p, args.revision, args.check) for p in args.artifacts]
    if args.check and not all(unchanged):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
