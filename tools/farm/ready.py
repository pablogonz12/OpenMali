#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`farm ready`: READY-TO-MERGE table for all open PRs (head, covering smoke job, GitHub checks, review threads, symlinks, mergeable).
Pure decision functions (no network) are unit-tested in test_farm.py; fetch_* shell out to gh/git."""
import json, os, subprocess

REPO_DEFAULT = "pgm1207/mali-open"
BAD_PATHS = ("third_party", "vk/deqp/lists", "vk/deqp/out")


def covering_job(jobs, head, is_anc, suite="smoke"):
    """newest job of SUITE whose tested commit contains HEAD (head itself, or a --merge-with-main commit with head as parent).
    -> (job|None, stale_job|None): stale = newest job whose commit is an ANCESTOR of head (an older head was tested, the current one was not)"""
    cover = stale = None
    for j in reversed(jobs):
        if j.get("suite") != suite or j.get("dirty") or not j.get("commit"):
            continue
        c = j["commit"]
        if cover is None and (c == head or is_anc(head, c)):
            cover = j
        elif stale is None and is_anc(c, head):
            stale = j
        if cover:
            break
    return cover, stale


def job_state(j, verdict_fn):
    v = verdict_fn(j)
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
    cover, stale = covering_job(jobs, head, is_anc)
    smoke = job_state(cover, verdict_fn) if cover else ("stale" if stale else "none")
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
    owner, name = repo.split("/")
    q = 'query($o:String!,$n:String!,$p:Int!){repository(owner:$o,name:$n){pullRequest(number:$p){reviewThreads(first:100){nodes{isResolved isOutdated}}}}}'
    d = json.loads(gh("api", "graphql", "-f", f"query={q}", "-F", f"o={owner}", "-F", f"n={name}", "-F", f"p={n}"))
    nodes = d["data"]["repository"]["pullRequest"]["reviewThreads"]["nodes"]
    return sum(1 for t in nodes if not t["isResolved"])


def fetch_links(repo, n, head):
    """changed files that are forbidden paths or symlinks (contents API type == symlink; only tiny patches are asked, a link target is one line)"""
    d = json.loads(gh("api", f"repos/{repo}/pulls/{n}/files?per_page=100"))
    bad = bad_files([f["filename"] for f in d])
    for f in d:
        if f.get("status") != "removed" and f["filename"] not in bad and f.get("changes", 0) <= 2:
            r = subprocess.run(["gh", "api", f"repos/{repo}/contents/{f['filename']}?ref={head}", "--jq", ".type"], capture_output=True, text=True)
            if r.stdout.strip() == "symlink":
                bad.append(f["filename"])
    return bad


def run(repo, jobs, is_anc, verdict_fn, only=None):
    rows = []
    for pr in fetch_prs(repo):
        if only and pr["number"] not in only:
            continue
        rows.append(make_row(pr, jobs, is_anc, verdict_fn, fetch_threads(repo, pr["number"]), fetch_links(repo, pr["number"], pr["headRefOid"])))
    return rows
