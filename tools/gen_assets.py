#!/usr/bin/env python3
"""Single source of truth for every converted asset that lives in cartridge
bank 1 (0x100000-0x1FFFFF).

Before this script existed, moving one asset out of the 68000's crowded
512 KB ROM window meant hand-syncing four numbers in four files: a linker
ORIGIN (sh_src/mars.ld), an AT() load address (the asset's own .s file), a
hardcoded 68000-side pointer (whatever md_src/*.c reads it) and a bank
index (md_src/bank.h). mars.ld and obj_tiles.s used to carry literal
comments saying "change one, change all three" -- the trio that made adding
a single new game object take over an hour. This script removes all of it:
add ONE line to the ASSETS list below, run `make` (game/Makefile's
assets_gen rule reruns this automatically whenever the list, this script or
any listed asset file changes), and every downstream address -- the SH2
linker regions, the SH2-side .incbin labels and the 68000-side fixed
pointers -- falls out of one packing pass, never hand-typed.

Why bank 1 specifically, and why every asset lands there (not just the ones
that do not fit the 68000's own window): once ALL data the 68000 reaches
through the banked window (0x900000-0x9FFFFF, md_src/bank.h) lives in bank
1, the 68000 selects bank 1 exactly once, at boot (main.c), and never
switches again -- zero bank switches means zero bank hazards, no
interrupt-safety problem, no "which bank is live right now" bug ever
possible. See md_src/bank.h's own comment for the hazards a *second* bank
switch would reopen.

Emits, every run:
  md_src/assets_gen.h        one ASSET_<NAME> pointer + ASSET_<NAME>_SIZE
                              byte count per entry, both computed -- the
                              68000-side half. Every pointer already carries
                              BANK1_WINDOW_BASE (0x900000): a consumer casts
                              nothing, it just uses ASSET_FOO directly, the
                              same convention main.c's old ghz_tiles_banked
                              hand-rolled for one asset.
  sh_src/assets_gen.s        one .section/.incbin/.global per entry -- the
                              SH2-side half. SH2s address the cartridge
                              linearly with no banked window at all, so they
                              always reach this data directly at its own
                              linked address, no bank_select() involved.
  sh_src/mars_gen_mem.ld     the `bank1` MEMORY region, INCLUDEd into
                              sh_src/mars.ld's MEMORY block.
  sh_src/mars_gen_sections.ld
                              one SECTIONS entry per asset placing it in
                              `bank1` at its computed address, INCLUDEd into
                              sh_src/mars.ld's SECTIONS block.

Two generated numbers stay in lockstep by construction because they come
from the same packing pass in this one process: an asset's 68000-side
pointer (md_src/assets_gen.h) and its SH2-side linked address
(sh_src/mars_gen_mem.ld + mars_gen_sections.ld) are never computed twice.

Usage: gen_assets.py (no arguments; run from anywhere -- every path below is
resolved relative to this script's own location, same convention every
other tools/convert_*.py script uses for its own inputs).
"""

import os

