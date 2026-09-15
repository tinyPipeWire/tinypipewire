#!/usr/bin/env python3
"""Checks that a library exports exactly its public header functions, each marked TPW_API.
Usage: check_exported_symbols.py <nm> <library> <header.h> [<header.h> ...]"""
import re
import subprocess
import sys

COMMENT_RE = re.compile(r'/\*.*?\*/', re.DOTALL)
NAME_RE = re.compile(r'\b(tpw_[a-z0-9_]+)\s*\(')
SKIPPED_LINES = ('#', 'extern "C" {', '}')


def declared_functions(paths, errors):
    names = set()
    for path in paths:
        text = COMMENT_RE.sub('', open(path).read())
        lines = [line for line in text.split('\n') if not line.strip().startswith(SKIPPED_LINES)]
        for decl in '\n'.join(lines).split(';'):
            decl = decl.strip()
            if '(' not in decl or decl.startswith('typedef'):
                continue
            match = NAME_RE.search(decl)
            if not match:
                continue
            names.add(match.group(1))
            if not re.search(r'\bTPW_API\b', decl):
                errors.append(f"{path}: {match.group(1)}() is declared without TPW_API, so it is not exported")
    return names


def exported_symbols(nm, library):
    out = subprocess.run([nm, '-D', '--defined-only', library], check=True, capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            name = fields[2].split('@')[0]
            if not name.startswith('_'):
                names.add(name)
    return names


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    nm, library, headers = sys.argv[1], sys.argv[2], sys.argv[3:]

    errors = []
    declared = declared_functions(headers, errors)
    exported = exported_symbols(nm, library)

    for name in sorted(exported - declared):
        errors.append(f"{name} is exported but not declared in a public header")
    for name in sorted(declared - exported):
        errors.append(f"{name}() is declared in a public header but not exported")

    if errors:
        print(f"check_exported_symbols: {len(errors)} problem(s):")
        for error in errors:
            print(f"  - {error}")
        return 1
    print(f"check_exported_symbols: OK ({len(exported)} exported functions match the public headers)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
