#!/usr/bin/env python3
"""Keep core contracts independent of reflection and higher library layers."""
import re
from pathlib import Path

core = Path(__file__).resolve().parents[1] / "include" / "refl" / "core"
violations = []
headers = list(core.rglob("*.hpp"))
headers += list((core.parent / "runtime").glob("*.hpp"))
headers += [core.parent / "extensions/observed.hpp"]
for header in sorted(headers):
    for include in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', header.read_text(), re.M):
        is_core = include.startswith("refl/core/") and ".." not in include
        is_standard = "/" not in include and "." not in include and include != "meta"
        is_invocation = header.name == "observed.hpp" and include == "refl/runtime/invoke.hpp"
        if not (is_core or is_standard or is_invocation):
            violations.append(f"{header.name}: forbidden dependency {include}")
if violations:
    raise SystemExit("\n".join(violations))
