#!/usr/bin/env python3
"""Enforce structural agent/skill synchronization for repository changes."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = re.compile(r"(?:^|/)(?:AGENTS\.md|[^/]*\.agent\.md|SKILL\.md|[^/]*instructions\.md)$", re.I)
REQUIRED = {
    "AGENTS.md": ["Continuous Agent & Skills Maintenance", "Post-change checklist"],
}
SKILL_SECTIONS = ["Maintenance & Continuous Learning"]
POLICY_PHRASES = [
    "Agent instructions and skills are living repository artifacts.",
    "Every repository modification MUST include an agent/skills synchronization step.",
    "A code change without the corresponding agent knowledge maintenance is considered incomplete.",
    "Never claim completion when the synchronization step has been skipped or has failed.",
]


def git(*args: str) -> str:
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True, stderr=subprocess.PIPE)


def changed_paths(base: str | None) -> tuple[set[str], set[str]]:
    if base:
        raw = git("diff", "--name-status", "--find-renames", f"{base}...HEAD")
    else:
        raw = git("diff", "--name-status", "--find-renames", "HEAD")
        raw += git("diff", "--cached", "--name-status", "--find-renames", "HEAD")
        raw += git("ls-files", "--others", "--exclude-standard")
    changed: set[str] = set()
    deleted: set[str] = set()
    for line in raw.splitlines():
        fields = line.split("\t")
        if len(fields) == 1:
            changed.add(fields[0])
        elif fields[0].startswith("D"):
            deleted.add(fields[-1])
        else:
            changed.update(fields[1:])
    return changed, deleted


def check() -> list[str]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="base commit/ref for PR comparison (diff base...HEAD)")
    args = parser.parse_args()
    errors: list[str] = []
    try:
        changed, deleted = changed_paths(args.base)
    except subprocess.CalledProcessError as exc:
        return [f"git change discovery failed: {exc.stderr.strip()}"]

    files = sorted(p for p in changed if (ROOT / p).is_file())
    project_changes = {p for p in changed if not DOC.search(p)}
    if project_changes and not any(DOC.search(p) and p not in deleted for p in changed):
        errors.append("project files changed without a changed agent instruction or SKILL.md")

    # Every instruction file in the repository must remain structurally valid.
    instruction_paths = sorted(
        p for p in (ROOT / ".").rglob("*")
        if p.is_file() and DOC.search(p.relative_to(ROOT).as_posix())
        and ".git" not in p.parts
    )
    for path in instruction_paths:
        rel = path.relative_to(ROOT).as_posix()
        content = path.read_text(encoding="utf-8")
        if rel.endswith("SKILL.md"):
            if not content.startswith("---\n") or content.count("\n---\n") < 1:
                errors.append(f"{rel}: missing valid YAML frontmatter delimiters")
            else:
                frontmatter = content.split("---\n", 2)[1]
                if not re.search(r"(?m)^name:\s*\S+", frontmatter):
                    errors.append(f"{rel}: frontmatter missing name")
                if not re.search(r"(?m)^description:\s*\S+", frontmatter):
                    errors.append(f"{rel}: frontmatter missing description")
            if not any(f"## {heading}" in content for heading in SKILL_SECTIONS):
                errors.append(f"{rel}: missing 'Maintenance & Continuous Learning' section")
        if rel == "AGENTS.md":
            for heading in REQUIRED[rel]:
                if heading not in content:
                    errors.append(f"{rel}: missing required section/text {heading!r}")
            for phrase in POLICY_PHRASES:
                if phrase not in content:
                    errors.append(f"{rel}: missing required policy statement {phrase!r}")
        # Validate relative Markdown links in instruction files; ignore anchors and URLs.
        for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", content):
            target = target.split("#", 1)[0].strip().strip("<>")
            if not target or re.match(r"(?:[a-z]+:|//)", target, re.I):
                continue
            resolved = (path.parent / target).resolve()
            if not resolved.exists():
                errors.append(f"{rel}: broken local reference {target!r}")

    # A maintenance edit must carry meaningful textual content, not whitespace,
    # timestamps, comments, or a cosmetic heading-only change.
    docs_changed = [p for p in changed if DOC.search(p) and p not in deleted]
    if project_changes:
        substantive = False
        for rel in docs_changed:
            try:
                diff = git("diff", "--unified=0", args.base or "HEAD", "--", rel)
            except subprocess.CalledProcessError:
                continue
            additions = [line[1:].strip() for line in diff.splitlines()
                         if line.startswith("+") and not line.startswith("+++")]
            removals = [line[1:].strip() for line in diff.splitlines()
                        if line.startswith("-") and not line.startswith("---")]
            meaningful = lambda s: bool(s) and not re.match(r"(?:<!--.*-->|\*?updated?:|last updated|date:)", s, re.I)
            if sum(1 for s in additions if meaningful(s)) >= 3:
                substantive = True
                break
        if not substantive:
            errors.append("instruction update must add and remove meaningful non-metadata text")

    if errors:
        return errors
    print(f"Agent maintenance structure PASS ({len(changed)} changed paths, {len(instruction_paths)} instruction files checked).")
    print("Semantic relevance still requires agent/reviewer judgment.")
    return []


if __name__ == "__main__":
    problems = check()
    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print("Update applicable agent/skill knowledge and rerun this validator.", file=sys.stderr)
        sys.exit(1)
