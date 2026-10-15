#!/usr/bin/env python3
"""MinGW only: rewrite xlive_exports.def so each export points at its C++-mangled stdcall symbol.

  mingw_def.py NM IN.def OUT.def obj1 obj2 ...

MSVC's linker matches the undecorated names in the .def against C++ symbols itself;
GNU ld does not, so map 'Name @ord' to 'Name = <mangled> @ord' using nm over the objects.
"""
import re, subprocess, sys

nm, src, out, objs = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
syms = {}
text = subprocess.run([nm, "-g", "--defined-only", *objs], capture_output=True, text=True, check=True).stdout
for line in text.splitlines():
    parts = line.split()
    if len(parts) != 3 or parts[1] != "T":
        continue
    sym = parts[2][1:]  # drop the i386 leading underscore, ld adds it back
    m = re.match(r"_Z(\d+)(\w+)", sym)  # _Z<len><name>...
    if m:
        name = m.group(2)[: int(m.group(1))]
    else:
        name = re.sub(r"@\d+$", "", sym)
    syms.setdefault(name, sym)
missing, lines = [], []
for line in open(src):
    m = re.match(r"\s*(\w+)\s+(@\d+.*)$", line)
    if m and m.group(1) not in ("LIBRARY", "EXPORTS"):
        name, rest = m.groups()
        if name not in syms:
            missing.append(name)
            continue
        lines.append(f"\t{name} = {syms[name]} {rest}\n")
    else:
        lines.append(line)
if missing:
    sys.exit("no symbol for: " + ", ".join(missing))
open(out, "w").writelines(lines)
