#!/bin/sh
# Install mib, Markdown under a mebibyte, from its latest GitHub release:
#
#   curl -fsSL https://osigurdson.github.io/mib/install.sh | sh
#
# Installs the binary, desktop entry and icon under ~/.local (no sudo), after checking the
# download against the SHA-256 below. Run it again to upgrade.
#
#   MIB_PREFIX=DIR   install under DIR instead of ~/.local
#
# tools/site.py fills in the release and its SHA-256 when it publishes this script, so the
# copy in the repository doesn't run on its own. For an older release, download its tarball
# from https://github.com/osigurdson/mib/releases and install it by hand.
set -eu

REPO=osigurdson/mib
VERSION=@VERSION@
SHA256=@SHA256@

say() { printf '%s\n' "$*"; }
fail() { printf 'mib install: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "needs $1; install it and try again"; }

# Everything runs from main, so a partly downloaded script does nothing.
main() {
    case $VERSION in v*) ;; *) fail "this copy of the script names no release; use https://osigurdson.github.io/mib/install.sh or build from https://github.com/$REPO" ;; esac
    [ "$(uname -s)" = Linux ] || fail "prebuilt binaries are for Linux; see https://github.com/$REPO to build elsewhere"
    [ "$(uname -m)" = x86_64 ] || fail "prebuilt binaries are for x86_64, not $(uname -m); see https://github.com/$REPO to build from source"
    for tool in curl tar sha256sum install sed mktemp; do need "$tool"; done

    file="mib-$VERSION-linux-x86_64.tar.gz"
    url="https://github.com/$REPO/releases/download/$VERSION/$file"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    say "Downloading mib $VERSION"
    curl -fsSL -o "$tmp/$file" "$url" || fail "couldn't download $url"
    (cd "$tmp" && printf '%s  %s\n' "$SHA256" "$file" | sha256sum -c --quiet -) >/dev/null 2>&1 ||
        fail "checksum mismatch for $file; not installing it"
    say "Checksum OK"

    tar -xzf "$tmp/$file" -C "$tmp"
    "$tmp/mib/mib" --version >/dev/null 2>&1 ||
        fail "the prebuilt binary doesn't run here: it needs glibc 2.39 or newer, libwayland-client and libxkbcommon; see https://github.com/$REPO to build from source"

    prefix=${MIB_PREFIX:-$HOME/.local}
    bin="$prefix/bin"
    apps="$prefix/share/applications"
    icons="$prefix/share/icons/hicolor/scalable/apps"
    install -Dm755 "$tmp/mib/mib" "$bin/mib"
    # Point the launcher at the installed binary, so it works even if bin isn't on PATH.
    sed "s|^Exec=mib\$|Exec=$bin/mib|" "$tmp/mib/io.github.osigurdson.mib.desktop" >"$tmp/mib.desktop"
    install -Dm644 "$tmp/mib.desktop" "$apps/io.github.osigurdson.mib.desktop"
    install -Dm644 "$tmp/mib/io.github.osigurdson.mib.svg" "$icons/io.github.osigurdson.mib.svg"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$apps" 2>/dev/null || true

    say "Installed $("$bin/mib" --version 2>&1 | tr '\n' ' ')"
    say "  $bin/mib, with a launcher entry and icon"
    case ":${PATH:-}:" in
        *":$bin:"*) ;;
        *) say "Add $bin to your PATH to run mib from a terminal." ;;
    esac
    [ -n "${WAYLAND_DISPLAY:-}" ] || say "Note: mib needs a Wayland session (GNOME, KDE Plasma, Hyprland, Sway...)."
}

main "$@"
