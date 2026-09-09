#!/usr/bin/env python3
"""Verify two fixed-Linux raw benchmark runs for TinyLSM Goals 4-6."""

import argparse
import json
import math
import re
import sys
from pathlib import Path


SNAPSHOT_NAMES = [
    "TinyLSM/SnapshotMaterializedScan10K",
    "TinyLSM/SnapshotIterator10K",
    "TinyLSM/SnapshotMaterializedScan100K",
    "TinyLSM/SnapshotIterator100K",
    "TinyLSM/SnapshotMaterializedScan1000K",
    "TinyLSM/SnapshotIterator1000K",
    "TinyLSM/SnapshotRetentionHeld0s",
    "TinyLSM/SnapshotRetentionHeld10s",
    "TinyLSM/SnapshotRetentionHeld60s",
    "TinyLSM/SnapshotRetentionReleased60s",
    "TinyLSM/SnapshotWriterOverlapMaterializedScan",
    "TinyLSM/SnapshotWriterOverlapIterator",
]

SSTABLE_NAMES = []
for shape in ("Prefix", "Random"):
    SSTABLE_NAMES += [
        f"TinyLSM/SstableDataBlockEncodeV1{shape}",
        f"TinyLSM/SstableDataBlockLookupV1{shape}",
    ]
    for interval in (4, 16, 64):
        SSTABLE_NAMES += [
            f"TinyLSM/SstableDataBlockEncodeV2{shape}Restart{interval}",
            f"TinyLSM/SstableDataBlockLookupV2{shape}Restart{interval}",
        ]

GROUP_COMMIT_NAMES = [
    f"TinyLSM/GroupCommit{mode}{writers}Writers"
    for writers in (1, 2, 4, 8, 16)
    for mode in ("Off", "On")
]

EXPECTED_NAMES = SNAPSHOT_NAMES + SSTABLE_NAMES + GROUP_COMMIT_NAMES
WRITE_REGRESSION_NAMES = [
    "TinyLSM/SnapshotWriterOverlapMaterializedScan",
    "TinyLSM/SnapshotWriterOverlapIterator",
] + GROUP_COMMIT_NAMES
MEDIAN_SUFFIX = re.compile(r"/iterations:1/repeats:5/manual_time_median$")


def medians(path: Path) -> dict[str, dict]:
    with path.open() as source:
        document = json.load(source)
    result: dict[str, dict] = {}
    for row in document.get("benchmarks", []):
        if row.get("aggregate_name") != "median":
            continue
        name = row.get("name", "")
        base_name = MEDIAN_SUFFIX.sub("", name)
        if base_name == name:
            continue
        result[base_name] = row
    return result


def verify_coverage(
    label: str, values: dict[str, dict], expected_names: list[str]
) -> None:
    expected = set(expected_names)
    actual = set(values)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        if missing:
            print(f"{label}: missing medians: {', '.join(missing)}", file=sys.stderr)
        if unexpected:
            print(f"{label}: unexpected medians: {', '.join(unexpected)}", file=sys.stderr)
        raise SystemExit(1)

    for name in expected.intersection(SSTABLE_NAMES):
        counters = values[name]
        required = "encoded_bytes" if "Encode" in name else "block_bytes"
        if required not in counters or "bytes_per_entry" not in counters:
            print(f"{label}: {name} is missing {required}/bytes_per_entry", file=sys.stderr)
            raise SystemExit(1)
    for name in expected.intersection(GROUP_COMMIT_NAMES):
        counters = values[name]
        required = {"wal_syncs", "wal_syncs_per_write", "physical_groups",
                    "writer_queue_wait_us", "put_p95_us"}
        if not required.issubset(counters):
            missing_counters = sorted(required - set(counters))
            print(f"{label}: {name} is missing counters: {', '.join(missing_counters)}",
                  file=sys.stderr)
            raise SystemExit(1)


def coefficient_of_variation(first: float, second: float) -> float:
    mean = (first + second) / 2.0
    if mean == 0.0:
        return 0.0
    return abs(first - second) / math.sqrt(2.0) / mean * 100.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run1", type=Path)
    parser.add_argument("run2", type=Path)
    parser.add_argument("--max-cv", type=float, default=10.0)
    parser.add_argument(
        "--scope", choices=("all", "write-regression"), default="all"
    )
    args = parser.parse_args()

    expected_names = (
        EXPECTED_NAMES if args.scope == "all" else WRITE_REGRESSION_NAMES
    )
    first = medians(args.run1)
    second = medians(args.run2)
    verify_coverage(str(args.run1), first, expected_names)
    verify_coverage(str(args.run2), second, expected_names)

    rejected = []
    for name in expected_names:
        cv = coefficient_of_variation(float(first[name]["real_time"]),
                                      float(second[name]["real_time"]))
        print(f"{name}\treal_time_cv_percent={cv:.3f}")
        if cv > args.max_cv:
            rejected.append((name, cv))
    if rejected:
        formatted = ", ".join(f"{name} ({cv:.3f}%)" for name, cv in rejected)
        print(f"runs exceed {args.max_cv:.1f}% CV: {formatted}", file=sys.stderr)
        return 1
    print(
        f"verified {len(expected_names)} median cases in each run; "
        f"all CV <= {args.max_cv:.1f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
