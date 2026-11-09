#!/bin/bash
# mac-install.sh - install built .plugin bundles into After Effects on macOS.
#
# The CMake deploy step is deliberately non-fatal: AE's Plug-ins folder under
# /Applications is root:wheel, so an unprivileged build cannot write there and
# prints "skipping deploy" instead of failing (docs/MAC_BUILD.md SS3). This is
# the other half - run it after a build to actually install, with one sudo
# prompt for the whole set.
#
#   ./tools/mac-install.sh                    # all five, AE 2026, build-mac/
#   ./tools/mac-install.sh EdgeRepair         # just one (repeatable)
#   ./tools/mac-install.sh --ae 2024          # a different AE version
#   ./tools/mac-install.sh --dry-run          # preflight only, no sudo, no copy
#
#   --build <dir>   build tree to install from   (default: build-mac)
#   --ae <ver|path> AE version or a full path    (default: 2026)
#
# Preflight refuses to install a bundle that would load and do nothing. The
# PiPL check is the important one: Rez without -useDF exits 0 and leaves a
# 0-byte .rsrc with the resource stranded in an xattr, which AE ignores - the
# failure that costs a debugging session because everything looks fine.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/build-mac"
AE="2026"
DRY_RUN=0
ALL_PLUGINS=(EdgeRepair GrainTransfer ModernEdgeBlur MM_Qualify MM_FreqSep)
WANTED=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)   BUILD_DIR="$2"; shift 2 ;;
        --ae)      AE="$2";        shift 2 ;;
        --dry-run) DRY_RUN=1;      shift ;;
        -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
        -*)        echo "unknown option: $1" >&2; exit 2 ;;
        *)         WANTED+=("$1"); shift ;;
    esac
done

[[ ${#WANTED[@]} -eq 0 ]] && WANTED=("${ALL_PLUGINS[@]}")

# --ae takes either a version number or a full path to a Plug-ins folder.
if [[ "$AE" == /* ]]; then
    DEST="$AE"
else
    DEST="/Applications/Adobe After Effects $AE/Plug-ins"
fi

echo "Source : $BUILD_DIR"
echo "Target : $DEST"
echo

if [[ ! -d "$DEST" ]]; then
    echo "ERROR: no such folder: $DEST" >&2
    echo "       Installed AE versions:" >&2
    ls -d /Applications/Adobe\ After\ Effects\ * 2>/dev/null | sed 's/^/         /' >&2
    exit 1
fi

# AE caches its plugin list at launch and holds the bundle open; installing
# underneath a running AE gets you a stale or half-read plugin.
if pgrep -x "After Effects" >/dev/null 2>&1; then
    echo "WARNING: After Effects is running. Quit it before installing, or it"
    echo "         will keep using the copy it loaded at launch."
    echo
fi

# ---------------------------------------------------------------------------
# Preflight. Check every requested bundle BEFORE asking for sudo, so a bad
# build fails here rather than half-way through an installed set.
# ---------------------------------------------------------------------------
READY=()
FAILED=0
for p in "${WANTED[@]}"; do
    BUNDLE="$BUILD_DIR/$p/$p.plugin"
    BIN="$BUNDLE/Contents/MacOS/$p"
    RSRC="$BUNDLE/Contents/Resources/$p.rsrc"

    if [[ ! -d "$BUNDLE" ]]; then
        printf "  %-16s NOT BUILT (%s)\n" "$p" "$BUNDLE"; FAILED=1; continue
    fi
    if [[ ! -f "$BIN" ]]; then
        printf "  %-16s no executable in Contents/MacOS\n" "$p"; FAILED=1; continue
    fi
    if [[ ! -f "$BUNDLE/Contents/Info.plist" ]]; then
        printf "  %-16s no Info.plist\n" "$p"; FAILED=1; continue
    fi
    RSRC_SIZE=$(stat -f%z "$RSRC" 2>/dev/null || echo 0)
    if [[ "$RSRC_SIZE" -eq 0 ]]; then
        printf "  %-16s PiPL EMPTY or missing - AE would load it and show nothing\n" "$p"
        printf "  %-16s (Rez needs -useDF; see docs/MAC_BUILD.md SS5.2)\n" ""
        FAILED=1; continue
    fi

    printf "  %-16s ok  %-15s pipl=%s bytes\n" "$p" "$(lipo -archs "$BIN" 2>/dev/null | tr ' ' ',')" "$RSRC_SIZE"
    READY+=("$p")
done

echo
if [[ $FAILED -ne 0 ]]; then
    echo "Preflight failed - nothing installed. Build first:" >&2
    echo "    cmake --build $BUILD_DIR -j" >&2
    exit 1
fi

if [[ $DRY_RUN -eq 1 ]]; then
    echo "Dry run: ${#READY[@]} bundle(s) would be installed into $DEST"
    exit 0
fi

# ---------------------------------------------------------------------------
# Install. One sudo prompt up front rather than one per bundle.
# ---------------------------------------------------------------------------
echo "Installing ${#READY[@]} bundle(s) - sudo required for $DEST"
sudo -v || { echo "sudo declined - nothing installed." >&2; exit 1; }

for p in "${READY[@]}"; do
    BUNDLE="$BUILD_DIR/$p/$p.plugin"
    sudo rm -rf "$DEST/$p.plugin" && sudo cp -R "$BUNDLE" "$DEST/$p.plugin"
    if [[ $? -ne 0 ]]; then
        echo "  $p FAILED to copy" >&2; FAILED=1; continue
    fi

    # Verify the PiPL survived the copy - the data fork must come across, or
    # the installed bundle is the silent-failure case all over again.
    SIZE=$(stat -f%z "$DEST/$p.plugin/Contents/Resources/$p.rsrc" 2>/dev/null || echo 0)
    if [[ "$SIZE" -eq 0 ]]; then
        echo "  $p installed but its PiPL is empty at the destination" >&2; FAILED=1
    else
        printf "  %-16s installed (pipl=%s bytes)\n" "$p" "$SIZE"
    fi
done

echo
if [[ $FAILED -ne 0 ]]; then
    echo "Finished WITH ERRORS - see above." >&2
    exit 1
fi
echo "Done. Restart After Effects, then look under Effect > MM."
