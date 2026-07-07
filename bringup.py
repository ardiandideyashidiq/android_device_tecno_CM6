#!/usr/bin/env python3
"""
Android device tree bring-up utility.

Trims proprietary blobs from a donor device tree for a target device.
Also resolves missing ELF dependencies from a stock ROM dump.

Usage:
  ./bringup.py --trim --base proprietary-files.txt --target x6812b.txt --output test.txt
  ./bringup.py --resolve --list test-t812.txt --dump /path/to/dump
  ./bringup.py --resolve --list test-t812.txt --dump /path/to/dump --apply
  ./bringup.py --clean test.txt
"""

import argparse
import logging
import os
import re
import subprocess
import sys
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

VERSION_RE = re.compile(r'(@)(\d+(?:\.\d+|\.X)*)')
VARIANT_CLEAN_RE = re.compile(r'(?:[-_])?(?:v\d+|64b|32b|64bit|lazy)$')
GENERAL_VARIANT_RE = re.compile(r'[-_][a-z]{2,6}$')
SOC_RE = re.compile(r'\b(mt|sm|sdm|exynos|kirin|sc|ums)\d{3,}')

# Resolve constants
SUPPRESSED_NEEDED = {"android.hardware.gnss-V1-ndk_platform.so"}
SCAN_PREFIXES = ("vendor/", "system/", "system_ext/", "product/")
ELF_PATH_HINTS = ("/bin/", "/lib/", "/lib64/")
PARTITION_GROUPS = {"vendor": "vendor", "odm": "vendor", "system": "system", "system_ext": "system", "product": "system"}
VERSION_STRIP_RE = re.compile(r'\.\d+(?:\.\d+)*$')
DLOPEN_RE = re.compile(r'\b(lib\w+\.so)\b')
AUTO_HEADER = "# Auto-added by resolve-missing-blobs"

logger = logging.getLogger("bringup")


def setup_logging() -> None:
    fmt = "%(asctime)s  %(levelname)-8s %(message)s"
    logging.basicConfig(level=logging.INFO, format=fmt, stream=sys.stderr, datefmt="%H:%M:%S")


# ── Trim functions ─────────────────────────────────────────────────────


def parse_versions(path: str) -> tuple[str, list[tuple[str, str, tuple[int, ...]]]]:
    normalized = path
    versions: list[tuple[str, str, tuple[int, ...]]] = []
    for match in VERSION_RE.finditer(path):
        full = match.group(0)
        ver_str = match.group(2)
        ver_parts = ver_str.split(".")
        ver_tuple = tuple(int(p) if p != "X" else 0 for p in ver_parts)
        versions.append((full, ver_str, ver_tuple))
    for full, _, _ in versions:
        normalized = normalized.replace(full, "@VERSION", 1)
    return normalized, versions


def _split_stem_ext(part: str) -> tuple[str, str]:
    dot = part.rfind(".")
    after = part[dot + 1:] if dot != -1 else ""
    if dot != -1 and after.isalpha() and len(after) <= 4:
        return part[:dot], part[dot:]
    return part, ""


def _strip_known_variants(stem: str) -> str:
    while True:
        new_stem = VARIANT_CLEAN_RE.sub("", stem)
        if new_stem == stem:
            break
        stem = new_stem
    return stem


def _strip_generalized_variant(stem: str) -> str:
    m = GENERAL_VARIANT_RE.search(stem)
    if m and len(stem[:m.start()]) >= 8:
        return stem[:m.start()]
    return stem


def fuzzy_key(path: str) -> str:
    key = VERSION_RE.sub("@VERSION", path)
    parts = key.split("/")
    for i, part in enumerate(parts):
        stem, ext = _split_stem_ext(part)
        stem = _strip_known_variants(stem)
        parts[i] = stem + ext
    return "/".join(parts)


def fuzzy_key_generalized(path: str) -> str:
    key = fuzzy_key(path)
    parts = key.split("/")
    for i, part in enumerate(parts):
        stem, ext = _split_stem_ext(part)
        stem = _strip_generalized_variant(stem)
        parts[i] = stem + ext
    return "/".join(parts)


def build_fuzzy_index(target_set: set[str]) -> dict[str, list[tuple[str, list[tuple[str, str, tuple[int, ...]]]]]]:
    index: dict[str, list[tuple[str, list[tuple[str, str, tuple[int, ...]]]]]] = {}
    for path in target_set:
        _, versions = parse_versions(path)
        if not versions:
            continue
        key = fuzzy_key(path)
        index.setdefault(key, []).append((path, versions))
    logger.info("Built fuzzy index: %d keys from %d versioned targets", len(index), sum(len(v) for v in index.values()))
    return index


