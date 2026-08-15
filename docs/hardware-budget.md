# Sega 32X + Mega CD: Measured Hardware Budget and Engine Shape

What the hardware can actually do, measured rather than assumed, and the engine
architecture that follows from it.

Test rig: **Neptune (FPGA) with a MegaSD cart**, unless a number is marked as
coming from ares. The benchmark is `blitbench/`, forked from the marsdev 32X
skeleton. All figures are 60 Hz, 320x224, 8bpp packed pixel, 32x32 sprites, both
SH-2s unless stated.

---

## 1. The short version

You can build the game. Pixels are not the constraint, memory is.

- **~160 32x32 sprites** from the 32X at 60 Hz over a cleared layer
- **plus 80 hardware sprites and a parallax playfield** from the MD VDP, free
- **one SH-2 draws, the other runs the game** at full speed
- the real problem is **256 KB of 32X RAM against a 199 MB asset set**, which
  makes the Mega CD the critical path, not the renderer

---

## 2. Measured costs

### Framebuffer operations, play area (320x192, 30,720 words)

| Operation | Time |
| --- | --- |
| Clear, CPU longword writes | 5709 us |
| Clear, VDP fill hardware | **4170 us** |
| Copy from SDRAM, CPU longword writes | 8455 us |
| Copy from SDRAM, SH-2 DMA burst | 12485 us |

The fill is only 1.37x better than CPU writes, not the 3.7x ares claimed. DMA is
48% *worse* than the CPU and has no place in the frame loop.

### Overlapping the fill with CPU work

The fill length register covers 256 words, so a full clear is 120 blocks and the
CPU re-arms each one.

| Case | Time |
| --- | --- |
| Fill alone | 4170 us |
| SDRAM-only workload alone | 5608 us |
| Both, run back to back | 9778 us |
| Both, interleaved per block | **8204 us** |

1574 us of the fill hides, about 38%, putting the effective clear near 2600 us.
Slice size matters: one work pass per 15 blocks recovered nothing, because a
700 us chunk hides a single 35 us block and the other 14 run serially.

### Sprite throughput

All with the play area cleared by hardware fill, in default mode.

| Configuration | Peak sprites |
| --- | --- |
| Overwrite image, word writes, two CPUs | 144 |
| Plain framebuffer, word writes, two CPUs | 144 |
| Plain framebuffer, longword writes, two CPUs | **160** |
| Overwrite image, word writes, one CPU | 104 |
| Line-table scroll, restore + blit per sprite, two CPUs | 96 |
| Line-table scroll, one CPU | 64 |

From which:

- **A blit costs ~173 us contended, ~120 us on one CPU.** A restore costs the
  same, which follows from the identical word count.
- **Hardware transparency is free.** The overwrite image measures identically to
  plain framebuffer writes, so let the hardware discard the zero bytes. There is
  nothing to gain from run-length encoding sprites to avoid it.
- **Longword writes buy 11%**, for the price of 4 px alignment.
- **The second CPU is worth about 1.4x, not 2x.** Framebuffer writes are the
  contended resource. Confirmed three separate ways: 60 to 84 on the MarkI,
  64 to 96 and 104 to 144 on the MegaSD.

### The MD VDP is free labour

80 hardware sprites plus per-line parallax running alongside the 32X benchmark
left the peak at 152 against 144 without them, inside the ramp's own +-8 noise,
and on ares the frame time was identical to the microsecond.

- **80 hardware sprites, 16x16, cost the SH-2s nothing.** The VDP draws them;
  the 68000 rewrites the sprite table, 320 word writes a frame.
- **Per-line horizontal scroll costs the SH-2s nothing.** Mode register 3 set to
  0x03, then one word per line into the HScroll table.

---

## 3. Engine shape

### Layer order: the MD draws in front

With the default priority the Genesis VDP composites **in front of** the 32X
framebuffer, and the 32X shows through wherever MD pixels are transparent. An
opaque MD background covers 32X sprites completely. Setting `MARS_VDP_PRIO_32X`
does not usefully invert this: it makes the 32X layer fully opaque and hides the
MD entirely, text included, even with the MD tiles' own priority bit set.

So:

- **MD VDP, in front:** playfield tilemap plus its 80 hardware sprites, scrolled
  in hardware, free of SH-2 cost, at MD colour depth. Characters go here.
- **32X framebuffer, behind:** high colour parallax and effects, showing through
  the MD's transparent pixels.

This is the Knuckles Chaotix arrangement. The MD VDP absorbs the expensive
scrolling either way; you just put characters there and spend the 32X budget on
the layers behind them.

