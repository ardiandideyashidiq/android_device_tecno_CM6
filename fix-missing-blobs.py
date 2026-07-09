#!/usr/bin/env python3
"""
Auto-add missing 32-bit blob dependencies for 64-bit-only builds.

Detects the active device tree from the lunch target, runs 'm nothing' to
find missing 32-bit module variants, adds the corresponding paths from
all_files.txt to proprietary-files.txt, re-extracts, and loops until all
dependencies are resolved, then runs the final build target.

Usage:
    cd /path/to/lineage_root
    python3 fix-missing-blobs.py --lunch-target lineage_CM6-bp4a-eng \
        --dump-dir /path/to/dump/out
"""

import argparse
import glob
import logging
import os
import re
import shutil
import subprocess
import sys
import time

logger = logging.getLogger('fix_missing_blobs')


def setup_logging(verbose: bool = False) -> None:
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format='%(asctime)s - %(levelname)s - %(message)s',
        datefmt='%H:%M:%S',
    )


def detect_lineage_root() -> str | None:
    root = os.path.abspath(os.getcwd())
    while True:
        if os.path.isfile(os.path.join(root, 'build', 'envsetup.sh')):
            return root
        parent = os.path.dirname(root)
        if parent == root:
            return None
        root = parent


def detect_device_tree(lineage_root: str, lunch_target: str | None = None) -> str | None:
    target_device = os.environ.get('TARGET_DEVICE')

    if not target_device and lunch_target:
        m = re.match(r'[^_]+_([^-]+)', lunch_target)
        if m:
            target_device = m.group(1)

    if target_device:
        pattern = os.path.join(lineage_root, 'device', '*', target_device)
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
        logger.warning('Device tree not found for device "%s", scanning...', target_device)

    for d in sorted(glob.glob(os.path.join(lineage_root, 'device', '*', '*'))):
        if not os.path.isdir(d):
            continue
        if (os.path.isfile(os.path.join(d, 'all_files.txt')) and
                os.path.isfile(os.path.join(d, 'proprietary-files.txt'))):
            return d

    return None


def load_all_files(path: str) -> set[str]:
    with open(path) as f:
        return set(line.strip() for line in f if line.strip())


def parse_undefined_modules(output: str) -> set[str]:
    return set(re.findall(r'depends on undefined module "([^"]+)"', output))


def parse_missing_variants(output: str) -> set[str]:
    return set(re.findall(r'dependency "([^"]+)" of "[^"]+" missing variant', output))


def parse_missing_variants_with_consumers(output: str) -> dict[str, set[str]]:
    """Parse 'dependency libX of Y missing variant' -> {libX: {consumer_Y, ...}}"""
    pattern = r'dependency "([^"]+)" of "([^"]+)" missing variant'
    result: dict[str, set[str]] = {}
    for dep, consumer in re.findall(pattern, output):
        result.setdefault(dep, set()).add(consumer)
    return result


def lookup_blobs(
    module_name: str, all_paths: set[str],
) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {'32': [], '64': []}

    # Derive base name by stripping -vN shim suffix
    base_name = re.sub(r'-v\d+$', '', module_name)

    # Directories to search: vendor paths (install target), then system paths (source only)
    install_prefixes = {
        '32': 'vendor/lib',
        '64': 'vendor/lib64',
    }
    source_prefixes = [
        ('vendor/lib', '32'),
        ('vendor/lib64', '64'),
    ]

    for key, install_pref in install_prefixes.items():
        # Direct match on install path
        for candidate in [
            f'{install_pref}/{module_name}.so',
            f'{install_pref}/lib{module_name}.so',
        ]:
            if candidate in all_paths:
                result[key].append(candidate)

        # Fallback: renamed shim
        if not result[key] and base_name != module_name:
            for src_pref, src_key in source_prefixes:
                if src_key != key:
                    continue
                for src_name in [base_name, f'lib{base_name}']:
                    src = f'{src_pref}/{src_name}.so'
                    if src in all_paths:
                        install = f'{install_pref}/{module_name}.so'
                        result[key].append(f'{src}:{install}')

    return result