def detect_soc_maps(donor_lines: list[str], target_set: set[str]) -> dict[str, str]:
    donor_socs: set[str] = set()
    for line in donor_lines:
        for m in SOC_RE.finditer(line):
            donor_socs.add(m.group())
    target_socs: set[str] = set()
    for p in target_set:
        for m in SOC_RE.finditer(p):
            target_socs.add(m.group())
    donor_only = donor_socs - target_socs
    target_only = target_socs - donor_socs
    if not donor_only or not target_only:
        return {}
    logger.info("Donor-only SoCs: %s  Target-only SoCs: %s", donor_only, target_only)
    maps: dict[str, str] = {}
    for d_soc in donor_only:
        best_target: str | None = None
        best_count = 0
        for t_soc in target_only:
            count = 0
            for line in donor_lines:
                if d_soc in line and line.replace(d_soc, t_soc) in target_set:
                    count += 1
            if count > best_count:
                best_count = count
                best_target = t_soc
        if best_target is not None and best_count > 0:
            maps[d_soc] = best_target
            logger.info("Auto SOC map: %s \u2192 %s  (%d matches)", d_soc, best_target, best_count)
    return maps


def version_distance(donor_versions: list[tuple[int, ...]], target_versions: list[tuple[int, ...]]) -> int:
    if not donor_versions and not target_versions:
        return 0
    if not donor_versions or not target_versions:
        return 1000
    total = 0
    for dv, tv in zip(donor_versions, target_versions):
        d_major = dv[0]
        d_minor = dv[1] if len(dv) > 1 else 0
        t_major = tv[0]
        t_minor = tv[1] if len(tv) > 1 else 0
        if d_major == t_major:
            total += abs(d_minor - t_minor)
        else:
            total += 100 + abs(d_major - t_major) * 10
    return total


def resolve_version_fuzzy(candidate: str, fuzzy_index: dict) -> str | None:
    _, donor_versions = parse_versions(candidate)
    if not donor_versions:
        return None
    key = fuzzy_key(candidate)
    entries = fuzzy_index.get(key)
    if not entries:
        gkey = fuzzy_key_generalized(candidate)
        if gkey != key:
            entries = fuzzy_index.get(gkey)
            if entries:
                logger.debug("Generalized fallback matched: %s \u2192 key %s", candidate, gkey)
    if not entries:
        return None
    dv_tuples = [v[2] for v in donor_versions]
    best = None
    best_score = float("inf")
    for target_path, target_versions in entries:
        tv_tuples = [v[2] for v in target_versions]
        score = version_distance(dv_tuples, tv_tuples)
        if score < best_score:
            best_score = score
            best = target_path
    return best


def resolve_fuzzy_on_candidates(candidates: list[str], fuzzy_index: dict) -> tuple[str, str] | None:
    best_candidate = None
    best_match = None
    best_score = float("inf")
    for c in candidates:
        match = resolve_version_fuzzy(c, fuzzy_index)
        if match:
            _, cv = parse_versions(c)
            _, tv = parse_versions(match)
            score = version_distance([v[2] for v in cv], [v[2] for v in tv])
            if score < best_score:
                best_score = score
                best_candidate = c
                best_match = match
    if best_candidate is not None:
        return best_candidate, best_match
    return None


def apply_version_update(raw_line: str, matched_candidate: str, matched_target: str) -> str:
    _, cand_versions = parse_versions(matched_candidate)
    _, target_versions = parse_versions(matched_target)
    updated = raw_line
    cand_parts = matched_candidate.split("/")
    target_parts = matched_target.split("/")
    for cp, tp in zip(cand_parts, target_parts):
        if cp != tp:
            updated = updated.replace(cp, tp)
    for cv, tv in zip(cand_versions, target_versions):
        updated = updated.replace(cv[0], tv[0])
    return updated


def load_target(path: str) -> tuple[set[str], dict]:
    logger.info("Loading target file list: %s", path)
    files: set[str] = set()
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            raw = line.strip()
            if raw:
                files.add(raw)
    logger.info("Loaded %d entries from %s", len(files), path)
    fuzzy_index = build_fuzzy_index(files)
    return files, fuzzy_index


def parse_line(raw: str) -> tuple[str, str, dict[str, str]]:
    raw = raw.strip()
    parts = raw.split(";")
    path_spec = parts[0].strip()
    modifiers: dict[str, str] = {}
    for mod in parts[1:]:
        mod = mod.strip()
        if not mod:
            continue
        if "=" in mod:
            k, v = mod.split("=", 1)
            modifiers[k.strip()] = v.strip()
        else:
            modifiers[mod] = ""
    src = path_spec
    dst = ""
    if ":" in path_spec:
        src, dst = path_spec.split(":", 1)
        src = src.strip()
        dst = dst.strip()
    return src, dst, modifiers


def is_system_line(src: str) -> bool:
    return src.startswith("system/") or src.startswith("system_ext/")


