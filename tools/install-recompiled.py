"""Copy regenerated sources into build/recompiled/<region>, touching only the files that changed."""
import filecmp
import shutil
import sys
from pathlib import Path

source, target = Path(sys.argv[1]), Path(sys.argv[2])
target.mkdir(parents=True, exist_ok=True)
for item in sorted(source.iterdir()):
    other = target / item.name
    if not other.exists() or not filecmp.cmp(item, other, shallow=False):
        shutil.copy2(item, other)
        print("updated", item.name)
for item in target.iterdir():
    if not (source / item.name).exists():
        item.unlink()
        print("removed", item.name)
