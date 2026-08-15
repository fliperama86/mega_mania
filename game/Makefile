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

$(TARGET).elf: $(SHOBJS)
	$(SHCC) $(SHLDFLAGS) $^ -o $@ $(SHLIBS)

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

# assets.s pulls the converted stage/character data in with .incbin, so it has
# to be re-assembled whenever that data changes. Without this the ROM silently
# keeps an old copy, the same trap as md_start.bin above.
#
# The empty recipe matters: without it these .bin files would be treated as
# buildable targets in their own right instead of being left alone as source
# data, which has cost this project hours before.
RES := $(wildcard ../assets/ghz/*.bin) $(wildcard ../assets/sonic/*.bin)
$(RES): ;
md_src/assets.o: $(RES)
