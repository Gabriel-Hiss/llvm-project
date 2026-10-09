#!/usr/bin/env python3
"""Merge relocation-masked Revenant library databases without duplicate signatures."""

import argparse
import json
from pathlib import Path


def content_key(function):
    return (
        function["size"],
        function["bytes"],
        tuple(function["masked_relocations"]),
    )


def record_key(function):
    return (
        function["name"],
        function["mangled_name"],
        content_key(function),
        tuple(function["callees"]),
    )


def load_database(path):
    with path.open(encoding="utf-8") as stream:
        database = json.load(stream)
    if database.get("version") != 1 or not isinstance(database.get("functions"), list):
        raise ValueError(f"{path}: expected libdb version 1 with a functions array")
    return database


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", required=True, type=Path)
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()

    merged = {"version": 1, "functions": []}
    base_signatures = set()
    seen_records = set()
    duplicates = 0
    for index, path in enumerate(args.inputs):
        database = load_database(path)
        for function in database["functions"]:
            content = content_key(function)
            key = record_key(function)
            if index == 0:
                merged["functions"].append(function)
                base_signatures.add(content)
                seen_records.add(key)
                continue
            if content in base_signatures or key in seen_records:
                duplicates += 1
                continue
            seen_records.add(key)
            merged["functions"].append(function)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(merged, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(
        f"database functions: {len(merged['functions'])} "
        f"({duplicates} duplicate signatures merged)"
    )


if __name__ == "__main__":
    main()