def collect_candidates(src: str, dst: str, modifiers: dict[str, str]) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()
    for p in [src, dst] if dst else [src]:
        p = p.strip()
        if p and p not in seen:
            seen.add(p)
            result.append(p)
    symlink_target = modifiers.get("SYMLINK", "")
    if symlink_target and symlink_target not in seen:
        result.append(symlink_target)
    return result


def _read_donor_lines(path: str) -> list[str]:
    lines: list[str] = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.strip()
            if stripped and not stripped.startswith("#"):
                lines.append(stripped)
    return lines


def _try_match_line(raw: str, target_set: set[str], fuzzy_index: dict, soc_maps: dict[str, str]) -> tuple[str | None, str | None, str]:
    stripped = raw.strip()
    if not stripped or stripped.startswith("#"):
        return raw, None, "PASS_THROUGH"
    try:
        src, dst, modifiers = parse_line(stripped)
    except Exception:
        return raw, None, "PARSE_FAILED"
    if is_system_line(src):
        return f"# DROPPED (system): {stripped}", None, "SYSTEM"
    candidates = collect_candidates(src, dst, modifiers)
    for c in candidates:
        if c in target_set:
            return raw, None, "EXACT"
    fuzzy_result = resolve_fuzzy_on_candidates(candidates, fuzzy_index)
    if fuzzy_result:
        matched_candidate, matched_target = fuzzy_result
        out_line = apply_version_update(raw, matched_candidate, matched_target)
        new_src, _, _ = parse_line(out_line)
        return out_line, new_src if new_src != src else None, "FUZZY"
    if soc_maps:
        for c in candidates:
            for soc_donor, soc_target in soc_maps.items():
                if soc_donor in c:
                    remapped = c.replace(soc_donor, soc_target)
                    if remapped in target_set:
                        out_line = raw.replace(soc_donor, soc_target)
                        new_src, _, _ = parse_line(out_line)
                        return out_line, new_src if new_src != src else None, "SOC_MAP"
    return f"# DROPPED (missing): {stripped}", None, "MISSING"


def process(base_path: str, target_set: set[str], fuzzy_index: dict, output_path: str) -> tuple[int, int, int, int, int, dict[str, str]]:
    kept = 0
    fuzzy = 0
    soc_remapped = 0
    dropped_system = 0
    dropped_missing = 0
    path_map: dict[str, str] = {}
    out_lines: list[str] = []
    donor_lines = _read_donor_lines(base_path)
    soc_maps = detect_soc_maps(donor_lines, target_set)
    with open(base_path, "r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            raw = line.rstrip("\n")
            out_line, new_src, mode = _try_match_line(raw, target_set, fuzzy_index, soc_maps)
            if mode == "PASS_THROUGH":
                out_lines.append(out_line if out_line is not None else raw)
                continue
            if mode == "PARSE_FAILED":
                logger.warning("L%04d  PARSE FAILED \u2192 kept verbatim: %s", lineno, raw[:80])
                out_lines.append(raw)
                continue
            if mode == "EXACT":
                out_lines.append(raw)
                kept += 1
                continue
            if mode == "FUZZY":
                src, _, _ = parse_line(raw.strip())
                out_lines.append(out_line)
                if new_src:
                    path_map[src] = new_src
                kept += 1
                fuzzy += 1
                continue
            if mode == "SOC_MAP":
                src, _, _ = parse_line(raw.strip())
                out_lines.append(out_line)
                if new_src:
                    path_map[src] = new_src
                kept += 1
                soc_remapped += 1
                continue
            if mode == "SYSTEM":
                out_lines.append(out_line)
                dropped_system += 1
                continue
            if mode == "MISSING":
                out_lines.append(out_line)
                dropped_missing += 1
                continue
    logger.info("Writing %d lines to %s", len(out_lines), output_path)
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out_lines))
        fh.write("\n")
    logger.info("Done.")
    return kept, fuzzy, soc_remapped, dropped_system, dropped_missing, path_map


def clean_file(path: str, output_path: str | None = None) -> int:
    logger.info("Cleaning dropped-comment lines from: %s", path)
    out_path = output_path or path
    with open(path, "r", encoding="utf-8") as fh:
        orig_lines = fh.readlines()
    kept_lines: list[str] = []
    removed = 0
    for line in orig_lines:
        stripped = line.strip()
        if stripped.startswith("# DROPPED"):
            removed += 1
        else:
            kept_lines.append(line)
    logger.info("Removed %d dropped-comment lines", removed)
    logger.info("Writing %d lines to %s", len(kept_lines), out_path)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        fh.writelines(kept_lines)
    return removed


EXTRACT_TEMPLATE_RE = re.compile(r"ExtractUtilsModule\(\s*'(\w+)'")


