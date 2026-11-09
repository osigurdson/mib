#!/usr/bin/env python3
"""Build the download page into a folder for GitHub Pages.

Fills site/index.html with the latest GitHub release: its version, the commit it was built
from, each file with its size and SHA-256, and how to verify and install them. Release data
comes from the gh CLI (GH_TOKEN and GH_REPO in the Pages workflow). Without a release, or
without gh, the page says how to build from source instead.

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


def release_html(r):
    e = html.escape
    repo = f"https://github.com/{REPO}"
    tag, commit = r["tag"], r["commit"]
    files = [a for a in r["assets"] if a["name"].startswith("mib-")]
    linux = next((a["name"] for a in files if "linux" in a["name"]), "mib-linux.tar.gz")
    windows = next((a["name"] for a in files if "windows" in a["name"]), None)
    rows = "".join(
        f'<tr><td><a href="{e(a["url"])}">{e(a["name"])}</a>'
        f'<code class="sum">sha256 {e(a["sha256"])}</code></td>'
        f'<td class="size">{size(a["size"])}</td></tr>' for a in files)
    sums = next((a for a in r["assets"] if a["name"] == "SHA256SUMS"), None)
    sums_link = f' and <a href="{e(sums["url"])}">SHA256SUMS</a>' if sums else ""
    return f"""<h2>Download mib {e(tag)}</h2>
<p class="meta">Released {e(r["date"])}. Built by GitHub Actions from commit
<a href="{repo}/commit/{e(commit)}"><code>{e(commit[:12])}</code></a>, the commit tagged
<a href="{repo}/tree/{e(tag)}">{e(tag)}</a>. <a href="{e(r["url"])}">Release notes</a>.</p>
<table class="files">{rows}</table>

<h3>Check what you downloaded</h3>
<p>Each file has a signed <a href="{repo}/attestations">build provenance attestation</a> that
ties it to the commit and the workflow run that built it{sums_link}.</p>
<pre>sha256sum -c SHA256SUMS --ignore-missing
gh attestation verify {e(linux)} --repo {e(REPO)}
mib --version     # mib {e(tag)}, commit {e(commit[:12])}…</pre>

<h3>Install on Linux</h3>
<p>x86_64 with a Wayland desktop (GNOME, KDE Plasma, Hyprland, Sway…) and glibc 2.39 or newer,
as in Ubuntu 24.04, Fedora 40, Debian 13 and Arch. This installs to <code>~/.local</code>, with
no sudo, after checking the download's checksum. Run it again to upgrade.</p>
<pre>curl -fsSL https://osigurdson.github.io/mib/install.sh | sh</pre>
<p>To read <a href="install.sh">the script</a> first:</p>
<pre>curl -fsSLO https://osigurdson.github.io/mib/install.sh
less install.sh
sh install.sh</pre>
<p>Or by hand, from the tarball above:</p>
<pre>tar xzf {e(linux)}
install -Dm755 mib/mib ~/.local/bin/mib
install -Dm644 mib/io.github.osigurdson.mib.desktop -t ~/.local/share/applications
install -Dm644 mib/io.github.osigurdson.mib.svg -t ~/.local/share/icons/hicolor/scalable/apps</pre>
{windows_html(windows)}"""


def windows_html(name):
    if not name:
        return """
<h3>Windows</h3>
<p>The Windows build is experimental and not released yet; see
<a href="https://github.com/osigurdson/mib#appendix-windows-experimental">building it</a>.</p>
"""
    return f"""
<h3>Windows</h3>
<p>Download <code>{html.escape(name)}</code> and run it; there is no installer. Windows 10
(1703) or newer. The file isn't code-signed yet, so SmartScreen may ask you to confirm.</p>
"""


SOURCE_HTML = """<h2>Get mib</h2>
<p>There is no release yet. Build it from source on Linux:</p>
<pre>git clone https://github.com/osigurdson/mib
cd mib
make install</pre>
<p>See the <a href="https://github.com/osigurdson/mib#build-and-run-linux-wayland">README</a>
for the packages you need.</p>
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
    page = page.replace("<!-- RELEASE -->", release_html(release) if release else SOURCE_HTML)
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