def find_consumer_32bit_path(consumer_name: str, prop_lines: list[str]) -> str | None:
    """Find a 32-bit vendor lib path for a consumer module in proprietary-files.txt lines."""
    for line in prop_lines:
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        if stripped.startswith('vendor/lib/') and not stripped.startswith('vendor/lib64/'):
            bare = stripped
            if ':' in stripped:
                bare = stripped.split(':')[1]
                bare = bare.split(';')[0]
            filename = bare.rstrip('/').split('/')[-1]
            filebase = filename.rsplit('.', 1)[0]
            if filebase == consumer_name:
                return stripped
    return None


def remove_lines_from_file(file_path: str, target_lines: set[str]) -> bool:
    """Remove matching lines from a file. Returns True if any were removed."""
    with open(file_path) as f:
        lines = f.readlines()

    new_lines = []
    removed = 0
    for line in lines:
        stripped = line.strip()
        if stripped in target_lines:
            removed += 1
            continue
        new_lines.append(line)

    if removed == 0:
        return False

    logger.info('Removed %d line(s) from %s', removed, file_path)
    with open(file_path, 'w') as f:
        f.writelines(new_lines)
    return True


def add_blobs(prop_file: str, new_blobs: list[str]) -> bool:
    with open(prop_file) as f:
        existing = set()
        for line in f:
            stripped = line.strip()
            if stripped and not stripped.startswith('#'):
                existing.add(stripped)

    to_add = sorted(b for b in new_blobs if b not in existing)
    if not to_add:
        logger.info('All blobs already present')
        return False

    logger.info('Adding %d blobs:', len(to_add))
    for b in to_add:
        logger.info('  + %s', b)

    with open(prop_file, 'a') as f:
        f.write(f'\n# Auto-added by fix-missing-blobs.py at {time.ctime()}\n')
        for b in to_add:
            f.write(f'{b}\n')

    return True


def run_build(lineage_root: str, lunch_target: str, make_target: str) -> tuple[str, int]:
    cmd = (
        f'bash -c \''
        f'cd {lineage_root} && '
        f'source build/envsetup.sh >/dev/null 2>&1 && '
        f'lunch {lunch_target} >/dev/null 2>&1 && '
        f'm {make_target} 2>&1'
        f'\''
    )

    logger.debug('Running: m %s', make_target)
    proc = subprocess.Popen(
        cmd, shell=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )

    output_lines = []
    for line in iter(proc.stdout.readline, ''):
        if line:
            sys.stdout.write(line)
            sys.stdout.flush()
            output_lines.append(line)

    proc.wait()
    return ''.join(output_lines), proc.returncode


