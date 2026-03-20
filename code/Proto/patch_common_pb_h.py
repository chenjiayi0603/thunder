#!/usr/bin/env python3
"""移除 common.pb.h 首段 namespace common 中的 `class Foo;` 前向声明（GCC + protobuf 7 offsetof 问题）。"""
import re
from pathlib import Path

def main():
    path = Path(__file__).resolve().parent / "src" / "common.pb.h"
    lines = path.read_text(encoding="utf-8").splitlines(True)
    start = None
    for i, line in enumerate(lines):
        if "descriptor_table_common_2eproto" in line:
            for j in range(i, min(i + 15, len(lines))):
                if lines[j].strip() == "namespace common {":
                    start = j
                    break
            break
    if start is None:
        raise SystemExit("patch_common_pb_h: could not locate first namespace common { in common.pb.h")

    end = None
    for k in range(start + 1, len(lines)):
        if lines[k].strip() == "}  // namespace common":
            end = k
            break
    if end is None:
        raise SystemExit("patch_common_pb_h: could not find end of first namespace common")

    class_line = re.compile(r"^\s*class\s+[A-Za-z0-9_]+\s*;\s*$")
    out = []
    for i, line in enumerate(lines):
        if start < i < end and class_line.match(line):
            continue
        out.append(line)
    path.write_text("".join(out), encoding="utf-8")
    print(f"patch_common_pb_h: stripped forward class decls in lines {start+1}-{end+1}")

if __name__ == "__main__":
    main()