### CPU split

**One CPU owns the framebuffer, the other owns everything else.** Physics,
collision, object updates, decompression, CD streaming and audio go on the
slave, which then runs at genuinely full speed in parallel. Proven directly: a
slave fully loaded with SDRAM-only work costs the master nothing, while a slave
touching the framebuffer costs both about 40%.

The choice is not 84 sprites versus 60. It is 84 with no CPU left for logic
versus 60 plus an entire free SH-2, and in the 84 case the logic still has to
run somewhere.

**Split every per-sprite job, not just the blit.** The first line-table build had
the master doing all the restores and half the blits and measured 72. Sharing
the restores took it straight to 104.

### The per frame handoff is five words, not a sprite list

The open question in the split was what has to travel from the SH-2 to the
68000 every frame, since only the 68000 can reach the VDP. On the MD side of
the picture the answer is: almost nothing. The piece tables are in ROM and both
CPUs can read them, so the slave sends only what cannot be derived from ROM,
which is camera position, the character's position, the animation frame and
facing, plus a sequence number. That fits in the comm registers with room left,
and the framebuffer never enters the path at all.

Two rules fell out of building it. Every field owns exactly one register: the
bug blitbench hit was reporting a result through the register its joypad
arrives in. And sequence number 0 is reserved, because a comm register reads
back as zero before either CPU has written it, so a published 0 is
indistinguishable from a slave that has never run, and the 68000 would take it
for a consistent frame with everything at the world origin.

The assets are linked once, into the 68000 program. The 68000 publishes the
cartridge relative offset of a descriptor table at boot and the slave reads the
same bytes through its own view of the cartridge, so there is one copy of the
data and one place that converts between the two address spaces.

What this costs has not been measured. It is five register writes against a
frame, so there is no reason to expect it to show up, but the number is not in
hand.

### Pipeline

The slave prepares frame N+1's sprite list and decompressed tiles in SDRAM while
the master blits frame N.

### What a 32X ROM does to the 68000 side

Read before converting an MD ROM, because the shape is not negotiable.

The 32X supplies the cartridge header, the vector table and the security PROM
blob itself: `mars_start.s` emits all of it and pulls the assembled 68000
program in with `.incbin` at ROM offset 0x800. So an MD ROM's own boot file and
header get dropped rather than ported, and the 68000 program links at 0x8803F0
and starts executing at 0x880800.

Once the adapter is live the cartridge is no longer at address zero. The 68000
sees cartridge offset 0 to 0x7FFFF as a fixed 512 KB window at **0x880000**,
plus a banked 1 MB window at **0x900000** whose bank is the low two bits of
**0xA15104**, zero at power on. `md.ld` claiming a 4 MB ROM region is a lie the
linker will not catch: it will happily place data past the addressable window.

Two things the 68000 must not do while RV is set: read ROM, or take an
interrupt. Setting RV reverts the 68000 to the original cartridge board so it
can reach SRAM, which kills the ROM windows and stalls SH-2 reads from the
cartridge at the same time. Both reference implementations clear RV once at boot
and never touch it again.

`-mshort` on the 68000 side is blitbench's choice, not a 32X requirement.

**Neither reference implementation lets the 68000 read the cartridge during
gameplay at all.** blitbench marks its whole MD-side file `.data` so it runs
from RAM, and Sega's own 32X Doom does manual CPU writes where VDP DMA would
have been simpler. Both give the same reason: bus contention. Measuring it
turned up something stronger.

### VDP DMA cannot source from the 32X ROM windows

Measured on the Neptune, 128 longwords of a self-checking pattern, each case
repeated 32 times:

| Source | DMA | CPU writes |
| --- | --- | --- |
| 0x880000, fixed window | **0 / 128** | 128 / 128 |
| 0x900000, banked window, bank 0 | **0 / 128** | 128 / 128 |
| MD work RAM | 128 / 128 | 128 / 128 |

Identical with the SH-2s loaded and with them parked, so it is not contention
and not intermittent. VRAM reads back as zero, so nothing is transferred at all.
The work RAM row is what makes this conclusive: the DMA engine, the trigger
sequence, the destination and the readback are all fine on this console, and the
sequence is byte for byte the one `ghzview` uses successfully on the same
hardware. **The 32X adapter does not serve VDP DMA cycles for the cartridge
windows.**

The 68000 reading those windows with ordinary instructions works perfectly,
which the CPU column shows. So only DMA is lost, not ROM access.

### What 68000 cartridge reads cost the SH-2s

Sprite peak with the 68000 sampling one word every 64 bytes through the fixed
window, at 60 Hz, two CPUs:

| 68000 ROM reads per frame | Peak sprites |
| --- | --- |
| none | 104 |
| 1024 reads, 2 KB | 96 |
| 16384 reads, 32 KB | 88, and unusable |

The 32 KB row is past saturation and its number should not be used: sixteen
thousand windowed reads do not fit in a frame on a 7.6 MHz 68000, so the MD side
stops answering commands and the whole loop crawls. It marks where the cliff is,
not a cost.

The 2 KB row is the one that matters, and it is a **7.7% cut**, on a scattered
access pattern with no locality, which is worse than real streaming.

Against that, what a stage streamer actually reads per frame:

| | bytes |
| --- | --- |
| one block column, map plus block entries | 160 |
| one block row, when the camera crosses vertically | 320 |
| a character frame's tiles, largest case | 768 |
| collision sensor lookups | ~200 |

Under 1.5 KB worst case, and most frames stream neither a row nor a column. So a
tilemap streamer on the 68000 costs the SH-2s under 8% of their sprite budget,
and the stage data can stay in ROM rather than being staged through the
framebuffer.

What follows for an MD side that has to upload tiles:

- Character frames and tilesets go to VRAM as CPU writes. Sonic's largest frame
  is 24 tiles, 768 bytes, about 200 longword writes, which fits in vblank.
- Where a transfer is big enough to be worth it, copy ROM to work RAM with the
  CPU and DMA from there, since work RAM is a legal DMA source.
- Map and block lookups are ordinary reads and are unaffected.

ares passes all six cases, so it is no help here. This is exactly the class of
thing it gets wrong.

---

## 4. Techniques

### Line-table scrolling

In packed pixel mode the framebuffer starts with a table of word offsets, one
per scanline. Paint a playfield larger than the screen once, and scrolling is
nothing but rewriting that table: 224 word writes a frame. The benchmark uses
384x256, which is 98 KB and fits a 128 KB bank alongside the table and a blank
line, leaving 64 px horizontal and 64 lines vertical of slack.

This converts a **fixed** background cost into a **variable** per-sprite one. At
50 sprites it costs about 7 ms and hands the rest of the frame to game logic,
where a full redraw would still be spending its fixed bill.

Infinite scrolling needs more: once the view crosses the slack, either draw
incoming edges into it or rebase the buffer periodically, amortized.

### Clearing: fill or dirty rects

A per-sprite restore costs ~173 us and a full hardware fill costs 4170 us, so
**the crossover is around 24 sprites**. Below that, restore per sprite. Above,
one hardware fill of the whole layer. Switching BG 3 from per-sprite clears to a
single fill took its peak from 80 to 144 on hardware.

The fill only writes a constant, so it clears and paints flat colour but cannot
replace a patterned background copy.

### What not to bother with

- **SH-2 DMA for framebuffer work.** 48% slower than CPU writes. It may still
  earn its place for SDRAM to SDRAM or ROM to SDRAM moves during loading.
- **Run-length encoding sprites to dodge the overwrite image.** Hardware
  transparency is free.

---

## 5. Memory and assets

The binding constraint. 256 KB of 32X SDRAM against a 199 MB asset set.

Available RAM across the stack:

| Where | Size |
| --- | --- |
| 32X SDRAM | 256 KB |
| 32X framebuffer | 256 KB, two banks |
| MD work RAM | 64 KB |
| MCD PRG RAM | 512 KB |
| MCD Word RAM | 256 KB |
| MCD PCM RAM | 64 KB |

### Load per act, do not stream

The drive can only do one thing at a time. If music is CD-DA, the head is busy
playing it, and any data seek during gameplay interrupts the music and costs
about half a second, thirty frames.

So the scheme is two tiers, not streaming:

1. At act load, pull everything into MCD PRG RAM and Word RAM. Start the CD-DA
   track. The drive then never moves.
2. During play, page from MCD RAM into the 32X's 256 KB on demand through Word
   RAM and the DREQ FIFO. Fast, and seek free.

This breaks only if an act's working set exceeds roughly 768 KB of MCD RAM.
Since the art is being re-authored for 32X anyway, that is a budget to design
to rather than engineer around.

**The SH-2s cannot touch Word RAM or PRG RAM directly.** The MD 68000 is the
only courier. That is its job in this engine, alongside sound driver dispatch.

### What else the MCD brings

- **CD-DA** for music, zero CPU. No loop points in the format, so looping means
  seeking back and wearing the gap, or keeping tight loops in PCM.
