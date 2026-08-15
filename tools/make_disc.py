#!/usr/bin/env python3
"""Build a Mega CD disc image (.bin/.cue) whose audio tracks are the game's
music, so a Mode 1 cartridge can play CD audio off the disc.

The pack has no name table (see rsdk.py), so there is no way to list its
contents directly. --list instead tries every music filename the RSDKv5
decompilation's source hardcodes as a string literal (Music.c's
Music_StageLoad, TitleSetup.c's intro stingers) against Data/Music/<name> and
reports which exist. Per-stage music is chosen by a scene object property at
runtime, not a source literal, so it cannot be found this way -- pass its
Data/Music/... path directly to extract it anyway.

Each track is decoded with ffmpeg to Red Book PCM: 44100 Hz, 16-bit signed
little endian, stereo. Sectors are 2352 bytes (588 sample frames), so a
track's PCM is padded with silence to a whole number of sectors. The first
audio track in the image carries the standard 2 second (150 sector) pregap,
written as real silence at the start of its .bin with INDEX 00/01 in the cue;
later tracks start at INDEX 01 00:00:00 in their own file. With --data-track,
that first-audio-track pregap also doubles as the gap Red Book wants between
a data track and the audio that follows it.

Usage: make_disc.py <Data.rsdk> --list
       make_disc.py <Data.rsdk> <outdir> <track.ogg> [track.ogg ...] [--data-track FILE]
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from rsdk import Pack

USAGE = ("usage: make_disc.py <Data.rsdk> --list\n"
         "       make_disc.py <Data.rsdk> <outdir> <track.ogg> [track.ogg ...] "
         "[--data-track FILE]")

SECTOR = 2352
PREGAP_SECTORS = 150
RATE = 44100
CHANNELS = 2
FRAME_BYTES = CHANNELS * 2                    # 16-bit signed samples
SECTOR_FRAMES = SECTOR // FRAME_BYTES          # 588

# Every ".ogg" string literal PlayStream() is handed in the decompiled source
# (dependencies/RSDKv5/.../Audio.cpp prefixes it with "Data/Music/"). These
# are the fixed jingles and stingers; a stage's own music is not among them.
KNOWN_TRACKS = (
    "1up.ogg", "ActClear.ogg", "BlueSpheresSPD.ogg", "BossEggman1.ogg",
    "BossEggman2.ogg", "BossHBH.ogg", "BossMini.ogg", "BossPuyo.ogg",
    "BuddyBeat.ogg", "Drowning.ogg", "GameOver.ogg", "HBHMischief.ogg",
    "IntroHP.ogg", "IntroTee.ogg", "Invincible.ogg", "RubyPresence.ogg",
    "ShiversawExplosion.ogg", "Sneakers.ogg", "Super.ogg",
)


def msf(sectors):
    """CD absolute time: minutes:seconds:frames, 75 sector-frames per second."""
    m, rem = divmod(sectors, 75 * 60)
    s, f = divmod(rem, 75)
    return f"{m:02d}:{s:02d}:{f:02d}"


def pad_to_sectors(data):
    rem = len(data) % SECTOR
    return data + b"\x00" * (SECTOR - rem) if rem else data


def decode_ogg(ogg_bytes, label):
    """ffmpeg needs a real file to sniff the container, so spool to a temp
    .ogg and read raw PCM back off its stdout."""
    with tempfile.NamedTemporaryFile(suffix=".ogg") as tmp:
        tmp.write(ogg_bytes)
        tmp.flush()
        proc = subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", tmp.name,
             "-f", "s16le", "-ar", str(RATE), "-ac", str(CHANNELS), "-"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0 or not proc.stdout:
        raise SystemExit(f"make_disc.py: ffmpeg failed decoding {label}: "
                          f"{proc.stderr.decode(errors='replace').strip()}")
    return proc.stdout


def list_music(pack):
    found = 0
    for name in KNOWN_TRACKS:
        entry = pack.lookup(f"Data/Music/{name}")
        if entry is None:
            print(f"{'-':>10}  {name}   NOT FOUND")
        else:
            offset, size, encrypted = entry
            print(f"{size:>10,}  {name}{'   [enc]' if encrypted else ''}")
            found += 1
    print(f"\n{found} of {len(KNOWN_TRACKS)} known names present in the pack.")


def build_disc(pack, outdir, track_names, data_track):
    os.makedirs(outdir, exist_ok=True)
    records = []
    num = 1

    if data_track:
        if not os.path.isfile(data_track):
            raise SystemExit(f"make_disc.py: data track {data_track} not found")
        size = os.path.getsize(data_track)
        if size % SECTOR:
            print(f"warning: {data_track} is {size:,} bytes, not a whole "
                  f"number of {SECTOR}-byte sectors")
        fname = f"track{num:02d}.bin"
        shutil.copyfile(data_track, os.path.join(outdir, fname))
        sectors = -(-size // SECTOR)                       # ceil
        records.append(dict(num=num, kind="MODE1/2352", file=fname, pregap=0,
                             sectors=sectors, bytes=size,
                             label=os.path.basename(data_track)))
        num += 1

    if not track_names:
        raise SystemExit("make_disc.py: at least one music track is required")

    first_audio = True
    for name in track_names:
        path = name if name.startswith("Data/") else f"Data/Music/{name}"
        raw = pack.read(path)
        if raw is None:
            raise SystemExit(f"make_disc.py: {path} not found in pack")
        pcm = decode_ogg(raw, name)
        pregap = PREGAP_SECTORS if first_audio else 0
        data = pad_to_sectors(b"\x00" * (pregap * SECTOR) + pcm)
        fname = f"track{num:02d}.bin"
        with open(os.path.join(outdir, fname), "wb") as f:
            f.write(data)
        records.append(dict(num=num, kind="AUDIO", file=fname, pregap=pregap,
                             sectors=len(data) // SECTOR, bytes=len(data),
                             label=name))
        first_audio = False
        num += 1

    cue_path = os.path.join(outdir, "disc.cue")
    with open(cue_path, "w") as f:
        for r in records:
            f.write(f'FILE "{r["file"]}" BINARY\n')
            f.write(f'  TRACK {r["num"]:02d} {r["kind"]}\n')
            if r["pregap"]:
                f.write("    INDEX 00 00:00:00\n")
                f.write(f'    INDEX 01 {msf(r["pregap"])}\n')
            else:
                f.write("    INDEX 01 00:00:00\n")

    print(f"disc image -> {outdir}")
    print(f"  cue                 {cue_path}")
    running = 0
    for r in records:
        start = running + r["pregap"]
        length = r["sectors"] - r["pregap"]
        print(f"  track {r['num']:02d} {r['kind']:<11} start {msf(start)}  "
              f"length {msf(length)}  {r['bytes']:>10,} bytes  "
              f"{r['file']}  ({r['label']})")
        running += r["sectors"]
    print(f"  total               {msf(running)}  {running:,} sectors  "
          f"{running * SECTOR:,} bytes")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(USAGE)
    pack = Pack(sys.argv[1])

    if len(sys.argv) > 2 and sys.argv[2] == "--list":
        list_music(pack)
        return

    if len(sys.argv) < 4:
        raise SystemExit(USAGE)
    outdir = sys.argv[2]

    data_track = None
    tracks = []
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] == "--data-track":
            if i + 1 >= len(args):
                raise SystemExit(USAGE)
            data_track = args[i + 1]
            i += 2
        else:
            tracks.append(args[i])
            i += 1

    if not shutil.which("ffmpeg"):
        raise SystemExit("make_disc.py: ffmpeg not found on PATH; install it "
                          "to decode music to PCM (no other decoder is used)")

    build_disc(pack, outdir, tracks, data_track)


if __name__ == "__main__":
    main()
