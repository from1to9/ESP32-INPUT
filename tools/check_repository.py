#!/usr/bin/env python3
"""Check first-party text hygiene and local documentation links."""
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit

root = Path(__file__).resolve().parents[1]
names = subprocess.check_output(
    ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=root
).decode().split("\0")
errors = []
checked = 0
for name in sorted(set(filter(None, names))):
    path = root / name
    if name.startswith("third_party/") or not path.is_file():
        continue
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        continue  # Binary assets are not prose.
    checked += 1
    if text and not text.endswith("\n"):
        errors.append(f"{name}: missing final newline")
    for number, line in enumerate(text.splitlines(), 1):
        if line.rstrip() != line:
            errors.append(f"{name}:{number}: trailing whitespace")
        # Guard the prior mixed-language regression; this is not a language detector.
        if re.search(r"[\u3400-\u9fff]", line):
            errors.append(f"{name}:{number}: CJK prose in English-only first-party content")
    if path.suffix == ".md":
        prose = re.sub(r"```.*?```", "", text, flags=re.S)
        for target in re.findall(r"\[[^\]]*\]\(([^)]+)\)", prose):
            target = target.strip("<>")
            parsed = urlsplit(target)
            if parsed.scheme or not parsed.path:
                continue
            if not (path.parent / unquote(parsed.path)).exists():
                errors.append(f"{name}: missing local link: {target}")
if errors:
    raise SystemExit("\n".join(errors))
print(f"PASS: first-party text and local Markdown links ({checked} files)")
print("Upstream submodules excluded; English wording still requires human review.")
