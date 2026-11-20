#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`farm ready`: READY-TO-MERGE table for all open PRs (head, covering smoke job, GitHub checks, review threads, symlinks, mergeable).
Pure decision functions (no network) are unit-tested in test_farm.py; fetch_* shell out to gh/git."""
import json, os, subprocess

REPO_DEFAULT = "pgm1207/mali-open"
BAD_PATHS = ("third_party", "vk/deqp/lists", "vk/deqp/out")


def covering_job(jobs, head, is_anc, suite="smoke"):
    """newest job of SUITE whose tested commit contains HEAD (head itself, or a --merge-with-main commit with head as parent).
    -> (job|None, stale_job|None, unknown_job|None): stale = newest job whose commit is an ANCESTOR of head (an older head was tested, the current one was not).
    Jobs submitted with --only run a subset of the cases and never count as the full gate. is_anc returns True/False, or None when git
    could not tell (e.g. a pruned --merge-with-main commit): such a job is reported as unknown, never as "not an ancestor"."""
    cover = stale = unknown = None
    for j in reversed(jobs):
        if j.get("suite") != suite or j.get("dirty") or j.get("only") or not j.get("commit"):
            continue
        c = j["commit"]
        a = c == head or is_anc(head, c)
        b = None if a else is_anc(c, head)
        if a is None or (not a and b is None):
            unknown = unknown or j
            continue
        if cover is None and a:
            cover = j
        elif stale is None and b:
            stale = j
        if cover:
            break
    return cover, stale, unknown


def job_state(j, verdict_fn):
    v = verdict_fn(j)
    if v == "pass" and any(p.get("state") == "cancelled" for p in j.get("parts", {}).values()):
        return "partial"   # a requested part was cancelled: the other parts passing is not the full gate
    return {"pass": "pass", "pending": "pending", "cancelled": "none"}.get(v, "FAIL")


def checks_state(rollup):
    """gh statusCheckRollup -> ok | pending | FAIL | none"""
    if not rollup:
        return "none"
    bad = pend = False
    for c in rollup:
        status = (c.get("status") or "").upper()
        concl = (c.get("conclusion") or c.get("state") or "").upper()
        if status and status != "COMPLETED" or concl in ("PENDING", "EXPECTED", ""):
            pend = True
        elif concl not in ("SUCCESS", "NEUTRAL", "SKIPPED"):
            bad = True
    return "FAIL" if bad else "pending" if pend else "ok"


def bad_files(paths):
    """symlinks are checked by the caller (mode 120000); here: files that must never be in a PR"""
    return [p for p in paths if any(p == b or p.startswith(b + "/") for b in BAD_PATHS)]


def verdict(row):
    """READY only when every column is green; otherwise the first blocker"""
    if row["smoke"] == "none":
        return "no smoke for head"
    if row["smoke"] == "partial":
        return "smoke job has cancelled part(s)"
    if row["smoke"] == "unknown":
        return "smoke ancestry unknown (commit not resolvable)"
    if row["smoke"] == "stale":
        return "smoke is for an older head"
    if row["smoke"] == "pending":
        return "smoke pending"
    if row["smoke"] == "FAIL":
        return "smoke FAIL"
    if row["checks"] in ("FAIL", "pending"):
        return f"GitHub checks {row['checks']}"
    if row["threads"]:
        return f"{row['threads']} unresolved thread(s)"
    if row["links"]:
        return "symlink/forbidden files: " + ",".join(row["links"][:3])
    if row["mergeable"] != "MERGEABLE":
        return f"not mergeable ({row['mergeable']})"
    if row["draft"]:
        return "draft"
    return "READY"


def make_row(pr, jobs, is_anc, verdict_fn, threads, links):
    head = pr["headRefOid"]
    cover, stale, unknown = covering_job(jobs, head, is_anc)
    smoke = job_state(cover, verdict_fn) if cover else ("unknown" if unknown else "stale" if stale else "none")
    if not cover and unknown and not stale:
        stale = unknown
    row = {"pr": pr["number"], "head": head[:8], "title": (pr.get("title") or "")[:40], "draft": bool(pr.get("isDraft")),
           "smoke": smoke, "job": (cover or stale or {}).get("id", "-")[9:] if (cover or stale) else "-",
           "checks": checks_state(pr.get("statusCheckRollup")), "threads": threads, "links": links,
           "mergeable": pr.get("mergeable") or "UNKNOWN"}
    row["verdict"] = verdict(row)
    return row


def render(rows):
    h = f"{'PR':>5} {'head':8} {'smoke':8} {'job':14} {'checks':8} {'thr':>3} {'merge':11} verdict"
    out = [h]
    for r in sorted(rows, key=lambda r: (r["verdict"] != "READY", r["pr"])):
        out.append(f"{r['pr']:>5} {r['head']:8} {r['smoke']:8} {r['job'][:14]:14} {r['checks']:8} {r['threads']:>3} {r['mergeable'][:11]:11} {r['verdict']}")
    out.append(f"{sum(r['verdict'] == 'READY' for r in rows)} READY of {len(rows)} open PRs")
    return "\n".join(out)


def gh(*a):
    r = subprocess.run(["gh", *a], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("farm: gh " + " ".join(a[:3]) + ": " + r.stderr.strip()[:200])
    return r.stdout


def fetch_prs(repo):
    return json.loads(gh("pr", "list", "-R", repo, "--state", "open", "--limit", "200", "--json",
                         "number,title,headRefOid,isDraft,mergeable,statusCheckRollup"))


def fetch_threads(repo, n):
    """unresolved review threads; follows the GraphQL cursor so thread 101+ is counted"""
    owner, name = repo.split("/")
    q = ('query($o:String!,$n:String!,$p:Int!,$c:String){repository(owner:$o,name:$n){pullRequest(number:$p){'
         'reviewThreads(first:100,after:$c){pageInfo{hasNextPage endCursor} nodes{isResolved isOutdated}}}}}')
    total, cursor = 0, None
    while True:
        args = ["api", "graphql", "-f", f"query={q}", "-f", f"o={owner}", "-f", f"n={name}", "-F", f"p={n}"]
        if cursor:
            args += ["-F", f"c={cursor}"]
        rt = json.loads(gh(*args))["data"]["repository"]["pullRequest"]["reviewThreads"]
        total += sum(1 for t in rt["nodes"] if not t["isResolved"])
        if not rt["pageInfo"]["hasNextPage"]:
            return total
        cursor = rt["pageInfo"]["endCursor"]


def symlink_paths(entries_by_dir, files):
    """pure: files whose tree entry at head has mode 120000. entries_by_dir: dir -> {name: mode}"""
    out = []
    for f in files:
        d, _, base = f.rpartition("/")
        if entries_by_dir.get(d, {}).get(base) == "120000":
            out.append(f)
    return out


def fetch_links(repo, n, head):
    """changed files that are forbidden paths or symlinks: every file page is read (--paginate) and every non-removed file's entry in the git tree
    at head is checked for mode 120000 (the contents API follows links to files, and patch size says nothing about the file type).
    Any gh failure aborts (gh() exits) so a partial inspection never yields READY."""
    d = [f for page in json.loads(gh("api", "--paginate", "--slurp", f"repos/{repo}/pulls/{n}/files?per_page=100")) for f in page]
    bad = bad_files([f["filename"] for f in d])
    live = [f["filename"] for f in d if f.get("status") != "removed" and f["filename"] not in bad]
    trees = {}
    for dr in sorted({f.rpartition("/")[0] for f in live}):
        t = json.loads(gh("api", f"repos/{repo}/git/trees/{head}" + (f":{dr}" if dr else "")))
        trees[dr] = {e["path"]: e["mode"] for e in t.get("tree", [])}
    return bad + symlink_paths(trees, live)


def run(repo, jobs, is_anc, verdict_fn, only=None):
    rows = []
    for pr in fetch_prs(repo):
        if only and pr["number"] not in only:
            continue
        rows.append(make_row(pr, jobs, is_anc, verdict_fn, fetch_threads(repo, pr["number"]), fetch_links(repo, pr["number"], pr["headRefOid"])))
    return rows