def derive_device_name(output_path: str, old_device: str) -> str:
    base = os.path.splitext(os.path.basename(output_path))[0]
    segments = re.split(r"[-_]", base)
    candidate = segments[-1]
    if old_device and old_device[0].isupper():
        candidate = candidate[0].upper() + candidate[1:] if candidate else candidate
    return candidate


def generate_extract_companion(output_path: str, base_path: str, path_map: dict[str, str], device_override: str | None = None) -> None:
    base_dir = os.path.dirname(os.path.abspath(base_path))
    template_path = os.path.join(base_dir, "extract-files.py")
    if not os.path.isfile(template_path):
        logger.warning("Template not found at %s \u2014 skipping companion generation", template_path)
        return
    with open(template_path, "r", encoding="utf-8") as fh:
        content = fh.read()
    m = EXTRACT_TEMPLATE_RE.search(content)
    if not m:
        logger.warning("Could not detect device name in template \u2014 skipping companion generation")
        return
    old_device = m.group(1)
    if device_override:
        device = device_override
    else:
        device = derive_device_name(output_path, old_device)
    updated = content
    if old_device != device:
        updated = updated.replace(old_device, device)
        updated = updated.replace(old_device.lower(), device.lower())
    for old_path, new_path in path_map.items():
        if old_path in updated:
            updated = updated.replace(old_path, new_path)
    out_dir = os.path.dirname(os.path.abspath(output_path))
    base_name = os.path.splitext(os.path.basename(output_path))[0]
    extract_path = os.path.join(out_dir, f"{base_name}-extract-files.py")
    with open(extract_path, "w", encoding="utf-8") as fh:
        fh.write(updated)
    os.chmod(extract_path, 0o755)
    logger.info("Generated: %s", extract_path)


# ── Resolve functions ──────────────────────────────────────────────────


def read_all_paths(path: Path) -> list[str]:
    paths = []
    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        for raw in handle:
            line = raw.strip()
            if line and not line.startswith("#") and line != "all_files.txt":
                paths.append(line)
    return paths


def _read_proprietary_paths(path: Path) -> list[str]:
    paths: list[str] = []
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            path = stripped.split(";")[0].split(":")[0].strip()
            if path:
                paths.append(path)
    return paths


def parse_proprietary_entry(raw: str) -> dict | None:
    line = raw.rstrip("\n")
    stripped = line.strip()
    if not stripped or stripped.startswith("#"):
        return None
    is_named_dependency = stripped.startswith("-")
    if is_named_dependency:
        stripped = stripped[1:].strip()
    if ";" in stripped:
        stripped, _flag_suffix = stripped.split(";", 1)
        stripped = stripped.strip()
    source = None
    dest = stripped
    if ":" in stripped:
        source, dest = stripped.split(":", 1)
        source = source.strip() or None
        dest = dest.strip() or source
    return {"raw_line": line, "source": source, "dest": dest, "is_named_dependency": is_named_dependency, "aliases": alias_paths(dest)}


def alias_paths(path: str) -> list[str]:
    aliases = [path]
    _prefixes = [
        ("system/", "system/system/"), ("system/system/", "system/"),
        ("system_ext/", "system/system_ext/"), ("system/system_ext/", "system_ext/"),
        ("product/", "system/product/"), ("system/product/", "product/"),
    ]
    for left, right in _prefixes:
        if path.startswith(left):
            alt = right + path[len(left):]
            if alt not in aliases:
                aliases.append(alt)
    return aliases


def load_parsed_entries(path: Path) -> list[dict]:
    entries = []
    with path.open("r", encoding="utf-8") as handle:
        for raw in handle:
            entry = parse_proprietary_entry(raw)
            if entry is not None:
                entries.append(entry)
    return entries


def load_tracked_entries(path: Path) -> tuple[set[str], list[str], list[dict]]:
    tracked = set()
    roots = []
    entries = load_parsed_entries(path)
    for entry in entries:
        tracked.update(entry["aliases"])
        roots.append(entry["dest"])
    return tracked, roots, entries


def should_scan_path(rel_path: str) -> bool:
    if not rel_path.startswith(SCAN_PREFIXES):
        return False
    return any(hint in f"/{rel_path}" for hint in ELF_PATH_HINTS) or rel_path.endswith(".so")


def partition_family(rel_path: str) -> str:
    top = rel_path.split("/", 1)[0]
    return PARTITION_GROUPS.get(top, top)


def bitness_hint(rel_path: str) -> str | None:
    if "/lib64/" in f"/{rel_path}":
        return "64"
    if "/lib/" in f"/{rel_path}":
        return "32"
    return None
