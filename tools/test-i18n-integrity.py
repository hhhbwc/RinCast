#!/usr/bin/env python3
"""
Test i18n completeness and charset coverage across PC and Headset.
Requirements:
  1. PC i18n.js:
     - 7 languages (en, zh-CN, zh-TW, de, es, fr, ja) have 100% identical key sets (missing = 0).
     - Placeholders {...} match en 100% across all languages.
     - Zero "Quest" occurrences except allowed device identifiers (e.g. device.quest.*).
  2. Headset strings.cpp:
     - 7 language tables (kEn, kZhCN, kZhTW, kDe, kEs, kFr, kJa) have 100% identical key sets (missing = 0).
     - Placeholders {...} match kEn 100% across all languages.
     - 100% of non-ASCII characters across all 7 tables are covered in charset.txt (missing = 0).
"""

import os
import re
import sys

def parse_js_dict(body_text):
    # Matches "key": "value" where value can have escaped quotes
    # Regex: "([^"]+)"\s*:\s*"((?:[^"\\]|\\.)*)"
    entries = re.findall(r'"([^"]+)"\s*:\s*"((?:[^"\\]|\\.)*)"', body_text)
    # Unescape \" to " and \\ to \
    result = {}
    for k, v in entries:
        val = v.replace('\\"', '"').replace('\\\\', '\\')
        result[k] = val
    return result

def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(repo_root)

    print("=== Testing RinCast i18n & Charset Integrity ===")
    errors = []

    # ---------------- 1. PC i18n.js ----------------
    pc_path = os.path.join(repo_root, "pc", "streamer", "ui", "i18n.js")
    with open(pc_path, "r", encoding="utf-8") as f:
        pc_text = f.read()

    langs = ["en", "zh-CN", "zh-TW", "de", "es", "fr", "ja"]
    pc_tables = {}
    for lang in langs:
        pat = r'(?:^|\n)\s*(?:["\']?' + re.escape(lang) + r'["\']?)\s*:\s*\{([\s\S]*?)(?:\n\s*\},?\n)'
        m = re.search(pat, pc_text)
        if not m:
            errors.append(f"PC i18n.js: language block '{lang}' not found")
            continue
        pc_tables[lang] = parse_js_dict(m.group(1))

    if "en" in pc_tables:
        en_keys = set(pc_tables["en"].keys())
        print(f"PC i18n.js 'en' key count: {len(en_keys)}")

        for lang in langs:
            if lang == "en":
                continue
            cur_keys = set(pc_tables.get(lang, {}).keys())
            missing = en_keys - cur_keys
            extra = cur_keys - en_keys
            if missing:
                errors.append(f"PC i18n.js: '{lang}' is missing {len(missing)} keys: {sorted(list(missing))}")
            if extra:
                errors.append(f"PC i18n.js: '{lang}' has {len(extra)} extra keys: {sorted(list(extra))}")

            # Check placeholders
            for k in en_keys:
                if k in pc_tables[lang]:
                    en_val = pc_tables["en"][k]
                    cur_val = pc_tables[lang][k]
                    en_placeholders = sorted(re.findall(r'\{[a-zA-Z0-9_]+\}', en_val))
                    cur_placeholders = sorted(re.findall(r'\{[a-zA-Z0-9_]+\}', cur_val))
                    if en_placeholders != cur_placeholders:
                        errors.append(f"PC i18n.js: placeholder mismatch in key '{k}' for lang '{lang}': expected {en_placeholders}, got {cur_placeholders}")

            # Check "Quest" occurrences
            for k, val in pc_tables[lang].items():
                if k.startswith("device.quest"):
                    continue
                if "Quest" in val:
                    errors.append(f"PC i18n.js: found 'Quest' in key '{k}' for lang '{lang}': '{val}'")

    # ---------------- 2. Headset strings.cpp ----------------
    hmd_path = os.path.join(repo_root, "headset", "app", "src", "main", "cpp", "app", "strings.cpp")
    with open(hmd_path, "r", encoding="utf-8") as f:
        hmd_text = f.read()

    hmd_tables = {}
    hmd_langs = ["kEn", "kZhCN", "kZhTW", "kDe", "kEs", "kFr", "kJa"]
    for lang in hmd_langs:
        pat = r'const Entry ' + re.escape(lang) + r'\[\]\s*=\s*\{([\s\S]*?)\};'
        m = re.search(pat, hmd_text)
        if not m:
            errors.append(f"Headset strings.cpp: table '{lang}' not found")
            continue
        entries = re.findall(r'\{"([^"]+)",\s*"((?:[^"\\]|\\.)*)"\}', m.group(1))
        hmd_tables[lang] = {k: v.replace('\\"', '"').replace('\\\\', '\\') for k, v in entries}

    if "kEn" in hmd_tables:
        ken_keys = set(hmd_tables["kEn"].keys())
        print(f"Headset strings.cpp 'kEn' key count: {len(ken_keys)}")

        for lang in hmd_langs:
            if lang == "kEn":
                continue
            cur_keys = set(hmd_tables.get(lang, {}).keys())
            missing = ken_keys - cur_keys
            extra = cur_keys - ken_keys
            if missing:
                errors.append(f"Headset strings.cpp: '{lang}' is missing {len(missing)} keys: {sorted(list(missing))}")
            if extra:
                errors.append(f"Headset strings.cpp: '{lang}' has {len(extra)} extra keys: {sorted(list(extra))}")

            # Check placeholders
            for k in ken_keys:
                if k in hmd_tables[lang]:
                    en_val = hmd_tables["kEn"][k]
                    cur_val = hmd_tables[lang][k]
                    en_placeholders = sorted(re.findall(r'\{[a-zA-Z0-9_]+\}', en_val))
                    cur_placeholders = sorted(re.findall(r'\{[a-zA-Z0-9_]+\}', cur_val))
                    if en_placeholders != cur_placeholders:
                        errors.append(f"Headset strings.cpp: placeholder mismatch in key '{k}' for lang '{lang}': expected {en_placeholders}, got {cur_placeholders}")

    # ---------------- 3. Headset charset.txt ----------------
    charset_path = os.path.join(repo_root, "headset", "app", "src", "main", "assets", "ui", "charset.txt")
    with open(charset_path, "r", encoding="utf-8") as f:
        charset_chars = set(f.read())

    all_hmd_non_ascii = set()
    for lang, table in hmd_tables.items():
        for k, val in table.items():
            for c in val:
                if ord(c) > 127:
                    all_hmd_non_ascii.add(c)

    charset_missing = all_hmd_non_ascii - charset_chars
    print(f"Total non-ASCII characters in strings.cpp: {len(all_hmd_non_ascii)}")
    print(f"Total characters in charset.txt: {len(charset_chars)}")
    if charset_missing:
        errors.append(f"Headset charset.txt: missing {len(charset_missing)} characters: {sorted(list(charset_missing))}")
    else:
        print("Headset charset coverage: 100% (0 missing)")

    # ---------------- Report ----------------
    if errors:
        print(f"\nFAILED with {len(errors)} error(s):")
        for err in errors:
            print("  -", err)
        sys.exit(1)
    else:
        print("\nSUCCESS: All i18n tables are 100% complete, placeholders match, zero Quest hardcoding, and charset coverage is 100%!")
        sys.exit(0)

if __name__ == "__main__":
    main()
