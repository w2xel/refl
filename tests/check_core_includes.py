#!/usr/bin/env python3
"""Keep core contracts independent of reflection and higher library layers."""
import re
from pathlib import Path

core = Path(__file__).resolve().parents[1] / "include" / "refl" / "core"
violations = []
for header in sorted(core.rglob("*.hpp")):
    for include in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', header.read_text(), re.M):
        is_core = include.startswith("refl/core/") and ".." not in include
        is_standard = "/" not in include and "." not in include and include != "meta"
        if not (is_core or is_standard):
            violations.append(f"{header.name}: forbidden dependency {include}")
if violations:
    raise SystemExit("\n".join(violations))
