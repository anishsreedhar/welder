#!/usr/bin/env python3
"""Fails when tracked files, or the commits being added, look like they leak a credential.

  python3 tools/check_secrets.py                 # scan every tracked file
  python3 tools/check_secrets.py --commits A..B  # also check the author / committer emails of A..B

Checks file contents for API keys and tokens (Anthropic, OpenAI, Hugging Face, GitHub, AWS, Slack,
private keys), and for Claude Code's local settings files, which can hold a person's own setup.
With --commits, every author and committer email must be a project or no-reply address, so a
personal address doesn't land in the public history by mistake (set the email with
`git config user.email`). Run by .github/workflows/secrets.yml on every push and pull request.
"""
import argparse
import re
import subprocess
import sys

SECRETS = {
    "Anthropic API key": r"sk-ant-[A-Za-z0-9_-]{20,}",
    "OpenAI API key": r"sk-(?:proj-)?[A-Za-z0-9]{32,}",
    "Hugging Face token": r"hf_[A-Za-z0-9]{30,}",
    "GitHub token": r"(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{60,}",
    "AWS access key": r"AKIA[0-9A-Z]{16}",
    "Slack token": r"xox[abprs]-[A-Za-z0-9-]{10,}",
    "Private key": r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY",
    "API key assignment": r"(?i)(?:ANTHROPIC|OPENAI|CLAUDE)_API_KEY\s*[=:]\s*['\"]?[A-Za-z0-9_-]{16,}",
}
# local files that should never be committed (Claude Code's per-person settings, env files, keys)
BAD_PATHS = r"(^|/)(\.claude/settings\.local\.json|\.claude/\.credentials\.json|CLAUDE\.local\.md|\.env(\.[^/]*)?|[^/]*\.pem|[^/]*\.p12|hf_token)$"
# addresses that may appear on commits: the project's, GitHub's and Claude's no-reply addresses
OK_EMAILS = r"(?i)^(weldereffects@gmail\.com|noreply@github\.com|noreply@anthropic\.com|[^@]+@users\.noreply\.github\.com|[0-9]+\+[^@]+@users\.noreply\.github\.com)$"
SKIP = ("third_party/", "licenses/")


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def scan_files():
    found = []
    pats = {name: re.compile(p) for name, p in SECRETS.items()}
    for path in git("ls-files", "-z").split("\0"):
        if not path or path.startswith(SKIP):
            continue
        if re.search(BAD_PATHS, path) and not path.endswith(".example"):
            found.append(f"{path}: a local settings / key file is tracked")
            continue
        try:
            with open(path, "rb") as f:
                data = f.read(4 << 20)
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue  # binary
        text = data.decode("utf-8", "replace")
        for n, line in enumerate(text.splitlines(), 1):
            for name, pat in pats.items():
                if pat.search(line):
                    found.append(f"{path}:{n}: looks like a {name}")
    return found


def scan_commits(rng):
    found = []
    for line in git("log", "--format=%h%x09%ae%x09%ce", rng).splitlines():
        sha, author, committer = line.split("\t")
        for who, email in (("author", author), ("committer", committer)):
            if not re.match(OK_EMAILS, email):
                found.append(f"commit {sha}: {who} email isn't a project or no-reply address")
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--commits", help="a revision range (A..B) whose emails to check")
    a = ap.parse_args()
    found = scan_files()
    if a.commits:
        found += scan_commits(a.commits)
    for f in found:
        print(f)
    if found:
        print(f"\n{len(found)} problem(s). Remove the secret (and rotate it if it was ever pushed).")
        return 1
    print("No secrets found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
