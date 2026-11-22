#!/usr/bin/env python3
# TheSuperHackers @build O144 Turns the compiler errors of a CI build log into GitHub annotations and a job summary, which
# a public repository shows without signing in (the raw job log needs a signed-in account).
#
#   report_errors.py build.log
import os
import re
import sys

log = sys.argv[1]
repo = os.environ.get("GITHUB_WORKSPACE", os.getcwd()).replace("\\", "/").rstrip("/") + "/"
pattern = re.compile(r"^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?P<kind>fatal error|error):\s*(?P<msg>.*)$")
errors = []
seen = set()
other = []
for raw in open(log, errors="replace"):
    line = raw.rstrip("\n")
    m = pattern.match(line)
    if m:
        path = m.group("file").replace("\\", "/")
        if path.startswith(repo):
            path = path[len(repo):]
        key = (path, m.group("line"), m.group("msg"))
        if key not in seen:
            seen.add(key)
            errors.append((path, m.group("line"), m.group("col") or "1", m.group("msg")))
    elif ("error" in line.lower() or "FAILED" in line) and len(other) < 40:
        other.append(line[:300])

# GitHub keeps at most 10 error annotations per step; the first ones are the useful ones (later errors often follow
# from them). Every error goes to the summary.
for path, line, col, msg in errors[:10]:
    print("::error file=%s,line=%s,col=%s::%s" % (path, line, col, msg.replace("%", "%25")[:500]))
if not errors:
    # No compiler error lines (a linker or ninja failure): the first lines that name an error.
    for line in other[:10]:
        print("::error::%s" % line.replace("%", "%25"))
summary = os.environ.get("GITHUB_STEP_SUMMARY")
lines = ["### %d distinct compiler errors" % len(errors), "", "```"]
lines += ["%s:%s:%s: %s" % e for e in errors[:300]]
lines += ["```"]
if not errors:
    lines += ["", "Other lines with 'error' or 'FAILED':", "```"] + other + ["```"]
text = "\n".join(lines) + "\n"
print(text)
if summary:
    with open(summary, "a") as f:
        f.write(text)