def run_extraction(device_dir: str, dump_dir: str | None = None) -> None:
    extract_py = os.path.join(device_dir, 'extract-files.py')
    setup_py = os.path.join(device_dir, 'setup-makefiles.py')

    if os.path.exists(extract_py):
        logger.info('Running extract-files.py...')
        cmd = [extract_py]
        if dump_dir:
            cmd.append(dump_dir)
        subprocess.run(cmd, cwd=device_dir, check=False)

    if os.path.exists(setup_py):
        logger.info('Running setup-makefiles.py...')
        subprocess.run([setup_py], cwd=device_dir, check=False)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='Auto-add missing 32-bit blob dependencies for 64-bit-only builds.',
    )
    parser.add_argument(
        '--lunch-target', required=True,
        help='Lunch target (e.g., lineage_CM6-bp4a-eng)',
    )
    parser.add_argument(
        '--dump-dir',
        help='Path to firmware dump directory (passed to extract-files.py)',
    )
    parser.add_argument(
        '--max-iterations', type=int, default=30,
        help='Maximum loop iterations (default: 30)',
    )
    parser.add_argument(
        '--verbose', action='store_true',
        help='Enable DEBUG logging',
    )
    parser.add_argument(
        '--final-target', default='vendorbootimage',
        help='Final build target after dependency resolution (default: vendorbootimage)',
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    setup_logging(args.verbose)

    lineage_root = detect_lineage_root()
    if not lineage_root:
        logger.error('LineageOS root not found (build/envsetup.sh missing)')
        logger.error('Run this script from within the source tree')
        return 1

    logger.info('LineageOS root: %s', lineage_root)

    device_dir = detect_device_tree(lineage_root, args.lunch_target)
    if not device_dir:
        logger.error('Could not detect device tree. Is the lunch target correct?')
        return 1

    logger.info('Device tree: %s', device_dir)

    prop_file = os.path.join(device_dir, 'proprietary-files.txt')
    all_file = os.path.join(device_dir, 'all_files.txt')

    for path, name in [(prop_file, 'proprietary-files.txt'), (all_file, 'all_files.txt')]:
        if not os.path.isfile(path):
            logger.error('%s not found in device tree', name)
            return 1

    all_paths = load_all_files(all_file)
    logger.info('all_files.txt: %d total paths', len(all_paths))

    # Single backup of original state
    backup_path = prop_file + '.bak'
    if not os.path.exists(backup_path):
        shutil.copy2(prop_file, backup_path)
        logger.info('Original backup: %s', backup_path)

    # Main loop
    for i in range(args.max_iterations):
        logger.info('=' * 60)
        logger.info('Iteration %d/%d', i + 1, args.max_iterations)
        logger.info('=' * 60)

        output, rc = run_build(lineage_root, args.lunch_target, 'nothing')

        undefined = parse_undefined_modules(output)
        missing_variant_map = parse_missing_variants_with_consumers(output)
        missing_variants = set(missing_variant_map.keys())

        logger.info('Build return code: %d', rc)
        logger.debug('Undefined modules: %s', sorted(undefined) or 'none')
        logger.debug('Missing variant modules: %s', sorted(missing_variants) or 'none')

        if not undefined and not missing_variants:
            if rc == 0:
                logger.info('SUCCESS: All dependencies resolved!')
                break
            else:
                logger.error('Build failed with no missing-module errors')
                logger.error('Check the full build output above')
                return 1

        # Read current prop lines for consumer lookups
        with open(prop_file) as f:
            prop_lines = f.readlines()

        new_blobs: set[str] = set()
        consumers_to_remove: set[str] = set()

        for mod in sorted(undefined):
            blobs = lookup_blobs(mod, all_paths)
            found = blobs['32'] + blobs['64']
            if not found:
                logger.warning('Undefined "%s" — not found in all_files.txt', mod)
            else:
                logger.info('Undefined "%s" → %s', mod, found)
                new_blobs.update(found)

        for mod in sorted(missing_variants):
            blobs = lookup_blobs(mod, all_paths)
            if not blobs['32']:
                logger.warning('Missing variant "%s" — no 32-bit path in all_files.txt', mod)
                # Try to remove the consumer's 32-bit blob instead
                for consumer in sorted(missing_variant_map.get(mod, set())):
                    path = find_consumer_32bit_path(consumer, prop_lines)
                    if path:
                        logger.info('  → Removing consumer "%s" 32-bit blob: %s', consumer, path)
                        consumers_to_remove.add(path)
                    else:
                        logger.warning('  → Consumer "%s" has no 32-bit blob to remove', consumer)
            else:
                best = blobs['32'][0]
                logger.info('Missing variant "%s" → %s', mod, best)
                new_blobs.add(best)

        if consumers_to_remove:
            remove_lines_from_file(prop_file, consumers_to_remove)

        if not new_blobs and not consumers_to_remove:
            logger.error('No new blobs or removals available — cannot proceed')
            return 1

        if new_blobs:
            logger.info('New blobs to add: %d', len(new_blobs))
            add_blobs(prop_file, list(new_blobs))

        logger.info('Regenerating vendor tree...')
        run_extraction(device_dir, args.dump_dir)

    logger.info('=' * 60)
    logger.info('Running final build: m %s', args.final_target)
    logger.info('=' * 60)

    output, rc = run_build(lineage_root, args.lunch_target, args.final_target)

    if rc == 0:
        logger.info('SUCCESS: %s build completed!', args.final_target)
        return 0
    else:
        logger.error('%s build failed with code %d', args.final_target, rc)
        return 1


if __name__ == '__main__':
    sys.exit(main())
