# Sample Makefile for Marsdev (32X version)
# For 32X both m68k and SH-2 code has to be built
# The usual variables are split into "MD" and "SH" versions

# Default paths, can be overridden by setting MARSDEV before calling make
MARSDEV ?= ${HOME}/mars
TOOLSBIN = $(MARSDEV)/bin
MDBIN    = $(MARSDEV)/m68k-elf/bin
SHBIN    = $(MARSDEV)/sh-elf/bin

TARGET  ?= megamania
MDTARGET = md_start

# m68k GCC and Binutils
MDCC   = $(MDBIN)/m68k-elf-gcc
MDCXX  = $(MDBIN)/m68k-elf-g++
MDAS   = $(MDBIN)/m68k-elf-as
MDLD   = $(MDBIN)/m68k-elf-ld
MDNM   = $(MDBIN)/m68k-elf-nm
MDOBJC = $(MDBIN)/m68k-elf-objcopy
# sh2 GCC and Binutils
SHCC   = $(SHBIN)/sh-elf-gcc
SHCXX  = $(SHBIN)/sh-elf-g++
SHAS   = $(SHBIN)/sh-elf-as
SHLD   = $(SHBIN)/sh-elf-ld
SHNM   = $(SHBIN)/sh-elf-nm
SHOBJC = $(SHBIN)/sh-elf-objcopy

# Some files needed are in a versioned directory
MDCC_VER := $(shell $(MDCC) -dumpversion)
SHCC_VER := $(shell $(SHCC) -dumpversion)

# Need the LTO plugin so NM can dump our symbol table
MDPLUGIN = $(MARSDEV)/m68k-elf/libexec/gcc/m68k-elf/$(MDCC_VER)/liblto_plugin.so
SHPLUGIN = $(MARSDEV)/sh-elf/libexec/gcc/sh-elf/$(SHCC_VER)/liblto_plugin.so

MDINCS   = -Imd_src
MDINCS  += -I$(MARSDEV)/m68k-elf/lib/gcc/m68k-elf/$(MDCC_VER)/include
SHINCS   = -Ish_src
SHINCS  += -I$(MARSDEV)/sh-elf/lib/gcc/sh-elf/$(SHCC_VER)/include

MDLIBS   = -L$(MARSDEV)/m68k-elf/lib/gcc/m68k-elf/$(MDCC_VER) -lgcc
SHLIBS   = -L$(MARSDEV)/sh-elf/lib/gcc/sh-elf/$(SHCC_VER) -lgcc

# 68000 side stays off -mshort: it is blitbench's choice, not a 32X
# requirement (docs/hardware-budget.md, section 3), and matching the game's
# own prior flags avoids auditing every int in the ported physics.
MDCCFLAGS  = -m68000 -Wall -Wextra -std=c99 -ffreestanding
MDCXXFLAGS = -m68000 -Wall -Wextra -std=c++17 -ffreestanding
SHCCFLAGS  = -m2 -mb -Wall -Wextra -pedantic -std=c99 -ffreestanding
SHCXXFLAGS = -m2 -mb -Wall -Wextra -pedantic -std=c++17 -ffreestanding

# Assembler flags
MDASFLAGS  = -m68000 --register-prefix-optional
SHASFLAGS  = -Ish_src --small

# Linker flags
MDLDFLAGS  = -T md_src/md.ld -nostdlib
SHLDFLAGS  = -T sh_src/mars.ld -nostdlib

# Extra options set by debug or release target
MDEXTRA =
SHEXTRA =