# ---------------------------------------------------------------------------
# THE MANIFEST. Add one asset to this project by adding one line here, then
# rerunning `make` (or this script directly to inspect the layout without a
# full build). Nothing else in this file needs to change for an ordinary
# new asset.
#
# Fields:
#   symbol   - base identifier for everything this asset generates:
#              ASSET_<SYMBOL upper> / ASSET_<SYMBOL upper>_SIZE in
#              assets_gen.h, section name .assets_gen_<symbol>, and (unless
#              sh_link_name overrides it) the SH2-side debug label
#              _<symbol>_sh.
#   path     - asset file, relative to the assets/ directory alongside
#              game/ (assets/ itself is gitignored and regenerated locally
#              by tools/convert_*.py -- see that file's own comment).
#   ctype    - the C pointee type of the generated 68000-side pointer
#              (uint8_t/int8_t/uint16_t/int16_t/uint32_t). Purely which type
#              a consumer gets for free; does not affect layout.
#   align    - byte alignment this asset's start address must satisfy in
#              bank 1, driven by how its consumer(s) read it. 2 unless a
#              consumer reads 32-bit longs across it (bg_blocks, read a
#              longword at a time by sh_src/bg.c's draw_strip()).
#   sh_link_name
#            - only set when another SH2-side .c/.s file links against this
#              asset BY NAME as an ordinary extern array (not through a
#              68000-side fixed pointer): sh_src/path.c's `extern const
#              uint16_t ghz_map_fgh[];` needs the linked symbol to be
#              exactly `_ghz_map_fgh`, so this overrides the cosmetic
#              default label for that one entry. Every other asset here is
#              reached from the SH2 side only indirectly (if at all), so the
#              default `_<symbol>_sh` debug label is fine.
ASSETS = [
    # --- formerly md_src/assets.s's .incbin lines -------------------------
    ("ghz_pal",              "ghz/pal.bin",              "uint16_t", 2, None),
    ("ghz_blocks",           "ghz/blocks.bin",           "uint16_t", 2, None),
    ("ghz_map",              "ghz/map_fg.bin",           "uint16_t", 2, None),
    # ghz_bgmap (map_bg.bin) has no reader anywhere in this codebase -- see
    # this script's own final report for the day this was discovered.
    # Relocated as-is, unread, rather than unilaterally dropped.
    ("ghz_bgmap",            "ghz/map_bg.bin",           "uint16_t", 2, None),
    ("ghz_collide_index",    "ghz/collide_index.bin",    "uint16_t", 2, None),
    ("ghz_collide_rows",     "ghz/collide_rows.bin",     "uint8_t",  2, None),
    ("sonic_pal",            "sonic/pal.bin",            "uint16_t", 2, None),
    ("sonic_tiles",          "sonic/tiles.bin",          "uint32_t", 2, None),
    ("bg_pal",               "ghzbg/bg_pal.bin",         "uint16_t", 2, None),
    ("bg_blocks",            "ghzbg/bg_blocks.bin",      "uint8_t",  4, None),
    ("bg_map",               "ghzbg/bg_map.bin",         "uint16_t", 2, None),
    ("bg_lines",             "ghzbg/bg_lines.bin",       "uint16_t", 2, None),
    # rings.bin: big-endian u16 count then the xy table immediately after --
    # one file, two symbols (md_src/rings.c derives the second at a fixed
    # +2 byte offset from this one pointer, same as the old assets.s did at
    # assembly level with `ghz_ring_xy = ghz_ring_count + 2`).
    ("ghz_rings",            "ghz/rings.bin",            "uint16_t", 2, None),
    ("ring_tiles",           "ring/tiles.bin",           "uint32_t", 2, None),
    ("sonic_hitbox",         "sonic/hitbox.bin",         "int8_t",   2, None),

    # --- formerly sh_src/sonic_rot.s ---------------------------------------
    ("sonic_rot_tiles",      "sonic/rot_tiles.bin",      "uint32_t", 2, None),

    # --- formerly sh_src/obj_tiles.s ---------------------------------------
    # springs.bin: same one-file-two-symbols shape as rings.bin above.
    ("ghz_springs",          "ghz/springs.bin",          "uint16_t", 2, None),
    ("spring_tiles",         "spring/tiles.bin",         "uint32_t", 2, None),
    # spring_stream_tiles.bin has no reader either (springs.c only ever
    # defined spring_tiles_md; a spring_stream_tiles_md was never added --
    # see springs.c's own "NEW DEVIATION" comment: springs draw their
    # resident pose only, no bounce animation). Relocated as-is, unread,
    # same call as ghz_bgmap above.
    ("spring_stream_tiles",  "spring/stream_tiles.bin",  "uint32_t", 2, None),
    ("signpost_tiles",       "signpost/tiles.bin",       "uint32_t", 2, None),
    ("signpost_stream_tiles","signpost/stream_tiles.bin","uint32_t", 2, None),

    # --- GHZ Act 1 object roster (tools/convert_objects.py) ----------------
    # One x-sorted scene table per class (ghz_<name>s, same "ghz_rings"/
    # "ghz_springs" convention above) and one tiles.bin per ArtRecipe-driven
    # class under assets/obj/<class>/ -- every line below printed verbatim
    # by that script's own --print-manifest-lines flag (see its main()'s own
    # comment for why this used to be 1-3 hand-typed lines per object).
    # Classes with no tiles line here (CollapsingPlatform, BreakableWall,
    # InvisibleBlock, SpinBooster, CorkscrewPath) draw no sprite art at all
    # in retail play -- see each one's own SceneRecipe comment in
    # convert_objects.py for why.
    ("ghz_motobugs",             "ghz/motobugs.bin",             "uint16_t", 2, None),
    ("motobug_tiles",            "obj/motobug/tiles.bin",        "uint32_t", 2, None),
    ("ghz_spikes",                "ghz/spikes.bin",               "uint16_t", 2, None),
    ("spikes_tiles",              "obj/spikes/tiles.bin",         "uint32_t", 2, None),
    ("ghz_itemboxes",             "ghz/itemboxes.bin",            "uint16_t", 2, None),
    ("itembox_tiles",             "obj/itembox/tiles.bin",        "uint32_t", 2, None),
    ("ghz_spikelogs",             "ghz/spikelogs.bin",            "uint16_t", 2, None),
    ("spikelog_tiles",            "obj/spikelog/tiles.bin",       "uint32_t", 2, None),
    ("ghz_newtrons",              "ghz/newtrons.bin",             "uint16_t", 2, None),
    ("newtron_tiles",             "obj/newtron/tiles.bin",        "uint32_t", 2, None),
    ("ghz_buzzbombers",           "ghz/buzzbombers.bin",          "uint16_t", 2, None),
    ("buzzbomber_tiles",          "obj/buzzbomber/tiles.bin",     "uint32_t", 2, None),
    ("ghz_choppers",              "ghz/choppers.bin",             "uint16_t", 2, None),
    ("chopper_tiles",             "obj/chopper/tiles.bin",        "uint32_t", 2, None),
    ("ghz_crabmeats",             "ghz/crabmeats.bin",            "uint16_t", 2, None),
    ("crabmeat_tiles",            "obj/crabmeat/tiles.bin",       "uint32_t", 2, None),
    ("ghz_batbrains",             "ghz/batbrains.bin",            "uint16_t", 2, None),
    ("batbrain_tiles",            "obj/batbrain/tiles.bin",       "uint32_t", 2, None),
    ("ghz_platforms",             "ghz/platforms.bin",            "uint16_t", 2, None),
    ("platform_tiles",            "obj/platform/tiles.bin",       "uint32_t", 2, None),
    ("ghz_collapsingplatforms",   "ghz/collapsingplatforms.bin",  "uint16_t", 2, None),
    ("ghz_bridges",               "ghz/bridges.bin",              "uint16_t", 2, None),
    ("bridge_tiles",              "obj/bridge/tiles.bin",         "uint32_t", 2, None),
    ("ghz_breakablewalls",        "ghz/breakablewalls.bin",       "uint16_t", 2, None),
    ("ghz_decorations",           "ghz/decorations.bin",          "uint16_t", 2, None),
    ("decoration_tiles",          "obj/decoration/tiles.bin",     "uint32_t", 2, None),
    ("ghz_invisibleblocks",       "ghz/invisibleblocks.bin",      "uint16_t", 2, None),
    ("ghz_spinboosters",          "ghz/spinboosters.bin",         "uint16_t", 2, None),
    ("ghz_corkscrewpaths",        "ghz/corkscrewpaths.bin",       "uint16_t", 2, None),

    # --- formerly sh_src/map_fgh.s ------------------------------------------
    # sh_src/path.c links straight against this one by name (ghz_map_fgh[]),
    # the same way it already links against ghz_collide_b_index/rows -- see
    # sh_src/collide_b.s's own comment for why plane 1's data took that
    # route from the start while plane 0's (ghz_map/ghz_collide_index/
    # ghz_collide_rows above) goes through the descriptor table instead.
    ("ghz_map_fgh",          "ghz/map_fgh.bin",          "uint16_t", 2, "ghz_map_fgh"),

    # --- formerly sh_src/ghz_tiles.s (bank switching's original proof) ----
    ("ghz_tiles",            "ghz/tiles.bin",            "uint32_t", 2, None),
]

