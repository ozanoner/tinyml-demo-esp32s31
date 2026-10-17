#!/usr/bin/env python3
"""Verify that the committed diagrams were stamped for the release being built.

`stamp_release.py` writes the stamp in the pre-release step, after the diagrams have
been regenerated and every cited `path:line` re-validated. This is the other half:
a release refuses to build unless the artefacts in the tree are stamped for *this*
tag, and unless no code has moved since the commit they were validated against.

Rules, in order:

  1. each artefact carries exactly one `<!-- release: <tag> @ <sha> -->` stamp
  2. the tag in the stamp is the tag being released
  3. all artefacts agree on the stamped commit
  4. that commit exists in this history and is an ancestor of the released commit
  5. nothing outside `docs/architecture/` changed between the two — the rule that
     catches "stamped, then moved code, then tagged anyway"

Usage:
    check_release_stamp.py --tag v1.2.0                 # released commit = HEAD
    check_release_stamp.py --tag v1.2.0 --ref 9d0c15b   # explicit commit
    check_release_stamp.py --tag v1.2.0 --repo-root .   # default

Exit 0 when the release may proceed (one summary line on stdout), 1 otherwise, with
GitHub `::error::` annotations naming the artefact and the reason — so the step fails
in CI with a message that says what to do, not just "exit 1".
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

STAMP_RE = re.compile(r"<!-- release: (\S+) @ ([0-9a-f]{7,40}) -->")
DEFAULT_ARTIFACTS = ("docs/architecture/architecture.html", "docs/architecture/runtime.html")


def fail(message: str, path: Path | None = None) -> None:
    """Print a GitHub annotation and stop."""
    where = f" file={path}" if path is not None else ""
    print(f"::error{where}::{message}")
    raise SystemExit(1)


def git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)


def resolve(repo: Path, ref: str, what: str) -> str:
    """Resolve a ref to a commit sha, or fail naming what could not be resolved."""
    proc = git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}")
    if proc.returncode != 0 or not proc.stdout.strip():
        fail(f"{what} {ref!r} is not a commit in this history — fetch it, or fix the stamp")
    return proc.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifacts", nargs="*", type=Path,
                        help=f"generated diagram pages (default: {' '.join(DEFAULT_ARTIFACTS)})")
    parser.add_argument("--tag", required=True, help="the release tag being built")
    parser.add_argument("--ref", default="HEAD",
                        help="commit being released (default: HEAD; in CI pass $GITHUB_SHA)")
    parser.add_argument("--repo-root", type=Path, default=Path("."),
                        help="git checkout to resolve against (default: .)")
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    if not (repo / ".git").exists():
        fail(f"{repo} is not a git checkout")

    artifacts = [Path(a) for a in args.artifacts] or [repo / a for a in DEFAULT_ARTIFACTS]
    released = resolve(repo, args.ref, "released commit")

    stamped: str | None = None
    for artifact in artifacts:
        if not artifact.exists():
            fail("artifact is missing — was it deleted without updating this check?",
                 artifact.relative_to(repo) if artifact.is_relative_to(repo) else artifact)

        found = STAMP_RE.findall(artifact.read_text(encoding="utf-8"))
        if not found:
            fail(f"no release stamp — refresh the diagrams and stamp them for {args.tag}",
                 artifact)
        if len(found) > 1:
            fail(f"{len(found)} release stamps; expected exactly one", artifact)

        tag, commit = found[0]
        if tag != args.tag:
            fail(f"stamped for {tag} but this release is {args.tag} — stamp the diagrams for this "
                 f"release", artifact)
        if stamped is not None and stamped != commit:
            fail(f"records {commit} but another diagram records {stamped} — they were not stamped "
                 f"together", artifact)
        stamped = commit

    assert stamped is not None  # the loop either sets it or exits
    # Normalise: the stamp may carry a short sha, and every git call wants a commit.
    stamped_commit = resolve(repo, stamped, "stamped commit")

    if git(repo, "merge-base", "--is-ancestor", stamped_commit, released).returncode != 0:
        fail(f"stamped commit {stamped_commit[:12]} is not an ancestor of the released commit "
             f"{released[:12]} — the diagrams describe something else")

    changed = git(repo, "diff", "--name-only", stamped_commit, released, "--", ".",
                  ":(exclude)docs/architecture").stdout.split()
    if changed:
        fail(f"code changed between the stamped commit and the release ({len(changed)} file(s)) "
             f"— regenerate and re-stamp the diagrams: {', '.join(changed[:5])}")

    print(f"release stamp ok: {args.tag} @ {stamped_commit[:12]} "
          f"(released {released[:12]}, nothing changed outside docs/architecture)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