def parse_elf_metadata(dump_root: Path, rel_path: str) -> tuple[dict | None, str | None]:
    """Parse ELF metadata from readelf output.

    Returns (metadata_dict, None) on success, or (None, reason) on failure
    where reason is one of 'no_readelf', 'not_elf', 'no_deps'.
    """
    file_path = dump_root / rel_path
    try:
        proc = subprocess.run(["readelf", "-h", "-d", str(file_path)], check=False, capture_output=True, text=True, timeout=30)
    except OSError:
        return None, "no_readelf"

    if proc.returncode != 0:
        return None, "not_elf"

    elf_class = None
    soname = None
    needed = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("Class:"):
            if "ELF64" in line:
                elf_class = "64"
            elif "ELF32" in line:
                elf_class = "32"
        elif "(SONAME)" in line and "[" in line and "]" in line:
            soname = line.split("[", 1)[1].split("]", 1)[0]
        elif "(NEEDED)" in line and "[" in line and "]" in line:
            needed.append(line.split("[", 1)[1].split("]", 1)[0])

    if not needed and soname is None:
        return None, "no_deps"

    return {
        "path": rel_path,
        "basename": Path(rel_path).name,
        "partition": partition_family(rel_path),
        "bitness": bitness_hint(rel_path) or elf_class,
        "soname": soname,
        "needed": needed,
    }, None
