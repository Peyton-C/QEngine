#!/usr/bin/env python3
"""report.py RESULTS_DIR BASELINE_DIR [--save] NAME... : summarise fake Engine runs.

Prints one row per instance, then every failure, then every result that differs
from the saved baseline. The baseline is what catches a shim that stopped
interposing something: the fake then reports the call as skipped, not failed,
because nothing claims it works any more. With --save the results become the
baseline. Exits 1 on a failure, a changed result or a run that did not finish."""
import os
import shutil
import sys


def parse(path):
    """Result lines as {name: (status, detail)}, and whether the run finished."""
    results, finished = {}, False
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError:
        return results, finished
    for line in lines:
        if line.startswith("1.."):
            finished = True
        if line.startswith("not ok "):
            status, rest = "fail", line[7:]
        elif line.startswith("ok "):
            status, rest = "ok", line[3:]
        else:
            continue
        name = rest.split(" ", 1)[0]
        detail = rest[len(name):].strip()
        if detail.startswith("# SKIP"):
            status, detail = "skip", detail[6:].strip()
        results[name] = (status, detail.lstrip("- "))
    return results, finished


def main():
    args = [a for a in sys.argv[1:] if a != "--save"]
    save = "--save" in sys.argv
    results_dir, baseline_dir, names = args[0], args[1], args[2:]
    bad = False
    rows, failures, changes = [], {}, {}
    for name in names:
        tap = os.path.join(results_dir, name + ".tap")
        base = os.path.join(baseline_dir, name + ".tap")
        got, finished = parse(tap)
        count = {s: sum(1 for v in got.values() if v[0] == s) for s in ("ok", "fail", "skip")}
        note = "" if finished else "did not finish, see %s.log" % name
        for test, (status, detail) in got.items():
            if status == "fail":
                failures.setdefault("%s - %s" % (test, detail), []).append(name)
        if os.path.exists(base) and not save:
            was, _ = parse(base)
            for test in sorted(set(got) | set(was)):
                now, then = got.get(test, ("gone", ""))[0], was.get(test, ("new", ""))[0]
                if now != then:
                    changes.setdefault("%s was %s, now %s" % (test, then, now), []).append(name)
        elif not save:
            note = note or "no baseline"
        if save and finished:
            os.makedirs(baseline_dir, exist_ok=True)
            shutil.copyfile(tap, base)
            note = "saved as baseline"
        bad = bad or not finished or count["fail"] > 0
        rows.append((name, count["ok"], count["fail"], count["skip"], note))

    width = max([len(r[0]) for r in rows] + [8])
    print("%-*s  %4s %4s %4s" % (width, "instance", "ok", "fail", "skip"))
    for name, ok, fail, skip, note in rows:
        print("%-*s  %4d %4d %4d  %s" % (width, name, ok, fail, skip, note))
    # One line per result, with the instances that share it: a shim change
    # usually breaks the same thing on a whole era.
    for title, found in (("failures", failures), ("changed since the baseline", changes)):
        if found:
            print("\n%s:" % title)
        for what, where in found.items():
            print("  %s\n      %s" % (what, " ".join(where)))
    sys.exit(1 if bad or changes else 0)


main()
