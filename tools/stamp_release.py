#!/usr/bin/env python3
"""Stamp the release tag into the generated diagram artefacts.

The expensive half of the diagram gate — re-resolving every cited `path:line`
against the pinned revision with archify, plus the visual check — runs once, in the
pre-release step, on the machine that regenerates the diagrams. This writes the
stamp that records which release those artefacts belong to:
`tools/check_release_stamp.py` is what reads it back at release time.

The stamp is an HTML comment, for two reasons:

- archify validates the JSON and rejects unknown keys under `meta`
  (`/meta must NOT have additional properties {"additionalProperty":"release"}`),
  so there is nowhere schema-clean for it to live there;
- anything written into the generated HTML is wiped by the next regeneration, and
  that is the desired behaviour: a regenerated diagram must be re-stamped, and an
  unstamped one fails the tag rather than shipping quietly.

Format, one line at the end of the file:

    <!-- release: v1.2.0 @ 0123456789abcdef0123456789abcdef01234567 -->

The commit written is taken from the sibling JSON's `meta.repository.revision`, so
the stamp can never disagree with the pin the citations were validated against.

Usage:
    stamp_release.py --tag v1.2.0 <artifact.html> [<artifact.html> ...]

Then, before tagging:  check_release_stamp.py --tag v1.2.0
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

STAMP_RE = re.compile(r"<!-- release: (\S+) @ ([0-9a-f]{7,40}) -->")


def pin_of(artifact: Path) -> str:
    """The revision the sibling JSON pins — what the stamp must record."""
    source = artifact.with_suffix(".json")
    if not source.exists():
        raise SystemExit(f"{artifact}: no sibling {source.name} to read the pin from")
    revision = json.loads(source.read_text(encoding="utf-8"))["meta"]["repository"]["revision"]
    if not re.fullmatch(r"[0-9a-f]{7,40}", revision):
        raise SystemExit(f"{source}: revision is not a commit sha: {revision!r}")
    return revision


def stamp(path: Path, tag: str) -> None:
    text = path.read_text(encoding="utf-8")
    commit = pin_of(path)

    found = STAMP_RE.findall(text)
    if len(found) > 1:
        raise SystemExit(f"{path}: {len(found)} release stamps, expected at most one")

    stripped = STAMP_RE.sub("", text).rstrip()
    path.write_text(f"{stripped}\n<!-- release: {tag} @ {commit} -->\n", encoding="utf-8")

    if found and found[0] == (tag, commit):
        print(f"{path}: already stamped {tag} @ {commit}")
    else:
        print(f"{path}: stamped {tag} @ {commit}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifacts", nargs="+", type=Path, help="generated .html artifacts")
    parser.add_argument("--tag", required=True, help="release tag, e.g. v1.2.0")
    args = parser.parse_args()

    if not re.fullmatch(r"v[0-9][0-9A-Za-z.\-+]*", args.tag):
        raise SystemExit(f"not a release tag: {args.tag!r}")

    for artifact in args.artifacts:
        stamp(artifact, args.tag)
    return 0


if __name__ == "__main__":
    sys.exit(main())
