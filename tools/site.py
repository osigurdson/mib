#!/usr/bin/env python3
"""Build the download page into a folder for GitHub Pages.

Fills site/index.html with the latest GitHub release: how to install it on Linux, notes on
Windows and macOS, then where the files come from (the commit, each file's size and SHA-256,
and how to check its build provenance). It also writes install.sh with that release built
in. Release data comes from the gh CLI (GH_TOKEN and GH_REPO in the Pages workflow). Without
a release, or without gh, the page says how to build from source instead.

    python3 tools/site.py OUT [--release release.json]

--release takes a JSON file shaped like the gh output below, for previewing locally:
{"tag": "v0.1.0", "date": "2026-11-08", "url": "...", "commit": "<sha>",
 "assets": [{"name": "...", "size": 123, "url": "...", "sha256": "..."}]}
"""
import argparse
import html
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = os.environ.get("GH_REPO", "osigurdson/mib")
REPO_URL = f"https://github.com/{REPO}"


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True, check=True).stdout


def latest_release():
    try:
        info = json.loads(gh("release", "view", "--json", "tagName,publishedAt,url,assets"))
    except (OSError, subprocess.CalledProcessError):
        return None
    tag = info["tagName"]
    sums = {}
    for line in gh("release", "download", tag, "--pattern", "SHA256SUMS", "--output", "-").splitlines():
        digest, name = line.split(maxsplit=1)
        sums[name.lstrip("*")] = digest
    commit = gh("api", f"repos/{REPO}/commits/{tag}", "--jq", ".sha").strip()
    assets = [{"name": a["name"], "size": a["size"], "url": a["url"], "sha256": sums.get(a["name"], "")}
              for a in info["assets"]]
    return {"tag": tag, "date": info["publishedAt"][:10], "url": info["url"], "commit": commit,
            "assets": assets}


def size(n):
    return f"{n / 1024:.0f} KiB" if n < 1024 * 1024 else f"{n / 1024 / 1024:.1f} MiB"


def page_html(r):
    """Linux install, Windows, macOS, then how a release is built and checked (with a release)."""
    files = [a for a in r["assets"] if a["name"].startswith("mib-")] if r else []
    windows = next((a["name"] for a in files if "windows" in a["name"]), None)
    return linux_html(r) + windows_html(windows) + MACOS_HTML + (checking_html(r, files) if r else "")


def linux_html(r):
    if not r:
        return f"""<h2>Install on Linux</h2>
<p>There is no release yet. Build it from source:</p>
<pre>git clone {REPO_URL}
cd mib
make install</pre>
<p>See the <a href="{REPO_URL}#build-and-run-linux-wayland">README</a> for the packages you
need.</p>
"""
    e = html.escape
    return f"""<h2>Install on Linux</h2>
<pre>curl -fsSL https://osigurdson.github.io/mib/install.sh | sh</pre>
<p>This installs mib {e(r["tag"])} (released {e(r["date"])}, <a href="{e(r["url"])}">release
notes</a>) to <code>~/.local</code>, with no sudo; run it again to upgrade. It needs x86_64, a
Wayland desktop (GNOME, KDE Plasma, Hyprland, Sway…) and glibc 2.39 or newer, as in
Ubuntu 24.04, Fedora 40, Debian 13 and Arch. Elsewhere,
<a href="{REPO_URL}#build-and-run-linux-wayland">build from source</a>.</p>
"""


def windows_html(name):
    if not name:
        return f"""
<h2>Windows</h2>
<p>The Windows build is experimental and not released yet; see
<a href="{REPO_URL}#appendix-windows-experimental">building it</a>.</p>
"""
    return f"""
<h2>Windows</h2>
<p>Download <code>{html.escape(name)}</code> below and run it; there is no installer. Windows 10
(1703) or newer. The file isn't code-signed yet, so SmartScreen may ask you to confirm.</p>
"""


MACOS_HTML = f"""
<h2>macOS</h2>
<p>Not supported yet. If you'd use it, say so in
<a href="{REPO_URL}/issues">an issue</a>; that decides whether it happens.</p>
"""


def checking_html(r, files):
    e = html.escape
    tag, commit = r["tag"], r["commit"]
    linux = next((a["name"] for a in files if "linux" in a["name"]), "mib-linux.tar.gz")
    rows = "".join(
        f'<tr><td><a href="{e(a["url"])}">{e(a["name"])}</a>'
        f'<code class="sum">sha256 {e(a["sha256"])}</code></td>'
        f'<td class="size">{size(a["size"])}</td></tr>' for a in files)
    return f"""
<h2>Where the files come from</h2>
<p>Nobody builds a release on their own computer. Pushing the tag {e(tag)} made GitHub Actions
build commit <a href="{REPO_URL}/commit/{e(commit)}"><code>{e(commit[:12])}</code></a> on
GitHub's servers, run the tests and publish the files below. Two things let you check that
what you have is what that run built:</p>
<ul>
<li><strong>Checksum.</strong> Each file's SHA-256 is listed below. The install script has the
Linux one built in and refuses a download that doesn't match.</li>
<li><strong>Build provenance.</strong> GitHub also signs a statement for each file naming the
repository, commit and workflow run that produced it. A file built anywhere else, or changed
afterwards, fails the check. With the <a href="https://cli.github.com">GitHub CLI</a>:
<pre>gh attestation verify {e(linux)} --repo {e(REPO)}</pre></li>
</ul>
<table class="files">{rows}</table>
<p><code>mib --version</code> shows the release and commit you're running. All
<a href="{REPO_URL}/releases">releases</a> and their
<a href="{REPO_URL}/attestations">attestations</a> are on GitHub.</p>
"""


def installer(release):
    """site/install.sh with the release and its Linux tarball's SHA-256 filled in. Without a
    release, the placeholders stay and the script says so when run."""
    script = (ROOT / "site/install.sh").read_text()
    if not release:
        return script
    name = f"mib-{release['tag']}-linux-x86_64.tar.gz"
    digest = next((a["sha256"] for a in release["assets"] if a["name"] == name), "")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise SystemExit(f"site.py: release {release['tag']} has no SHA-256 for {name}")
    return script.replace("@VERSION@", release["tag"]).replace("@SHA256@", digest)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out", type=Path)
    parser.add_argument("--release", type=Path, help="release JSON, for a local preview")
    args = parser.parse_args()
    release = json.loads(args.release.read_text()) if args.release else latest_release()
    page = (ROOT / "site/index.html").read_text()
    page = page.replace("<!-- RELEASE -->", page_html(release))
    assets = args.out / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    (args.out / "index.html").write_text(page)
    for name in ("wordmark-dark.svg", "wordmark-light.svg", "icon-small.svg", "social-preview.png"):
        shutil.copy(ROOT / "docs/brand" / name, assets / name)
    shutil.copy(ROOT / "docs/demo.svg", assets / "demo.svg")
    (args.out / "install.sh").write_text(installer(release))
    print("wrote", args.out / "index.html", "for", release["tag"] if release else "no release")


if __name__ == "__main__":
    main()