# ---------------------------------------------------------------------------
# Layout constants. Bank 1 is cartridge offset 0x100000-0x1FFFFF; the 68000
# reaches it at 0x900000-0x9FFFFF once bank_select(1) has run (md_src/
# bank.h); the SH2 reaches it directly at 0x02100000 and up, the same
# ORIGIN convention every fixed-address region in mars.ld already used
# (sonicrot/objtiles/maphigh/ghztiles, all folded into this one region now).
BANK1_CART_OFFSET = 0x100000
BANK1_LENGTH      = 0x100000   # 1 MB: the whole of cartridge bank 1
BANK1_WINDOW_BASE = 0x900000   # 68000 banked-window address of bank1+0
BANK1_SH_ORIGIN   = 0x02100000 # SH2 linked address of bank1+0


def compute_layout(assets):
    offset = 0
    layout = []
    for symbol, relpath, ctype, align, sh_link_name in assets:
        if offset % align:
            offset += align - (offset % align)
        path = os.path.join(ASSET_DIR, relpath)
        size = os.path.getsize(path)
        layout.append(dict(symbol=symbol, relpath=relpath, ctype=ctype,
                            align=align, sh_link_name=sh_link_name,
                            offset=offset, size=size))
        offset += size
    total = offset
    if total > BANK1_LENGTH:
        raise SystemExit(
            "gen_assets.py: bank 1 overflow -- %u bytes packed, only %u "
            "available (0x100000-0x1FFFFF). This is a real design problem, "
            "not a bug in this script: stop and report the numbers instead "
            "of trying to fit a second bank." % (total, BANK1_LENGTH))
    return layout, total