# Generate m68k object target list
MDSS    = $(wildcard md_src/*.s)
MDCS    = $(wildcard md_src/*.c)
MDCPPS  = $(wildcard md_src/*.cpp)
MDOBJS  = $(MDSS:.s=.o)
MDOBJS += $(MDCS:.c=.o)
MDOBJS += $(MDCPPS:.cpp=.o)

# Generate sh object target list
SHSS    = $(wildcard sh_src/*.s)
SHCS    = $(wildcard sh_src/*.c)
SHCPPS  = $(wildcard sh_src/*.cpp)
SHOBJS  = $(SHSS:.s=.o)
SHOBJS += $(SHCS:.c=.o)
SHOBJS += $(SHCPPS:.cpp=.o)
# sh_src/assets_gen.s (tools/gen_assets.py's output) does not exist until
# that generator has run at least once, so SHSS's wildcard() above can miss
# it on a clean checkout -- wildcard() expands when Make parses this file,
# before assets_gen.stamp's rule has had a chance to create it. Added
# unconditionally rather than trusting the wildcard to see a file that might
# not exist yet; harmless if the wildcard already found it too, since $^
# de-duplicates prerequisite lists.
SHOBJS += sh_src/assets_gen.o

.PHONY: all release debug

all: release

release: MDEXTRA  = -O3 -fno-web -fno-gcse -fno-unit-at-a-time -fomit-frame-pointer -fshort-enums -flto -fuse-linker-plugin
release: SHEXTRA  = -Ofast -fomit-frame-pointer -flto -fuse-linker-plugin
release: $(MDTARGET).bin $(MDTARGET).lst $(TARGET).32x $(TARGET).lst

# Gens-KMod, BlastEm and UMDK support GDB tracing, enabled by this target
debug: MDEXTRA = -g -Og -DDEBUG -DKDEBUG
debug: SHEXTRA = -g -Og -DDEBUG -DKDEBUG
debug: $(MDTARGET).bin $(MDTARGET).lst $(TARGET).32x $(TARGET).lst

# Symbol listings for both CPUs
$(MDTARGET).lst: $(MDTARGET).elf
	$(MDNM) --plugin=$(MDPLUGIN) -n $< > $@

$(TARGET).lst: $(TARGET).elf
	$(SHNM) --plugin=$(SHPLUGIN) -n $< > $@

# Mega CD sub-CPU program: minimal port of cd_src/crt.s from
# ~/Projects/references/SegaCDMode1PCM (MIT, Mikael Kalms), linked flat at
# 0x6000 with cd_src/cd-mode1.ld, then flattened to a raw binary that
# md_src/sub_cd.s pulls into the 68000 ROM with .incbin. Modelled exactly on
# cdbench/Makefile.
CDOBJS = cd_src/crt.o

cd_src/cd.bin: cd_src/cd.elf
	@echo "Stripping ELF header from Sub-CPU program"
	@$(MDOBJC) -O binary $< $@

cd_src/cd.elf: $(CDOBJS)
	$(MDLD) -T cd_src/cd-mode1.ld -o $@ $^

cd_src/%.o: cd_src/%.s
	@echo "MDAS $<"
	@$(MDCC) -x assembler-with-cpp -Icd_src -m68000 -Wa,--register-prefix-optional -c $< -o $@

# md_src/sub_cd.s pulls cd.bin in with .incbin, so it has to be re-assembled
# whenever that binary changes. Without this the ROM silently keeps an old
# copy of the sub-CPU program (see docs/hardware-budget.md, section 6).
md_src/sub_cd.o: cd_src/cd.bin

# m68k stuff

$(MDTARGET).bin: $(MDTARGET).elf
	@echo "Stripping ELF header from M68K program"
	@$(MDOBJC) -O binary $< $@

$(MDTARGET).elf: $(MDOBJS)
	$(MDCC) $(MDLDFLAGS) $^ -o $@ $(MDLIBS)

md_src/%.o: md_src/%.s
	@echo "MDAS $<"
	@$(MDAS) $(MDASFLAGS) $< -o $@

md_src/%.o: md_src/%.c
	@echo "MDCC $<"
	@$(MDCC) $(MDCCFLAGS) $(MDEXTRA) $(MDINCS) -c $< -o $@

md_src/%.o: md_src/%.cpp
	@echo "MDCXX $<"
	@$(MDCXX) $(MDCXXFLAGS) $(MDEXTRA) $(MDINCS) -c $< -o $@

# sh2 stuff

$(TARGET).32x: $(TARGET).elf
	@echo "Stripping ELF header from SH-2 program"
	@$(SHOBJC) -O binary $< temp.32x
	@dd if=temp.32x of=$@ bs=8192 conv=sync
	@rm -f temp.32x

# sh_src/mars.ld INCLUDEs the two generated linker fragments below at link
# time; ld reads them fresh off disk (no Make involvement there), so this
# project's Make graph has to know about that dependency on its own or a
# stale fragment would silently keep an old bank-1 layout. $(filter %.o,$^)
# rather than a bare $^ for two reasons: it drops the two .ld fragments back
# out (they belong on ld's -T script, not its object list) and, since $^
# already de-duplicates, it also protects against sh_src/assets_gen.o
# appearing twice in $(SHOBJS) (the wildcard-found copy and this file's own
# unconditional `SHOBJS += sh_src/assets_gen.o` above) turning into two
# copies of the same global symbols on the actual link command line.
$(TARGET).elf: $(SHOBJS) sh_src/mars_gen_mem.ld sh_src/mars_gen_sections.ld
	$(SHCC) $(SHLDFLAGS) $(filter %.o,$^) -o $@ $(SHLIBS)

# mars_start.s pulls the assembled 68000 program in with .incbin, so it has to
# be re-assembled whenever that binary changes. Without this the ROM silently
# keeps an old copy of the MD side.
sh_src/mars_start.o: $(MDTARGET).bin

sh_src/%.o: sh_src/%.s
	@echo "SHAS $<"
	@$(SHAS) $(SHASFLAGS) $< -o $@

sh_src/%.o: sh_src/%.c
	@echo "SHCC $<"
	@$(SHCC) $(SHCCFLAGS) $(SHEXTRA) $(SHINCS) -c $< -o $@

sh_src/%.o: sh_src/%.cpp
	@echo "SHCXX $<"
	@$(SHCXX) $(SHCXXFLAGS) $(SHEXTRA) $(SHINCS) -c $< -o $@

.PHONY: clean

clean:
	rm -f $(MDOBJS) $(SHOBJS)
	rm -f $(MDTARGET).bin $(MDTARGET).elf $(MDTARGET).lst
	rm -f $(CDOBJS) cd_src/cd.bin cd_src/cd.elf
	rm -f $(TARGET).32x $(TARGET).elf $(TARGET).lst
	rm -f assets_gen.stamp md_src/assets_gen.h sh_src/assets_gen.s \
	      sh_src/mars_gen_mem.ld sh_src/mars_gen_sections.ld

# collide_b_index.bin/collide_b_rows.bin (RSDK collision path B) are linked
# into the SH2 program directly (see sh_src/collide_b.s), the one converted
# asset that stays there instead of going through tools/gen_assets.py's
# manifest below: nothing on the 68000 side ever reaches it (that file's own
# comment), so it has no reason to spend any of cartridge bank 1 or the
# 68000's fixed window either -- it just rides the SH2 program's own
# `rom` region (sh_src/mars.ld) like ordinary linked .rodata. The reassembly
# dependency belongs on that object file since it isn't pulled in by any
# generated file.
sh_src/collide_b.o: ../assets/ghz/collide_b_index.bin ../assets/ghz/collide_b_rows.bin

# tools/gen_assets.py is the single source of truth for every OTHER
# converted asset: one manifest (that script's own ASSETS list), and every
# downstream address -- the SH2 linker regions (sh_src/mars_gen_mem.ld,
# sh_src/mars_gen_sections.ld), the SH2-side .incbin file (sh_src/
# assets_gen.s) and the 68000-side fixed-pointer header (md_src/
# assets_gen.h) -- computed from it, never hand-typed. Adding an asset is
# one line in that script's ASSETS list; nothing here needs to change.
#
# It emits all four files on every run, so GNU Make's usual "one recipe, one
# target" shape does not fit; GNU Make 3.81 (this project's baseline) has no
# grouped-target syntax (`&:`) to declare that cleanly, so this uses the
# standard workaround instead: one stamp file the script's recipe touches
# last (after writing all four real files, so the stamp's mtime is always
# the newest of the five), and one empty rule per real file naming the stamp
# as its prerequisite. Make re-stats a prerequisite's mtime AFTER running
# the rule that produces it, not before, so this correctly lets each real
# file's OWN mtime (set by the script, not by `touch`) drive whatever
# compiles or links against it -- the empty recipe never re-invokes the
# script itself, only the stamp rule does that.
#
# The empty recipe on $(ASSET_BINS) matters too, same reasoning as this
# comment used to carry for assets.s's own $(RES): without it these .bin
# files would be treated as buildable targets in their own right instead of
# being left alone as source data, which has cost this project hours before.
ASSET_BINS := $(wildcard ../assets/ghz/*.bin) $(wildcard ../assets/sonic/*.bin) \
              $(wildcard ../assets/ghzbg/*.bin) $(wildcard ../assets/ring/*.bin) \
              $(wildcard ../assets/spring/*.bin) $(wildcard ../assets/signpost/*.bin) \
              $(wildcard ../assets/obj/*/*.bin)
