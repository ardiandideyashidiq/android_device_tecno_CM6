#!/usr/bin/env python3
import os
import sys


def scan_symlinks(root_dir):
    root_abs = os.path.abspath(root_dir)
    parent = os.path.dirname(root_abs)
    lines = []

    for dirpath, dirnames, filenames in os.walk(root_abs, followlinks=False):
        # Check directory symlinks in place
        for d in list(dirnames):
            full = os.path.join(dirpath, d)
            if os.path.islink(full):
                target_raw = os.readlink(full)
                _format_link(lines, full, target_raw, parent, root_abs)
                dirnames.remove(d)

        for f in filenames:
            full = os.path.join(dirpath, f)
            if os.path.islink(full):
                target_raw = os.readlink(full)
                _format_link(lines, full, target_raw, parent, root_abs)

    return sorted(set(lines))


def _format_link(lines, full_path, target_raw, parent, root_abs):
    if os.path.isabs(target_raw):
        resolved = target_raw
    else:
        resolved = os.path.normpath(os.path.join(os.path.dirname(full_path), target_raw))

    symlink_rel = os.path.relpath(full_path, parent)
    if resolved.startswith(root_abs):
        target_rel = os.path.relpath(resolved, parent)
    elif resolved.startswith('/vendor'):
        target_rel = 'vendor' + resolved[6:]
    elif os.path.exists(resolved):
        target_rel = os.path.relpath(resolved, parent)
    else:
        target_rel = resolved

    lines.append(f"{target_rel};SYMLINK={symlink_rel}")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <directory>", file=sys.stderr)
        sys.exit(1)

    root_dir = sys.argv[1]
    if not os.path.isdir(root_dir):
        print(f"Error: '{root_dir}' is not a directory", file=sys.stderr)
        sys.exit(1)

    basename = os.path.basename(os.path.abspath(root_dir))
    out_file = f"{basename}-symlink-list.txt"

    lines = scan_symlinks(root_dir)
    with open(out_file, "w") as f:
        f.write("\n".join(lines))
        f.write("\n")

    print(f"Found {len(lines)} symlinks. Written to {out_file}")


if __name__ == "__main__":
    main()