def emit_header(layout, total):
    lines = []
    lines.append("#ifndef ASSETS_GEN_H")
    lines.append("#define ASSETS_GEN_H")
    lines.append("")
    lines.append("/* GENERATED by tools/gen_assets.py from that script's own ASSETS list --")
    lines.append(" * do not edit by hand. To add or resize an asset, edit that list and")
    lines.append(" * rerun `make` (game/Makefile's assets_gen rule reruns this script")
    lines.append(" * automatically whenever it, the manifest or any listed asset file")
    lines.append(" * changes -- see that rule's own comment).")
    lines.append(" *")
    lines.append(" * Every asset below lives in cartridge bank 1 (0x100000-0x1FFFFF),")
    lines.append(" * reached by the 68000 through the banked window at 0x900000-0x9FFFFF")
    lines.append(" * (md_src/bank.h) once bank_select(1) has run -- main.c does that once,")
    lines.append(" * at boot, and never switches away again, so every pointer below stays")
    lines.append(" * valid for the program's entire run with no per-access bank switch.")
    lines.append(" * Total packed size: %u of %u bytes (%u free)." % (
        total, BANK1_LENGTH, BANK1_LENGTH - total))
    lines.append(" */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define BANK1_WINDOW_BASE 0x%06Xu" % BANK1_WINDOW_BASE)
    lines.append("")
    for a in layout:
        name = a["symbol"].upper()
        addr = BANK1_WINDOW_BASE + a["offset"]
        lines.append("#define ASSET_%-24s ((const %s *)(BANK1_WINDOW_BASE + 0x%06Xu))"
                     % (name, a["ctype"], a["offset"]))
        lines.append("#define ASSET_%-24s %uu" % (name + "_SIZE", a["size"]))
    lines.append("")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def emit_sh_asm(layout):
    lines = []
    lines.append("! GENERATED by tools/gen_assets.py -- do not edit by hand. See that")
    lines.append("! script's own header comment for the mechanism this file is half of:")
    lines.append("! one manifest, two generated outputs (this file's SH2-side .incbin")
    lines.append("! labels, md_src/assets_gen.h's 68000-side fixed pointers), so an")
    lines.append("! asset's address can never go stale in one without the other.")
    lines.append("!")
    lines.append("! Every section here is placed by sh_src/mars_gen_sections.ld at a fixed")
    lines.append("! address in the `bank1` region (sh_src/mars_gen_mem.ld): the SH2 sees it")
    lines.append("! directly at that ORIGIN (SH2s address the cartridge linearly, no banked")
    lines.append("! window, no bank_select() involved), and the 68000 sees the identical")
    lines.append("! bytes at BANK1_WINDOW_BASE plus the same offset, once bank 1 is")
    lines.append("! selected (md_src/bank.h, main.c's boot sequence).")
    lines.append("")
    for a in layout:
        symbol = a["symbol"]
        label = "_%s" % (a["sh_link_name"] if a["sh_link_name"] else "%s_sh" % symbol)
        section = ".assets_gen_%s" % symbol
        lines.append("\t.section %s,\"a\",@progbits" % section)
        lines.append("\t.global\t%s" % label)
        lines.append("%s:" % label)
        lines.append("\t.incbin\t\"../assets/%s\"" % a["relpath"])
        lines.append("")
    return "\n".join(lines)


