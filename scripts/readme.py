"""Checks that the minimal program in README.md is tal/demo/hello.cpp as it is, so that the example
of the README is a program that builds. Exits with 1, and says where they part, when they differ.

Usage: python3 scripts/readme.py (from anywhere)
"""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
readme = (root / "README.md").read_text(encoding="utf-8")
program = (root / "tal" / "demo" / "hello.cpp").read_text(encoding="utf-8")
marker = "<!-- tal/demo/hello.cpp -->\n```cpp\n"
start = readme.find(marker)

if start < 0:
    sys.exit("README.md: no block after " + marker.splitlines()[0])

start += len(marker)
end = readme.find("```\n", start)
shown = readme[start:end]

if shown != program:
    for number, (one, other) in enumerate(zip(shown.splitlines(), program.splitlines()), 1):
        if one != other:
            sys.exit(f"README.md and tal/demo/hello.cpp part at line {number} of the program:\n  {one}\n  {other}")

    sys.exit("README.md and tal/demo/hello.cpp differ in their length")

print("README.md shows tal/demo/hello.cpp as it is")
