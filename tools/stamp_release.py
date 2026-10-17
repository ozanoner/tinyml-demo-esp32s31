#!/usr/bin/env python3
"""Stamp the release tag into the generated diagram artefacts.

The expensive half of the diagram gate — re-resolving every cited `path:line`
against the pinned revision with archify, plus the visual check — runs once, in the
pre-release step, on the machine that regenerates the diagrams. What the release
build actually needs is far cheaper: *were these artefacts stamped for this tag?*
That is two greps, not a Node toolchain and a browser download.

The stamp is an HTML comment, for two reasons:

- archify validates the JSON and rejects unknown keys under `meta`
  (`/meta must NOT have additional properties {"additionalProperty":"release"}`),
  so there is nowhere schema-clean for it to live there;
- anything written into the generated HTML is wiped by the next regeneration, and
  that is the desired behaviour: a regenerated diagram must be re-stamped, and an
  unstamped one fails the tag rather than shipping quietly.

Format, one line at the end of the file:

    <!-- release: v1.2.0 @ 0123456789abcdef0123456789abcdef01234567 -->

Usage:
    stamp_release.py --tag v1.2.0 <artifact.html> [<artifact.html> ...]
    stamp_release.py --tag v1.2.0 --check <artifact.html> [...]   # exit 1 on mismatch

The commit written is taken from the sibling JSON's `meta.repository.revision`, so
the stamp can never disagree with the pin the citations were validated against.
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


def stamp(path: Path, tag: str, check: bool) -> bool:
    text = path.read_text(encoding="utf-8")
    commit = pin_of(path)
    found = STAMP_RE.findall(text)

    if len(found) > 1:
        raise SystemExit(f"{path}: {len(found)} release stamps, expected at most one")

    if check:
        if not found:
            print(f"{path}: no release stamp")
            return False
        if found[0] != (tag, commit):
            print(f"{path}: stamped {found[0][0]} @ {found[0][1]}, expected {tag} @ {commit}")
            return False
        return True

    stripped = STAMP_RE.sub("", text).rstrip()
    path.write_text(f"{stripped}\n<!-- release: {tag} @ {commit} -->\n", encoding="utf-8")
    if found and found[0] == (tag, commit):
        print(f"{path}: already stamped {tag} @ {commit}")
    else:
        print(f"{path}: stamped {tag} @ {commit}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifacts", nargs="+", type=Path, help="generated .html artifacts")
    parser.add_argument("--tag", required=True, help="release tag, e.g. v1.2.0")
    parser.add_argument("--check", action="store_true",
                        help="verify the stamps instead of writing them")
    args = parser.parse_args()

    if not re.fullmatch(r"v[0-9][0-9A-Za-z.\-+]*", args.tag):
        raise SystemExit(f"not a release tag: {args.tag!r}")

    results = [stamp(p, args.tag, args.check) for p in args.artifacts]
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