def emit_ld_mem():
    return ("/* GENERATED by tools/gen_assets.py -- do not edit by hand. INCLUDEd from\n"
            " * sh_src/mars.ld's MEMORY block. */\n"
            "bank1 (r) : ORIGIN = 0x%08X, LENGTH = 0x%08X\n"
            % (BANK1_SH_ORIGIN, BANK1_LENGTH))


def emit_ld_sections(layout):
    lines = []
    lines.append("/* GENERATED by tools/gen_assets.py -- do not edit by hand. INCLUDEd from")
    lines.append(" * sh_src/mars.ld's SECTIONS block, one output section per manifest entry,")
    lines.append(" * each KEEPing its .assets_gen_<symbol> input section (sh_src/")
    lines.append(" * assets_gen.s) so garbage collection can never drop unreferenced asset")
    lines.append(" * data the way it could an ordinary unreferenced symbol. AT() keeps each")
    lines.append(" * section's load address contiguous with bank 1's own cartridge offset")
    lines.append(" * (BANK1_CART_OFFSET + the same packed offset assets_gen.h's pointers")
    lines.append(" * use), the same convention every fixed-address region in this project")
    lines.append(" * has always used (see mars.ld's own .text stanza). */")
    lines.append("")
    for a in layout:
        symbol = a["symbol"]
        section = ".assets_gen_%s" % symbol
        vma = BANK1_SH_ORIGIN + a["offset"]
        lma = BANK1_CART_OFFSET + a["offset"]
        lines.append("%s 0x%08X : AT(0x%08X) {" % (section, vma, lma))
        lines.append("\tKEEP(*(%s))" % section)
        lines.append("} > bank1")
        lines.append("")
    return "\n".join(lines)


def main():
    global ASSET_DIR
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(tools_dir)
    asset_dir = os.path.join(repo_root, "assets")
    game_dir = os.path.join(repo_root, "game")
    ASSET_DIR = asset_dir

    layout, total = compute_layout(ASSETS)

    header_path = os.path.join(game_dir, "md_src", "assets_gen.h")
    asm_path = os.path.join(game_dir, "sh_src", "assets_gen.s")
    ld_mem_path = os.path.join(game_dir, "sh_src", "mars_gen_mem.ld")
    ld_sections_path = os.path.join(game_dir, "sh_src", "mars_gen_sections.ld")

    with open(header_path, "w") as fp:
        fp.write(emit_header(layout, total))
    with open(asm_path, "w") as fp:
        fp.write(emit_sh_asm(layout))
    with open(ld_mem_path, "w") as fp:
        fp.write(emit_ld_mem())
    with open(ld_sections_path, "w") as fp:
        fp.write(emit_ld_sections(layout))

    print("gen_assets.py: %u assets packed, %u of %u bytes used in bank 1 (%u free)"
          % (len(layout), total, BANK1_LENGTH, BANK1_LENGTH - total))


if __name__ == "__main__":
    main()
