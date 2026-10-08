#!/usr/bin/env python3
"""Checks the runtime/training cut line of src/server/game/Animus.

Usage: python3 apps/forge/tools/runtime_graph_check.py [animus_dir]

Scans every file under Animus/Runtime/, resolves each `#include "X.h"` (bare basenames, as the core's
directory-collecting CMake does) against all files under Animus/, and reports every Runtime file that includes a file
outside Animus/Runtime/. Headers that are not in Animus/ (the core's, system and third-party ones) are ignored.
Exit status: 0 when Runtime/ includes only Runtime/ and core headers, 1 listing the offending edges, 2 on a usage
error (no Animus directory, or two Animus files sharing a basename, which would make the resolution ambiguous).
Stdlib only.
"""

import os
import re
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def main(argv):
    root = os.path.abspath(argv[1]) if len(argv) > 1 else os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'src', 'server', 'game', 'Animus'))
    runtime = os.path.join(root, 'Runtime')
    if not os.path.isdir(runtime):
        print('no Runtime directory under ' + root, file=sys.stderr)
        return 2

    by_name = {}
    for directory, _, names in os.walk(root):
        for name in names:
            by_name.setdefault(name, []).append(os.path.join(directory, name))
    clashes = {n: p for n, p in by_name.items() if len(p) > 1}
    for name, paths in sorted(clashes.items()):
        print('basename clash ' + name + ': ' + ', '.join(os.path.relpath(p, root) for p in paths), file=sys.stderr)
    if clashes:
        return 2

    violations = []
    scanned = 0
    for directory, _, names in os.walk(runtime):
        for name in sorted(names):
            path = os.path.join(directory, name)
            scanned += 1
            with open(path, encoding='utf-8', errors='replace') as handle:
                text = handle.read()
            for include in INCLUDE.findall(text):
                target = by_name.get(os.path.basename(include))
                if target and not os.path.abspath(target[0]).startswith(runtime + os.sep):
                    violations.append((os.path.relpath(path, root), include, os.path.relpath(target[0], root)))

    for source, include, target in sorted(violations):
        print(source + ' includes ' + include + ' (' + target + ')')
    print('runtime_graph_check: %d Runtime files scanned, %d cut-line violation(s)' % (scanned, len(violations)))
    return 1 if violations else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
