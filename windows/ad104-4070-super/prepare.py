#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble the 4070 SUPER source tree from hash-pinned shared files and overrides."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath


def prepare(base, overlay, catalog, output):
    output = Path(output).resolve()
    if output.exists():
        raise FileExistsError(f'Choose a new output directory: {output}')
    roots = {'base': Path(base).resolve(), 'overlay': Path(overlay).resolve()}
    files = json.loads(Path(catalog).read_text(encoding='utf-8'))['files']
    contents = {}
    for name, pin in files.items():
        path = PurePosixPath(name)
        if (not name or path.is_absolute() or name != path.as_posix()
                or any(p in ('.', '..') for p in path.parts)
                or any(c in name for c in ('\\', ':', '\x00'))):
            raise ValueError(f'Invalid relative source path: {name!r}')
        root = roots[pin['source']]
        source = root.joinpath(*path.parts)
        if source.is_symlink() or not source.resolve().is_relative_to(root):
            raise ValueError(f'Source escapes its declared directory: {name}')
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise ValueError(f'Source hash differs: {pin["source"]}/{name}')
        contents[name] = data
    # Validate every input before writing anything; keep a single byte snapshot.
    output.mkdir(parents=True, exist_ok=False)
    for name, data in contents.items():
        destination = output.joinpath(*PurePosixPath(name).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    print(f'Prepared {len(contents)} source files in {output}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True, help='New build/source directory')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    prepare(here.parent / 'ad104-msi', here / 'overlay', here / 'source-files.json', args.out)


if __name__ == '__main__':
    main()
