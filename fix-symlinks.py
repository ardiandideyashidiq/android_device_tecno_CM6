#!/usr/bin/env python3

SYMLINKS_FILE = "symlinks.txt"
PROPRIETARY_FILE = "proprietary-files.txt"
OUTPUT_FILE = "proprietary-files-fixed.txt"


def parse_symlinks(path):
    mappings = {}

    with open(path, "r") as f:
        for line in f:
            line = line.rstrip("\n")

            if not line or "SYMLINK=" not in line:
                continue

            real, symlink = line.split(";SYMLINK=", 1)

            mappings[real] = {
                "full": line,
                "symlink": symlink
            }

    return mappings


def fix_files():
    symlink_map = parse_symlinks(SYMLINKS_FILE)

    with open(PROPRIETARY_FILE, "r") as f:
        lines = f.readlines()

    symlink_remove = set()

    # identify standalone symlink paths to remove
    for real, data in symlink_map.items():
        if any(line.rstrip("\n") == real for line in lines) and \
           any(line.rstrip("\n") == data["symlink"] for line in lines):
            symlink_remove.add(data["symlink"])

    output = []

    for line in lines:
        stripped = line.rstrip("\n")

        # replace real file entry with SYMLINK format
        if stripped in symlink_map:
            output.append(symlink_map[stripped]["full"] + "\n")

        # remove standalone symlink entry
        elif stripped in symlink_remove:
            continue

        else:
            # keep original line exactly (including empty lines)
            output.append(line)

    with open(OUTPUT_FILE, "w") as f:
        f.writelines(output)

    print(f"Created: {OUTPUT_FILE}")
    print(f"Fixed: {len(symlink_remove)} symlinks")


if __name__ == "__main__":
    fix_files()