#!/usr/bin/env python3
"""Checks an Autobahn TestSuite report (its index.json) for one agent.

Every case's behavior and behaviorClose must be OK, NON-STRICT or INFORMATIONAL (which Autobahn doesn't grade: those
cases only record what the peer did); anything else (FAILED, WRONG CODE, UNCLEAN, UNIMPLEMENTED, ...) fails, and so
does a report with fewer or more cases than expected (e.g. a run cut short). Each failing case is printed.

usage: check.py <index.json> [--agent cppnet] [--cases 301]
"""

import argparse
import collections
import json
import sys

PASSING = {"OK", "NON-STRICT", "INFORMATIONAL"}


def case_key(case_id):
    """Sorts case ids (e.g. 6.22.10) by their numbers."""
    return [int(part) if part.isdigit() else part for part in case_id.split(".")]


def main():
    parser = argparse.ArgumentParser(description="Check an Autobahn TestSuite report.")
    parser.add_argument("index", help="the report's index.json")
    parser.add_argument("--agent", default="cppnet", help="the agent whose cases to check (default: cppnet)")
    parser.add_argument("--cases", type=int, required=True, help="how many cases the report must have")
    args = parser.parse_args()

    try:
        with open(args.index, encoding="utf-8") as f:
            index = json.load(f)
    except (OSError, ValueError) as err:
        print(f"{args.index}: no report: {err}")
        return 1

    if args.agent not in index:
        print(f"{args.index}: no cases for agent {args.agent!r} (agents: {', '.join(sorted(index)) or 'none'})")
        return 1

    cases = index[args.agent]
    counts = collections.Counter()
    failures = []
    for case_id in sorted(cases, key=case_key):
        result = cases[case_id]
        behavior = result.get("behavior", "missing")
        close = result.get("behaviorClose", "missing")
        counts[behavior] += 1
        if behavior not in PASSING or close not in PASSING:
            failures.append(
                f"  {case_id}: behavior {behavior}, behaviorClose {close}"
                f" (report {result.get('reportfile', 'none')})"
            )

    summary = ", ".join(f"{count} {behavior}" for behavior, count in sorted(counts.items()))
    print(f"{args.index}: {args.agent}: {len(cases)} cases: {summary}")

    ok = True
    if len(cases) != args.cases:
        print(f"expected {args.cases} cases, found {len(cases)}")
        ok = False
    if failures:
        print(f"{len(failures)} failing:")
        print("\n".join(failures))
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