- **RF5C164 PCM**, 8 channels, 64 KB of PCM RAM, for sound effects.
- **Sub-CPU**, a 68000 at 12.5 MHz with 512 KB to itself, for decompression.
- **Backup RAM**, 8 KB, for saves, so no battery on the cart.
- The **ASIC** does hardware stamp rotation and scaling into Word RAM. Probably
  not worth it for a 2D platformer once you account for shipping the result to
  the 32X or MD VDP.

---

## 6. Traps

**marsdev silently ships a stale 68000 program.** `sh_src/mars_start.s` pulls the
assembled MD binary in with `.incbin`, and the stock Makefile has no dependency
between `sh_src/mars_start.o` and `md_start.bin`. Change anything MD-side and the
ROM keeps whatever copy was assembled last, with no warning. Cost hours: every
MD-side diagnostic came back negative because none of the code under test was in
the ROM. Fixed here with an explicit dependency. Suspect it first whenever
MD-side changes appear to do nothing.

**marsdev never releases the slave SH-2.** The master is supposed to clear
`COMM4`, but `r0` is clobbered by the `jsr` into the C initializers immediately
before, so the store lands in ROM. The stock skeleton never noticed because its
`s_main` is an empty `for(;;)`. Patched in `sh_src/mars_start.s`. Worth reporting
upstream.

**Z80 bus request: the busy flag is bit 0, not bit 8.** Write `0x100` to
`0xA11100`, then poll bit 0 until it clears. Bit 8 is the request itself and
never clears, so waiting on it hangs the 68000 forever and the screen goes black
with no text, since the MD stops servicing commands.

**The controller ports need their direction registers set.** Writing 0x40 to
the data register at 0xA10003 only drives TH if 0xA10009 says TH is an output,
and it comes up as an input. Without that write the pad read comes back from a
floating pin: on the Neptune the ROM looked either hung or unresponsive, while
ares drove TH regardless and behaved perfectly. Set 0xA10009, 0xA1000B and
0xA1000D to 0x40 before the first read. Suspect this first whenever input works
in an emulator and not on hardware.

**`vdp_vsync` returns at the end of vblank, not the start.** The marsdev
skeleton's version waits for the flag to set and then to clear, so anything
after it runs in active display. A tile DMA placed there gets about eighteen
bytes a scanline instead of two hundred and stalls the 68000 for most of the
frame. `vdp_wait_vblank` returns at the start of the interval; put DMA there.

**Per-line HScroll collides with the sprite table.** It needs 224 lines x 4
bytes, occupying 0xFC00 through 0xFF7F, which runs over the default sprite
attribute table at 0xFE00. Sprites silently vanish. Moved here to 0xF000
(register 5 = 0x78). Plan the VRAM map before enabling per-line scroll.

**marsdev never initialises the SH-2 free running timer.** Technical Bulletin
10 says it has to be, interrupts or no interrupts, and Bulletin 27's fix for the
double-acknowledge erratum depends on it. Sega's own 32X Doom programs the whole
FRT block before anything else; `mars_start.s` skips it and its interrupt
handlers omit the bulletin's workaround. Only bites once more than one SH-2
interrupt source is live, and the two omissions compound.

**GCC's byte copy can land one byte off, and the 68000 manual does not settle
it.** A plain `while (n--) *dst++ = *src++;` over `uint8_t*` compiles at -O2 to
`move.b (%a0)+,(%a0,%d1.l)`: the same address register post-incremented as the
source and used as the index of the destination. GCC computes `d1` expecting the
destination to see the register *before* the increment. ares fetches the source
first, so the register is already incremented when the destination address is
worked out, and the whole copy lands one byte high. In `cdbench` this presented
as a sub-CPU program that copied into PRG RAM with byte zero never written, and
it survived a separate byte-write test that passed, because the test wrote
through a `volatile` pointer and never got the idiom. Which side real hardware
takes is unmeasured and is worth five lines on the Neptune, since it needs no CD
at all. Until then, do not let the compiler generate it: copy word-wise, or
write through `volatile`. No current ROM in this repo contains the instruction.

**Cache alignment is worth a few percent.** The same 104 sprites measured 16.6 ms
in one build and 17.2 ms in the next, purely from unrelated edits shifting the
blit loop inside the 4 KB cache.

**Silence the audio hardware at boot.** Nothing in the skeleton does. The PSG
powers up audible, the YM2612 comes up with undefined registers, and the Z80 runs
loose out of uninitialized RAM writing garbage into both. `audio_silence()` in
`md_main.c` handles all three, plus the 32X PWM. Note that 0 is a prohibited
value for the PWM cycle register.

---

## 7. On measurement