def collect_elf_metadata(dump_root: Path, paths, jobs: int) -> dict:
    metadata = {}
    total = len(paths)
    completed = 0
    log_interval = max(1, min(250, total // 20))
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
        future_map = {executor.submit(parse_elf_metadata, dump_root, rel_path): rel_path for rel_path in paths}
        for future in as_completed(future_map):
            rel_path = future_map[future]
            info, _reason = future.result()
            completed += 1
            if info is not None:
                metadata[rel_path] = info
            if completed == 1 or completed == total or completed % log_interval == 0:
                logger.info("[scan %d/%d] %d ELF files", completed, total, len(metadata))
    return dict(sorted(metadata.items()))


def build_provider_map(metadata) -> dict:
    provider_map = defaultdict(list)
    for rel_path, info in metadata.items():
        keys = [info["basename"]]
        if info["soname"] and info["soname"] not in keys:
            keys.insert(0, info["soname"])
        for key in keys:
            provider_map[key].append(rel_path)
    for key in provider_map:
        provider_map[key] = sorted(set(provider_map[key]))
    return provider_map


def is_system_side(rel_path: str) -> bool:
    return rel_path.startswith(("system/", "system_ext/", "product/"))


def path_group(rel_path: str) -> str:
    if rel_path.startswith("vendor/lib64/"):
        return "vendor/lib64"
    if rel_path.startswith("vendor/lib/"):
        return "vendor/lib"
    if rel_path.startswith("system/"):
        return "system"
    if rel_path.startswith("system_ext/"):
        return "system_ext"
    if rel_path.startswith("product/"):
        return "product"
    return "other"


def _version_normalize(name: str) -> str:
    return VERSION_STRIP_RE.sub("", name)


def _path_prefix_score(requester_path: str, candidate_path: str) -> int:
    pr = requester_path.split("/")
    pc = candidate_path.split("/")
    score = 0
    for a, b in zip(pr, pc):
        if a == b:
            score += 1
        else:
            break
    return score


def choose_provider(requester_info, providers, include_system) -> tuple[str, list[str]]:
    candidates = list(providers)
    same_partition = [p for p in candidates if partition_family(p) == requester_info["partition"]]
    if same_partition:
        candidates = same_partition
    if requester_info["partition"] == "vendor":
        vendor_candidates = [p for p in candidates if partition_family(p) == "vendor"]
        if vendor_candidates:
            candidates = vendor_candidates
    if requester_info["bitness"] == "64":
        matches = [p for p in candidates if bitness_hint(p) == "64"]
        if matches:
            candidates = matches
    elif requester_info["bitness"] == "32":
        matches = [p for p in candidates if bitness_hint(p) == "32"]
        if matches:
            candidates = matches
    if not include_system:
        filtered = [p for p in candidates if not is_system_side(p)]
        if filtered:
            candidates = filtered
    chosen = max(candidates, key=lambda p: _path_prefix_score(requester_info["path"], p))
    return chosen, sorted(candidates)


def resolve_dependencies(metadata, provider_map, roots, tracked, excluded, source_provided, include_system, max_depth) -> tuple[dict, dict, dict, dict, defaultdict, dict]:
    additions = {}
    ambiguous = {}
    excluded_hits = {}
    provided_hits = {}
    unresolved = defaultdict(set)
    unresolved_chain = {}
    queue = deque()
    seen = set()
    processed = 0
    for root in roots:
        if root in metadata and root not in seen:
            queue.append((root, 0))
            seen.add(root)
    while queue:
        rel_path, depth = queue.popleft()
        info = metadata.get(rel_path)
        if info is None:
            continue
        processed += 1
        if processed == 1 or processed % 250 == 0:
            logger.info("[resolve %d] queue=%d additions=%d unresolved=%d", processed, len(queue), len(additions), sum(len(v) for v in unresolved.values()))
        if max_depth is not None and depth >= max_depth:
            continue
        for need in info["needed"]:
            if need in SUPPRESSED_NEEDED:
                unresolved["suppressed"].add(need)
                continue
            providers = provider_map.get(need, [])
            if not providers:
                normalized = _version_normalize(need)
                if normalized != need:
                    providers = provider_map.get(normalized, [])
            if not providers:
                unresolved["missing"].add(need)
                unresolved_chain.setdefault(need, []).append(rel_path)
                continue
            chosen, considered = choose_provider(info, providers, include_system)
            if not include_system and is_system_side(chosen):
                unresolved["filtered"].add(need)
                unresolved_chain.setdefault(need, []).append(rel_path)
                continue
            edge = {"from": rel_path, "needed": need, "provider": chosen}
            if len(considered) > 1:
                ambiguous[need] = {"chosen": chosen, "providers": considered}
            if chosen in excluded:
                excluded_hits[chosen] = edge
            elif chosen in source_provided:
                provided_hits[chosen] = {**edge, "modules": source_provided[chosen]}
            elif chosen not in tracked and chosen not in additions:
                additions[chosen] = edge
            if chosen in metadata and chosen not in seen:
                seen.add(chosen)
                queue.append((chosen, depth + 1))
    return additions, ambiguous, excluded_hits, provided_hits, unresolved, unresolved_chain


def _scan_one_strings(file_path: Path) -> set[str]:
    try:
        out = subprocess.run(["strings", str(file_path)], capture_output=True, text=True, check=False, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return set()
    return set(DLOPEN_RE.findall(out.stdout))


def scan_dlopen_references(dump_root: Path, rel_paths: list[str], tracked: set[str], jobs: int) -> dict[str, list[str]]:
    logger.info("Scanning binaries for dlopen references in %d files...", len(rel_paths))
    found: dict[str, list[str]] = {}
    total = len(rel_paths)
    completed = 0
    with ThreadPoolExecutor(max_workers=max(1, jobs)) as ex:
        fut_map = {}
        for rel_path in rel_paths:
            file_path = dump_root / rel_path
            if not file_path.is_file():
                continue
            fut_map[ex.submit(_scan_one_strings, file_path)] = rel_path
        for future in as_completed(fut_map):
            rel_path = fut_map[future]
            libs = future.result()
            completed += 1
            if completed == 1 or completed == total or completed % 500 == 0:
                logger.info("[dlopen %d/%d] found %d unique libs", completed, total, len(found))
            for lib in libs:
                if lib not in tracked:
                    found.setdefault(lib, []).append(rel_path)
    logger.info("dlopen scan complete: found %d potential libs", len(found))
    return found


def emit_human(summary, additions, ambiguous, excluded, provided_by_source, unresolved, unresolved_chain) -> str:
    lines = [
        f"tracked entries: {summary['tracked_entries']}",
        f"tracked ELF roots: {summary['tracked_elf_roots']}",
        f"indexed ELF files: {summary['indexed_elf_files']}",
        f"proposed additions: {summary['proposed_additions']}",
        f"proposed vendor additions: {summary['proposed_vendor_additions']}",
        f"ambiguous SONAMEs: {summary['ambiguous_count']}",
        f"dlopen additions: {summary.get('dlopen_additions', 0)}",
        f"excluded providers: {summary['excluded_count']}",
        f"provided by source: {summary['provided_by_source_count']}",
        f"unresolved names: {summary['unresolved_count']}",
    ]
    grouped = defaultdict(list)
    for rel_path in sorted(additions):
        grouped[path_group(rel_path)].append(rel_path)
    for group_name in sorted(grouped):
        lines.append("")
        lines.append(f"[{group_name}]")
        lines.extend(grouped[group_name])
    sample_edges = [additions[key] for key in sorted(additions)[:40]]
    if sample_edges:
        lines.append("")
        lines.append("Sample dependency edges:")
        for edge in sample_edges:
            lines.append(f"{edge['from']} --[{edge['needed']}]--> {edge['provider']}")
    if ambiguous:
        lines.append("")
        lines.append("Ambiguous providers:")
        for need in sorted(ambiguous):
            item = ambiguous[need]
            lines.append(f"{need}: {item['chosen']} ({', '.join(item['providers'])})")
    if excluded:
        lines.append("")
        lines.append("Excluded providers:")
        for rel_path in sorted(excluded):
            edge = excluded[rel_path]
            lines.append(f"{rel_path} <- {edge['from']} [{edge['needed']}]")
    if provided_by_source:
        lines.append("")
        lines.append("Provided by source:")
        for rel_path in sorted(provided_by_source):
            edge = provided_by_source[rel_path]
            lines.append(f"{rel_path} <- {edge['from']} [{edge['needed']}] modules={','.join(edge['modules'])}")
    if unresolved:
        lines.append("")
        lines.append("Unresolved names:")
        for category in sorted(unresolved):
            for need in sorted(unresolved[category]):
                requesters = unresolved_chain.get(need, [])
                req_str = f"  (needed by {', '.join(sorted(requesters)[:3])})" if requesters else ""
                lines.append(f"{category}: {need}{req_str}")
    return "\n".join(lines)


def apply_additions(prop_path: Path, additions) -> bool:
    if not additions:
        return False
    with prop_path.open("r", encoding="utf-8") as handle:
        existing = handle.readlines()
    block = [AUTO_HEADER + "\n"]
    block.extend(f"{path}\n" for path in sorted(additions))
    block.append("\n")
    with prop_path.open("w", encoding="utf-8") as handle:
        handle.writelines(block + existing)
    return True


def _load_scan_paths(dump_path: Path, list_path: Path) -> tuple[list[str], set[str], list[str], list]:
    """Load tracked entries and determine which files to scan.

    Returns (scan_paths, tracked, roots, tracked_entries).
    Falls back to --list as file listing when all_files.txt is absent.
    """
    tracked, roots, tracked_entries = load_tracked_entries(list_path)
    logger.info("Loaded proprietary entries: %d", len(tracked_entries))
    all_files_path = dump_path / "all_files.txt"
    if all_files_path.is_file():
        all_paths = read_all_paths(all_files_path)
    else:
        all_paths = _read_proprietary_paths(list_path)
    logger.info("Loaded file listing: %d entries", len(all_paths))
    scan_paths = [p for p in all_paths if should_scan_path(p) and (dump_path / p).is_file()]
    logger.info("Candidate scan paths: %d", len(scan_paths))
    return scan_paths, tracked, roots, tracked_entries


def _run_dlopen_scan(dump_path: Path, tracked: set[str], additions: dict, provider_map: dict, jobs: int, include_system: bool = False) -> dict[str, dict]:
    """Post-BFS: scan binaries for dlopen() references to untracked libs."""
    all_traversed = list(tracked) + list(additions.keys())
    dlopen_libs = scan_dlopen_references(dump_path, all_traversed, tracked, jobs)
    dlopen_additions: dict[str, dict] = {}
    for lib, requesters in sorted(dlopen_libs.items()):
        providers = provider_map.get(lib, [])
        if not providers:
            normalized = _version_normalize(lib)
            if normalized != lib:
                providers = provider_map.get(normalized, [])
        if providers:
            vendor_provs = [p for p in providers if p.startswith("vendor/")]
            if not vendor_provs and not include_system:
                continue
            candidates = vendor_provs if vendor_provs else providers
            for prov in candidates:
                if prov not in tracked and prov not in additions and prov not in dlopen_additions:
                    dlopen_additions[prov] = {"from": requesters[0], "needed": lib, "provider": prov, "via": "dlopen"}
                    break
    logger.info("dlopen-triggered additions: %d", len(dlopen_additions))
    return dlopen_additions


def _build_additions_summary(additions: dict, tracked_entries: list, elf_roots: int, metadata: dict,
                              ambiguous: dict, excluded_hits: dict, provided_hits: dict,
                              unresolved_count: int, dlopen_count: int) -> dict:
    vendor_additions = [p for p in additions if p.startswith("vendor/")]
    return {
        "tracked_entries": len(tracked_entries),
        "tracked_elf_roots": elf_roots,
        "indexed_elf_files": len(metadata),
        "proposed_additions": len(additions),
        "proposed_vendor_additions": len(vendor_additions),
        "ambiguous_count": len(ambiguous),
        "excluded_count": len(excluded_hits),
        "provided_by_source_count": len(provided_hits),
        "unresolved_count": unresolved_count,
        "dlopen_additions": dlopen_count,
    }


def run_resolve(list_path: Path, dump_path: Path, apply: bool = False, jobs: int | None = None, include_system: bool = False, max_depth: int | None = None) -> dict:
    jobs = jobs or (os.cpu_count() or 4)
    if not dump_path.is_dir():
        raise ValueError(f"dump root not found: {dump_path}")
    if not list_path.is_file():
        raise ValueError(f"list file not found: {list_path}")

    scan_paths, tracked, roots, tracked_entries = _load_scan_paths(dump_path, list_path)
    excluded: set[str] = set()
    source_provided: dict = {}

    logger.info("Starting parallel readelf scan...")
    metadata = collect_elf_metadata(dump_path, scan_paths, jobs)
    logger.info("Scan complete: indexed %d ELF files", len(metadata))
    provider_map = build_provider_map(metadata)
    logger.info("Provider map built: %d names", len(provider_map))

    elf_roots = sum(1 for root in roots if root in metadata)
    logger.info("Starting dependency resolution from %d ELF roots...", elf_roots)
    additions, ambiguous, excluded_hits, provided_hits, unresolved, unresolved_chain = resolve_dependencies(
        metadata=metadata, provider_map=provider_map, roots=roots, tracked=tracked,
        excluded=excluded, source_provided=source_provided, include_system=include_system, max_depth=max_depth,
    )
    unresolved_count = sum(len(v) for v in unresolved.values())
    logger.info("Resolution complete: additions=%d ambiguous=%d unresolved=%d",
                len(additions), len(ambiguous), unresolved_count)

    dlopen_additions = _run_dlopen_scan(dump_path, tracked, additions, provider_map, jobs)
    additions.update(dlopen_additions)
    logger.info("Final with dlopen: additions=%d", len(additions))

    summary = _build_additions_summary(additions, tracked_entries, elf_roots, metadata,
        ambiguous, excluded_hits, provided_hits, unresolved_count, len(dlopen_additions))

    if apply and additions:
        logger.info("Applying %d additions to %s", len(additions), list_path)
        apply_additions(list_path, additions)
        logger.info("Apply complete")

    return {
        "summary": summary,
        "additions": {k: additions[k] for k in sorted(additions)},
        "edges": [additions[k] for k in sorted(additions)],
        "ambiguous": {k: ambiguous[k] for k in sorted(ambiguous)},
        "excluded": {k: excluded_hits[k] for k in sorted(excluded_hits)},
        "provided_by_source": {k: provided_hits[k] for k in sorted(provided_hits)},
        "unresolved": {k: sorted(v) for k, v in sorted(unresolved.items())},
        "unresolved_chain": {k: sorted(v) for k, v in unresolved_chain.items()},
    }


# ── Resolve mode wrapper ──────────────────────────────────────────────


def resolve_mode(args) -> None:
    try:
        payload = run_resolve(
            list_path=Path(args.list).resolve(),
            dump_path=Path(args.dump).resolve(),
            apply=args.apply,
            include_system=args.include_system if hasattr(args, 'include_system') else False,
        )
    except ValueError as e:
        logger.error("%s", e)
        sys.exit(1)
    if args.output:
        with open(args.output, "w") as fh:
            for path in sorted(payload["additions"]):
                fh.write(f"{path}\n")
        logger.info("Wrote %d blob paths to %s", len(payload["additions"]), args.output)
    print(emit_human(payload["summary"], payload["additions"], payload["ambiguous"], payload["excluded"],
                     payload["provided_by_source"], payload["unresolved"], payload.get("unresolved_chain", {})))


# ── Main ──────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(description="Android device tree bring-up utility")
    parser.add_argument("--trim", action="store_true", help="Trim proprietary blobs against a target file list")
    parser.add_argument("--resolve", action="store_true", help="Resolve missing ELF dependencies from a stock ROM dump")
    parser.add_argument("--clean", help="Clean DROPPED comments from a prior output")
    parser.add_argument("--base", help="Donor proprietary-files.txt")
    parser.add_argument("--target", help="Stock ROM file listing (one path per line)")
    parser.add_argument("--extract-device", help="Override device codename in companion script")
    parser.add_argument("--list", help="Proprietary-files list to resolve")
    parser.add_argument("--dump", help="Stock ROM dump root (must contain vendor/ etc.)")
    parser.add_argument("--apply", action="store_true", help="Apply additions to list file")
    parser.add_argument("--output", help="Output file path")

    if len(sys.argv) == 1:
        parser.print_help()
        sys.exit(0)

    args = parser.parse_args()
    setup_logging()

    if args.resolve:
        if not args.list or not args.dump:
            parser.error("--list and --dump are required in resolve mode")
        resolve_mode(args)
        return

    if args.clean:
        if not os.path.isfile(args.clean):
            logger.error("File not found: %s", args.clean)
            sys.exit(1)
        removed = clean_file(args.clean, args.output)
        logger.info("REMOVED %d dropped-comment lines", removed)
        return

    if not args.base or not args.target or not args.output:
        parser.error("--base, --target, and --output are required in trim mode")

    for path in (args.base, args.target):
        if not os.path.isfile(path):
            logger.error("File not found: %s", path)
            sys.exit(1)

    target_set, fuzzy_index = load_target(args.target)
    kept, fuzzy, soc_remapped, dropped_system, dropped_missing, path_map = process(args.base, target_set, fuzzy_index, args.output)

    logger.info(
        "KEPT=%-5d UPDATED=%-5d SOC-MAP=%-5d SYSTEM=%-5d MISSING=%-5d TOTAL=%d",
        kept, fuzzy, soc_remapped, dropped_system, dropped_missing,
        kept + dropped_system + dropped_missing,
    )

    generate_extract_companion(args.output, args.base, path_map, device_override=args.extract_device)


if __name__ == "__main__":
    main()
