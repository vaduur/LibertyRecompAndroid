#!/usr/bin/env python3
"""Replace Git symlinks checked out as plain text files with real copies.

Windows checkouts without core.symlinks leave each symlink as a small file
holding its target path; compilers then try to build that path as source.
Every repository in the tree (superproject and nested submodules) is scanned;
replaced entries are marked assume-unchanged so status stays clean.
"""
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True,
                          stdout=subprocess.PIPE).stdout


def repositories(root):
    yield root
    out = git(root, 'submodule', 'foreach', '--quiet', '--recursive', 'echo $displaypath')
    for line in out.decode().splitlines():
        if line.strip():
            yield root / line.strip()


def materialize(repo):
    replaced = []
    for row in git(repo, 'ls-files', '-s', '-z').split(b'\0'):
        if not row.startswith(b'120000 '):
            continue
        rel = row.split(b'\t', 1)[1].decode()
        link = repo / rel
        # A real symlink is already what the build wants. Only a checkout
        # without symlink support leaves the target path in a plain file, and
        # reading a real symlink would yield the target's contents instead.
        if link.is_symlink() or not link.is_file() or link.stat().st_size > 4096:
            continue
        target = (link.parent / link.read_text(encoding='utf-8', errors='replace').strip()).resolve()
        if target.is_dir():
            link.unlink()
            shutil.copytree(target, link)
        elif target.is_file():
            shutil.copyfile(target, link)
        else:
            continue
        replaced.append(rel)
    if replaced:
        subprocess.run(['git', '-C', str(repo), 'update-index', '--assume-unchanged', '--', *replaced],
                       check=False)
    return replaced


def main():
    total = 0
    for repo in repositories(ROOT):
        if not (repo / '.git').exists():
            continue
        done = materialize(repo)
        if done:
            print(f'{repo.relative_to(ROOT)}: {len(done)}')
            total += len(done)
    print(f'materialized {total} symlinks')
    return 0


if __name__ == '__main__':
    sys.exit(main())
