"""Extract the exact emitted functions for the portable on-device regression."""
import re
import sys
from pathlib import Path

source, output = map(Path, sys.argv[1:])
required = {"recomp_entrypoint", "func_8000B770", "func_80064980", "func_800117B8"}
found = {}
for unit in source.glob("funcs_*.c"):
    for body in unit.read_text().split("RECOMP_FUNC void ")[1:]:
        name = body.partition("(")[0]
        if name in required:
            if name in found:
                raise ValueError(f"Duplicate generated function {name}")
            found[name] = "RECOMP_FUNC void " + body
if missing := required.difference(found):
    raise ValueError(f"Missing generated functions {missing}")
output.write_text('#include "funcs.h"\n' + "\n".join(found[k] for k in sorted(found)))
