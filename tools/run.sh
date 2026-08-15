#!/usr/bin/env bash
#
# Build a ROM and run it in ares.
#
# Usage: tools/run.sh [game|cdbench|blitbench|mania] [--no-build] [--no-disc] [--disc PATH]
#
# "mania" runs the original instead: the RSDKv5 decompilation in the references
# folder, which is the reference for what this is supposed to look and sound
# like. Nothing here builds it; it is already built.
#
# Every ROM here is launched as a "Mega 32X" cartridge, including the ones that
# drive the Mega CD: ares attaches the CD hardware itself when the ROM header
# declares a C in its device field, while "--system Mega CD 32X" would take the
# cartridge for a disc image and boot the BIOS CD player instead. See
# docs/hardware-budget.md, section 8.
#
# The disc is optional and is only music. Build one with:
#   python3 tools/make_disc.py <Data.rsdk> assets/disc MainMenu.ogg
# and this script picks it up from there without being told.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ares=${ARES:-$HOME/Projects/references/ares/build_macos/desktop-ui/Release/ares.app/Contents/MacOS/ares}
mania_dir=${MANIA_DIR:-$HOME/Projects/references/Sonic-Mania-Decompilation}

target=game
build=1
disc=${MEGAMANIA_DISC:-$root/assets/disc/disc.cue}

while [ $# -gt 0 ]; do
	case $1 in
	game | cdbench | blitbench | mania) target=$1 ;;
	--no-build) build=0 ;;
	--no-disc) disc= ;;
	--disc) disc=$2; shift ;;
	*) echo "usage: tools/run.sh [game|cdbench|blitbench|mania] [--no-build] [--no-disc] [--disc PATH]" >&2; exit 2 ;;
	esac
	shift
done

# The original, for comparison. None of the ares handling below applies to it.
#
# It is a .app bundle now, but we exec the binary inside rather than "open" the
# bundle: that keeps it in the foreground with its log on this terminal, and it
# still picks up the bundle's icon and menu bar name, because macOS resolves
# those by walking up from the executable path. Older build trees only have the
# bare executable, so fall back to that.
#
# Either way it no longer matters what directory we are in. InitUserDirectory()
# points the engine at ~/Library/Application Support/RSDKv5, which is where
# Data.rsdk, Settings.ini and the saves live.
if [ "$target" = mania ]; then
	app=${MANIA_APP:-$mania_dir/build/dependencies/RSDKv5/Sonic Mania.app}
	rsdk="$app/Contents/MacOS/Sonic Mania"
	data="$HOME/Library/Application Support/RSDKv5"

	if [ ! -x "$rsdk" ]; then
		rsdk=$mania_dir/build/dependencies/RSDKv5/RSDKv5U
		[ -x "$rsdk" ] || { echo "no Sonic Mania.app at $app (set MANIA_APP or MANIA_DIR to override)" >&2; exit 1; }
		echo "mania $rsdk (unbundled, rebuild for the .app)"
	else
		echo "mania $app"
	fi

	[ -f "$data/Data.rsdk" ] || { echo "no Data.rsdk in $data" >&2; exit 1; }
	exec "$rsdk"
fi

case $target in
game) rom=$root/game/megamania.32x ;;
cdbench) rom=$root/cdbench/cdbench.32x ;;
blitbench) rom=$root/blitbench/blitbench.32x ;;
esac

[ -x "$ares" ] || { echo "no ares at $ares (set ARES to override)" >&2; exit 1; }

# Old instances keep rendering the ROM they were launched with, so a screenshot
# of "the ares window" can be several builds stale. Insist on none before
# starting, rather than trusting that a previous run went away.
pkill -9 -f "MacOS/ares" 2>/dev/null || true
for _ in 1 2 3 4 5; do
	[ "$(pgrep -f "MacOS/ares" | wc -l | tr -d ' ')" = 0 ] && break
	sleep 0.2
done
if [ "$(pgrep -f "MacOS/ares" | wc -l | tr -d ' ')" != 0 ]; then
	echo "ares is still running and would not die; kill it by hand" >&2
	exit 1
fi

if [ "$build" = 1 ]; then
	make -C "$(dirname "$rom")" >/dev/null
fi
[ -f "$rom" ] || { echo "no ROM at $rom, build it first" >&2; exit 1; }

echo "rom  $rom"
if [ -n "$disc" ] && [ -f "$disc" ]; then
	echo "disc $disc"
	exec "$ares" --system "Mega 32X" --no-file-prompt "$rom" "$disc"
fi

if [ -n "$disc" ]; then
	echo "disc none ($disc does not exist, running without music)"
else
	echo "disc none"
fi
exec "$ares" --system "Mega 32X" --no-file-prompt "$rom"