$(ASSET_BINS): ;

assets_gen.stamp: ../tools/gen_assets.py $(ASSET_BINS)
	@echo "GEN assets_gen (tools/gen_assets.py)"
	@python3 ../tools/gen_assets.py
	@touch assets_gen.stamp

md_src/assets_gen.h sh_src/assets_gen.s sh_src/mars_gen_mem.ld sh_src/mars_gen_sections.ld: assets_gen.stamp ;

# Every 68000 .c file that reads a bank-1 asset includes md_src/assets_gen.h;
# this project does not track header dependencies automatically (no -MMD),
# so each has to be listed explicitly, same discipline as every other
# incbin/regeneration dependency in this file.
md_src/main.o md_src/descriptor.o md_src/sonic.o md_src/springs.o md_src/signpost.o md_src/rings.o: md_src/assets_gen.h
# BADNIKS batch: motobug.c/crabmeat.c/buzzbomber.c/chopper.c/newtron.c/
# batbrain.c/badnik_base.c all read their own class's bank-1 scene table
# and tile sheet through the same generated header -- same "no -MMD, list
# it by hand" discipline as every entry above.
md_src/motobug.o md_src/crabmeat.o md_src/buzzbomber.o md_src/chopper.o md_src/newtron.o md_src/batbrain.o md_src/badnik_base.o: md_src/assets_gen.h

# springs.c/signpost.c's own sprite-building loops compile much larger under
# release's default -O3 (aggressive unrolling over their piece loops) than
# their logic actually needs; -Os here is what let the 68000 program fit
# back under its fixed 512 KB window once this feature's own tile art (moved
# to the SH2 side, originally sh_src/obj_tiles.s, now generated bank-1 data
# via tools/gen_assets.py) still left the code itself too big to fit
# alongside everything else already linked there. Kept even though every
# asset has since moved out of this program's window (so the ROOT cause of
# the old budget pressure is gone): not worth relitigating a working -Os
# choice as part of this layout refactor.
md_src/springs.o md_src/signpost.o: MDEXTRA := -Os -flto -fuse-linker-plugin
