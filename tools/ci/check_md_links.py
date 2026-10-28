#!/usr/bin/env python3
"""Check that relative links in the repository's Markdown files resolve.

Every [text](target) whose target is not a URL must name an existing file or
directory, and a #fragment must match a heading in the target file (GitHub's
anchor rules: lower case, punctuation dropped, spaces to hyphens, -N suffixes
for repeats). Run from anywhere; exits non-zero and lists each broken link.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
FENCE = re.compile(r"^\s*(```|~~~)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


def strip_code(text):
    """Drop fenced code blocks and inline code spans, which hold no links."""
    out, in_fence = [], False
    for line in text.splitlines():
        if FENCE.match(line):
            in_fence = not in_fence
            out.append("")
            continue
        out.append("" if in_fence else re.sub(r"`[^`]*`", "", line))
    return out


def anchors(path):
    seen, result = {}, set()
    for line in strip_code(path.read_text(encoding="utf-8")):
        m = HEADING.match(line)
        if not m:
            continue
        text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", m.group(2))
        slug = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
        n = seen.get(slug, 0)
        seen[slug] = n + 1
        result.add(slug if n == 0 else f"{slug}-{n}")
    return result


def main():
    files = [p for p in ROOT.rglob("*.md")
             if not any(part.startswith(".") or part in ("build", "build-host")
                        for part in p.relative_to(ROOT).parts)]
    broken = []
    for md in sorted(files):
        for lineno, line in enumerate(strip_code(md.read_text(encoding="utf-8")), 1):
            for target in LINK.findall(line):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                    continue  # http:, https:, mailto:
                path_part, _, fragment = target.partition("#")
                dest = (md.parent / path_part).resolve() if path_part else md
                if not dest.exists():
                    broken.append(f"{md.relative_to(ROOT)}:{lineno}: {target} (no such file)")
                elif fragment and dest.suffix == ".md" and fragment not in anchors(dest):
                    broken.append(f"{md.relative_to(ROOT)}:{lineno}: {target} (no such heading)")
    for b in broken:
        print(b, file=sys.stderr)
    print(f"checked {len(files)} Markdown files: {len(broken)} broken links")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