**Ares gets parallel scaling badly wrong.** It reported a clean 2.00x from the
second CPU where hardware gives 1.4x, because it models no contention at all:
its `MIN` and `MAX` came out identical to the microsecond. It also had the fill
3.7x faster than CPU writes where hardware says 1.37x, with the error in both
directions at once. Single threaded it is roughly right. Treat it as a
correctness check and take timings from hardware.

**Compare like with like.** Two wrong conclusions in this project's history came
from comparing runs taken in different configurations, one of them recommending
a blitter rewrite that the clean numbers say is pointless. `PEAK` resets on every
mode change and the ramp climbs in steps of 8 every half second, so give it 15 to
20 seconds to settle before reading.

**Not every problem is the ROM.** An audible pop chased across three emulator
settings and a full YM2612 silencing routine turned out to be the MacBook's
built-in speakers, which do it with any game under ares.

---

## 8. Status

Done: blit budget, CPU split, line-table scrolling, MD VDP sprites and parallax,
hardware fill, fill overlap, DMA, layer order.

Also done: a stage converted from the data pack, Player.c's ground and air
physics, the RSDKv5 tile collision, a camera ported from Camera.c, and Sonic's
own sprite on hardware sprites with the collision box coming from the animation
frame. Now a 32X ROM, with the Mega Drive drawing the foreground and the master
SH-2 drawing the layer behind it, which is the compositing arrangement this
document argues for, proven with real art. The game logic has moved onto the
slave SH-2: the 68000 keeps the VDP and gives up pad, physics, collision,
camera and the animator, and the frame reaches it through five comm registers
rather than a sprite list. See `ghzview/`.

Emulator path for the full tower confirmed working, cart first then disc:

```sh
ares --system "Mega CD 32X" blitbench.32x "Sonic CD (USA).chd"
```

**The tower has no hardware test path here, and everything below is written
knowing that.** The 32X side is a Neptune and the CD side is a MegaSD, which is
itself a cartridge, so the two cannot be stacked. Every CD finding until that
changes is an emulator finding and gets labelled as one. Section 7 is the reason
this matters: both hardware-only traps in this document, DMA out of the ROM
windows and the 128 KB wrap, were invisible in emulation, and the CD path runs
through the same adapter that produced them. ares stays the development loop for
CD work, and the work goes on; what it cannot be is the evidence, so CD results
do not get to settle architecture on their own.

The one thing that might close the gap is the MegaSD itself: it is a flash cart
that also emulates Mega CD hardware, so it can carry the test ROM and be the
device under test at once. Whether it answers at 0x400000 with an adapter in the
way is unknown and worth a single measurement.

**Mode 1 bring-up runs, in emulation.** `cdbench/` reaches the CD BIOS at
0x415800, resets the sub-CPU, probes PRG RAM through the adapter, decompresses
the sub-CPU BIOS, uploads a 224 byte program, starts it, completes the
INITIALIZING to READY handshake in about 2,900 spins and holds a per frame ping
for thousands of frames with no failures. So the windows at 0x400000 do decode
with a 32X in the way, and the comm register protocol works, in ares. On a bare
32X the same ROM prints NO CD DETECTED and idles.

Two things that cost real time and are worth knowing before touching this again.
The cart must declare Mega CD support: `mia/medium/mega-32x.cpp` reads the
device field at ROM offset 0x190 and only attaches the CD hardware if it finds a
`C` there. And the tower has to be launched as `--system "Mega 32X"` with the
cart alone; `--system "Mega CD 32X"` treats the first file as a disc image,
leaves the cartridge slot empty and boots the BIOS CD player instead, which is
what "confirmed working" above actually meant.

```sh
ares --system "Mega 32X" --no-file-prompt cdbench/cdbench.32x
```

The CD BIOS needs a level 2 interrupt at roughly vblank rate to make progress,
and this ROM cannot give it one the way the reference does, since 68000
interrupts stay off here. Driving it off the VDP's vblank flag works. Driving it
on every spin of a wait loop does not: the sub-CPU can re-enter the handler
faster than it leaves it, and the wait then times out for a reason that has
nothing to do with the hardware.

Next, in order:

1. **One zone end to end**, to get a real cost per act. First because it is the
   only item on this list that can be measured on the hardware in the room.
2. **CD to 32X pipeline.** Word RAM staging through the DREQ FIFO. Not
   "measured", the way the rest of this document uses that word, until there is
   a tower to measure.

Not yet measured, and probably not worth measuring: the Z80 as a sound driver.
It is standard MD practice with no surprise in it, and it only matters once the
68000 is busy running the CD pipeline.
