#!/bin/sh
# Install mib, Markdown under a mebibyte, from its GitHub releases:
#
#   curl -fsSL https://osigurdson.github.io/mib/install.sh | sh
#
# Installs the binary, desktop entry and icon under ~/.local (no sudo), after checking the
# download against the release's SHA256SUMS, and its build provenance too when the GitHub
# CLI is signed in. Run it again to upgrade.
#
#   MIB_VERSION=v0.1.0   install that release instead of the latest
#   MIB_PREFIX=DIR       install under DIR instead of ~/.local
#
# Source: https://github.com/osigurdson/mib/blob/main/site/install.sh
set -eu

REPO=osigurdson/mib

say() { printf '%s\n' "$*"; }
fail() { printf 'mib install: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "needs $1; install it and try again"; }

# Everything runs from main, so a partly downloaded script does nothing.
main() {
    [ "$(uname -s)" = Linux ] || fail "prebuilt binaries are for Linux; see https://github.com/$REPO to build elsewhere"
    [ "$(uname -m)" = x86_64 ] || fail "prebuilt binaries are for x86_64, not $(uname -m); see https://github.com/$REPO to build from source"
    for tool in curl tar sha256sum install sed mktemp; do need "$tool"; done

    glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2 || true)
    [ -n "$glibc" ] || fail "the prebuilt binary needs glibc (musl systems such as Alpine aren't supported)"
    case $glibc in
        2.[0-9] | 2.[0-9].* | 2.[0-2][0-9] | 2.[0-2][0-9].* | 2.3[0-8] | 2.3[0-8].*)
            fail "the prebuilt binary needs glibc 2.39 or newer, and this system has $glibc; build from source instead" ;;
    esac

    version=${MIB_VERSION:-}
    if [ -z "$version" ]; then
        # The latest release page redirects to .../releases/tag/vX.Y.Z.
        url=$(curl -fsSLI -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest") ||
            fail "couldn't reach GitHub to find the latest release"
        version=${url##*/}
        case $version in v*) ;; *) fail "couldn't find the latest release (got $url)" ;; esac
    fi

    file="mib-$version-linux-x86_64.tar.gz"
    base="https://github.com/$REPO/releases/download/$version"
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    say "Downloading mib $version"
    curl -fsSL -o "$tmp/$file" "$base/$file" || fail "couldn't download $base/$file"
    curl -fsSL -o "$tmp/SHA256SUMS" "$base/SHA256SUMS" || fail "couldn't download $base/SHA256SUMS"

    grep "  $file\$" "$tmp/SHA256SUMS" >"$tmp/expected" || fail "SHA256SUMS has no entry for $file"
    (cd "$tmp" && sha256sum -c --quiet expected) || fail "checksum mismatch for $file; not installing it"
    say "Checksum OK"

    if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        gh attestation verify "$tmp/$file" --repo "$REPO" >/dev/null 2>&1 ||
            fail "build provenance check failed for $file; not installing it"
        say "Build provenance OK"
    fi

    tar -xzf "$tmp/$file" -C "$tmp"
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
