#ifndef SH_COMM_H
#define SH_COMM_H

#include <stdint.h>
#include "mars.h"

/* The 68000 <-> slave SH2 comm-register protocol. This is the one canonical
 * writeup; md_src/comm.h carries only the matching 68000-side addresses and
 * points back here.
 *
 * Reserved, never touched by any of this: COMM0 and COMM4 are the boot
 * handshake between the 68000 and the two SH2s (md_src/md_start.s,
 * sh_src/mars_start.s). Neither this protocol nor the descriptor handoff in
 * assets.c ever reads or writes them.
 *
 * Register map (SH2 addresses; the matching 68000 address is always
 * SH2_addr - 0x1EFEEF00, verified against md_start.s's known-correct
 * COMM0/COMM4 pair: 0x20004020 -> 0xA15120, 0x20004024 -> 0xA15124):
 *
 *   COMM0  (0x20004020)  reserved, boot handshake only.
 *
 *   COMM2  (0x20004022)  Boot, one-shot: descriptor-ready flag. The 68000
 *                        writes 1 here last, after COMM12 and COMM6 below
 *                        are already committed, so assets_init() spinning on
 *                        this is guaranteed to see valid data in both.
 *                        Steady state: camera X, screen-space top-left
 *                        corner, already clamped to the map. Slave writes,
 *                        68000 reads.
 *
 *   COMM4  (0x20004024)  reserved, boot handshake only.
 *
 *   COMM6  (0x20004026)  Boot, one-shot: screenCenterY (SCREEN_HALF_H - 16,
 *                        PAL/NTSC-aware). This is a deliberate deviation
 *                        from putting screenCenterY inside the ROM-resident
 *                        AssetDescriptor: that struct is genuine read-only
 *                        ROM once flashed, and screenCenterY is only known
 *                        after vdp_init() reads the PAL/NTSC hardware bit at
 *                        runtime, so it cannot live in ROM at all. COMM6 is
 *                        otherwise idle at boot (nothing needs it before
 *                        steady state), so it carries this one extra
 *                        one-shot value the same way COMM12 already carries
 *                        the descriptor offset before being reinterpreted.
 *
 *                        Steady state: camera Y in bits [11:0], plus two
 *                        fields COMM_ANIM's word has no room for (see its
 *                        own entry below, which is exactly full): bits
 *                        [14:12] are dispRot, the 3-bit snapped sprite
 *                        rotation (sh_src/player.c's ground_rotation/
 *                        air_gravity compute Player.rotation, 0-511, exactly
 *                        like Player_HandleGroundRotation/HandleAirRotation,
 *                        Player.c:3207-3254; this is that value snapped to
 *                        one of 8 steps by the engine's own ROTSTYLE_45DEG
 *                        formula, Drawing.cpp:2703-2704 --
 *                        dispRot = ((rotation + 0x20) & 0x1C0) >> 6), and
 *                        bit [15] is Player.drawGroupHigh (player.h),
 *                        PlaneSwitch's other write alongside collisionPlane
 *                        (PlaneSwitch_CheckCollisions, PlaneSwitch.c:94-109
 *                        -- other->drawGroup = low/high, the mechanism
 *                        Zone->playerDrawGroup[0]/[1] names in the original),
 *                        telling md_src/sonic.c whether to draw Sonic's
 *                        sprite at low or high priority against FG High.
 *                          dispRot = ((rotation + 0x20) & 0x1C0) >> 6;
 *                          word = (camY & 0x0FFFu) | ((dispRot & 7u) << 12)
 *                                | ((drawGroupHigh & 1u) << 15);
 *
 *                        Bits [15:12] cannot collide with a legitimate camY
 *                        value, so none of the four are stolen coordinate
 *                        bits: s_main.c clamps the published camY every
 *                        frame to at most `cam.boundsB - SCREEN_H` (its
 *                        limitY), and cam.boundsB is eased toward
 *                        ZoneBounds.cameraBoundsB, which starts at the FG
 *                        layer height in px (bounds_init, bounds.c:
 *                        `g_map_h * 16`) and is only ever narrowed further by
 *                        BoundsMarker entries, never widened past the layer.
 *                        GHZ Act 1's layer is 128 blocks tall (2048 px);
 *                        Act 2's 96-block layer (docs/green-hill.md) is
 *                        smaller still. So camY tops out in the low
 *                        thousands for any act this converter could
 *                        plausibly produce, comfortably inside 12 bits
 *                        (4096) and nowhere near bit 15's value of 32768 --
 *                        the invariant, not a one-time measurement, is what
 *                        makes the bits free. s_main.c backs this with a
 *                        one-line boot trap once g_map_h is known (`g_map_h
 *                        * 16 <= 4096`), so a future stage that broke the
 *                        invariant hangs at boot instead of silently
 *                        corrupting dispRot/drawGroupHigh every frame after.
 *                        Slave writes, 68000 reads; the master SH2 (bg.c)
 *                        also reads this register directly for its own
 *                        parallax math and masks camY down to bits [11:0]
 *                        before using it, since it never needs dispRot or
 *                        drawGroupHigh.
 *
 *   COMM8  (0x20004028)  Steady state only: Sonic's world X, an int16_t bit
 *                        pattern carried in a uint16_t register. Slave
 *                        writes, 68000 reads.
 *
 *   COMM10 (0x2000402A)  Steady state only: Sonic's world Y, same terms as
 *                        world X. Slave writes, 68000 reads.
 *
 *   COMM12 (0x2000402C-0x2F, all 32 bits)
 *                        Boot, one-shot: the descriptor table's cartridge-
 *                        relative offset (the 68000 program links at
 *                        0x880000+, so this is (uint32_t)&asset_descriptor
 *                        - 0x880000u), written once by the 68000 as a single
 *                        32-bit store before COMM2's ready flag goes up.
 *                        Once assets_init() has consumed it, this address is
 *                        never accessed as one 32-bit register again: it
 *                        splits into two independent 16-bit halves below.
 *
 *   . upper 16 bits, address 0x2000402C (COMM_ANIM here, not the existing
 *     32-bit MARS_SYS_COMM12 -- writing through that macro post-boot would
 *     be a 32-bit store that clobbers the lower half, which independently
 *     holds the 68000's tick+pad word; that would be a real, silent
 *     corruption bug, exactly the kind blitbench hit reusing COMM8 for two
 *     unrelated purposes, so this gets its own distinctly-named macro):
 *     Steady state only: the packed anim word, slave writes, 68000 reads.
 *       bits [15:8] seq, uint8_t, incremented once per frame published.
 *                   0 is reserved and never a genuinely published value:
 *                   comm_publish_frame's counter starts at 0 before the
 *                   slave has ever published a frame, and this register
 *                   also reads back as plain 0 at power-on, before either
 *                   CPU has touched it -- if 0 were a legal published
 *                   value there would be no way for the 68000 to tell
 *                   "the slave published seq 0" apart from "the slave
 *                   has never published anything", and the latter would
 *                   be misread as a valid, consistent, all-zero frame
 *                   (camera and Sonic both at world (0,0)) well before
 *                   the slave has run a single real update. So
 *                   comm_publish_frame skips 0 when its seq++ wraps
 *                   through it, keeping every genuinely published value
 *                   in 1..255, and comm_read_frame treats an observed
 *                   seq of 0 as "not ready yet" unconditionally, never as
 *                   a consistent frame even if two reads agree on it.
 *       bits [7:1]  absolute frame index into sonic_frames[] (7 bits,
 *                   0-127; the generated table currently has 124 entries
 *                   (tools/convert_sonic.py's SONIC_FRAME_COUNT), 3 short of
 *                   the 127-frame limit. One of the unused values past the
 *                   real table, SONIC_FRAME_COUNT itself (124), is claimed
 *                   as a sentinel: "do not draw Sonic this tick", the blink
 *                   half of post-hit invulnerability (sh_src/player.c's
 *                   player_hit()/Player.hidden field, sh_src/s_main.c's
 *                   publish call). This spends none of comm.h's bit/
 *                   register budget: sonic_frames[] never had this index to
 *                   begin with, and md_src/sonic.c's sonic_build()/
 *                   sonic_upload() both bounds-check frameIndex against
 *                   SONIC_FRAME_COUNT and skip drawing/uploading on exactly
 *                   this value, the same out-of-range convention md_src/
 *                   rings.c's own FALLBACK_HITBOX check already established
 *                   for this field, not a new one. Values 125-127 stay
 *                   genuinely unused headroom, 3 short of a 10th new pose
 *                   without any protocol change.
 *       bit  [0]    facing: 0 = right, 1 = left, matching Player.direction.
 *     word = ((uint16_t)seq << 8) | ((frameIndex & 0x7Fu) << 1) | (facing & 1u);
 *
 *     This word is exactly full (8+7+1 = 16 bits): seq needs its full 8 bits
 *     (the seqlock's whole reason to be a uint8_t, not a narrower counter,
 *     is in the paragraph above), and frameIndex's 7 bits are the minimum
 *     for the current 124-entry sonic_frames[] table (124 > 2^6, so 6 bits
 *     is not enough -- the 3-frame gap to 127 above describes headroom in
 *     the VALUE 124 has against 127, not a spare BIT position).
 *     drawGroupHigh rides COMM6's bit 15 instead -- see that register's
 *     entry above for why there was room there and not here.
 *
 *   . lower 16 bits, address 0x2000402E (COMM_TICK here; there is no
 *     existing MARS_SYS_COMM14 in mars.h, so this is a wholly new macro,
 *     kept in this file rather than added to mars.h to keep every new
 *     protocol address in one place):
 *     Steady state only: the packed tick+ring+pad word, 68000 writes, slave
 *     reads.
 *       bits [15:9] tick, 7 bits (0-127), incremented once per 68000
 *                   vblank, wrapping mod 128. This field is never read as
 *                   an absolute value -- its one consumer (sh_src/bg.c's
 *                   g_driftTick/tickDelta, see that file's comment above
 *                   line_offset() and above tickDelta's own computation)
 *                   only ever takes an unsigned difference of two samples.
 *                   That difference is masked to 7 bits (`& 0x7Fu`) before
 *                   use: while tick occupied a full uint8_t's 0-255 range,
 *                   plain uint8_t subtraction's own mod-256 wraparound was
 *                   already exactly the field's wraparound, for free; now
 *                   that the field's logical modulus (128) is narrower than
 *                   its uint8_t storage (256), the subtraction needs that
 *                   explicit mask to keep wrapping correctly through
 *                   127->0 -- without it, a delta that crosses that
 *                   boundary comes out wrong by +128. Narrowing tick from 8
 *                   bits to 7 only changes the wrap period, from 256 real
 *                   vblanks to 128 (~2.1s at 60Hz instead of ~4.3s), which
 *                   only matters if the master SH2 (bg.c, a separate CPU
 *                   from the slave this register is named for) ever falls
 *                   more than one full wrap period behind the 68000's tick
 *                   -- a state in which the game is already unplayable,
 *                   tick delta or not.
 *                   Narrowing this field is what frees bit [8] below.
 *       bit  [8]    hasRings: 1 if md_src/rings.c's own ring counter
 *                   (ringPlayerCount) is nonzero as of this 68000 vblank, 0
 *                   at exactly zero rings. Not a second ring counter --
 *                   the only fact sh_src/player.c's player_hit() needs from
 *                   rings.c's count to transcribe Player_Hit's `hurtType =
 *                   (player->rings <= 0) + PLAYER_HURT_RINGLOSS`
 *                   (Player.c:3572): whether the hit should be survivable
 *                   knockback or an instant kill. Read through
 *                   comm_has_rings() below, cached from the same word
 *                   comm_wait_tick() already reads every tick -- not a
 *                   second register access.
 *       bits [7:0]  the pad byte pad_read() just returned this frame.
 *     word = ((uint16_t)(tick & 0x7Fu) << 9)
 *          | ((uint16_t)(hasRings & 1u) << 8) | (pad & 0xFFu);
 *
 * Why every field gets its own register: blitbench reports diagnostic
 * results through COMM8, the same register its joypad arrives in, which
 * caused a real bug when the two uses collided. Every field above has
 * exactly one steady-state role and one owner; nothing here is multiplexed
 * onto a register something else is concurrently using for a different,
 * temporally-overlapping purpose. Two registers deviate from "one role per
 * register," in two different, both-safe ways: COMM6 and COMM12 carry a
 * boot-time role and a steady-state role that never overlap in time (the
 * boot role is fully consumed, once, before the steady-state role is ever
 * written); COMM6's own steady-state role additionally packs three fields
 * (camY, dispRot, drawGroupHigh) into one word, safe not because they are
 * temporally separated -- all three are live every steady-state frame --
 * but because an invariant on camY's own range leaves bits [15:12] with no
 * legitimate value ever to collide with (see COMM6's entry above). That is
 * a different, stronger guarantee than blitbench's bug had: diagnostic
 * writes and joypad reads on COMM8 had no such invariant keeping them
 * apart, which is exactly why they collided.
 *
 * Frame boundary handling (comm_publish_frame/comm_read_frame):
 *
 * Slave publish order (comm_publish_frame, this file's .c): camera X,
 * camera Y, world X, world Y, THEN the packed anim word with the bumped seq,
 * last. By the time the 68000 observes a new seq value, every other field
 * for that frame is already on the bus.
 *
 * 68000 read (comm_read_frame, md_src/comm.c): a seqlock, bounded to 4
 * attempts. Read the packed anim word, extract seq1; if seq1 is 0, the
 * slave has never published a frame yet -- this attempt is discarded
 * unconditionally (never cached, never accepted, even if a second read
 * would agree), which is what makes the 68000's one-time startup spin on
 * this function's return value genuinely block until the slave's first
 * real frame lands, instead of accepting the register file's all-zero
 * power-on state as a valid frame. Otherwise, if seq1 equals the last seq
 * this 68000 already consumed, there is nothing new -- keep using the
 * previously cached values for this display frame (the data words are
 * never re-read in that case, so there is nothing to tear). Otherwise read
 * the four data words, then re-read the packed anim word and extract seq2;
 * if seq2 == seq1, the four data words are self-consistent for that exact
 * frame, accept them and remember seq1 as consumed. If seq2 != seq1, the
 * slave overwrote mid-read; retry, up to the 4-attempt budget. If the
 * budget is exhausted, fall back to the last known-good cached values
 * rather than use a torn read. A sentinel outside 0-255 (int32_t, starts at
 * -1) marks "no frame consumed yet" so the very first read after boot is
 * unconditionally treated as new, since seq wraps through the full uint8_t
 * range and 0 is a value it can legitimately take -- except 0 itself, which
 * is reserved (see COMM_ANIM's bit layout above) and can never be "the last
 * seq consumed". This never lets the 68000 observe a half-written set of
 * values: the seqlock guarantees a torn read is detected and discarded,
 * never accepted. It can drop an entire frame's worth of new data (if the
 * slave races ahead and overwrites before the 68000 got to it, or if the
 * retry budget is exhausted), but a dropped frame is always a complete,
 * self-consistent frame becoming stale-by-one, never a mix of two frames'
 * fields.
 *
 * 68000 -> slave direction (comm_send_input/comm_wait_tick): the 68000
 * writes the tick+ring+pad word once per its own loop iteration, immediately
 * after reading the pad and before anything else that frame, matching where
 * the original single-CPU code called player_update immediately after
 * pad_read, so the phase relationship between "a vblank happened" and
 * "input for that vblank is available" is unchanged. The slave spin-waits
 * for the tick field to change, then extracts both the pad byte and the
 * hasRings bit from that same 16-bit word (atomic together: a single SH2
 * word load can never observe a new tick paired with a stale pad byte or a
 * stale hasRings bit, or vice versa), and runs exactly one player_update/
 * collision/camera update per observed tick change. This is the mechanism
 * that keeps physics running at exactly one update per real 60 Hz vblank
 * instead of running as fast as the SH2's loop can spin, which would
 * otherwise silently change gameplay speed: preserving this is required by
 * "do not change any game behaviour" even though nothing named this
 * mechanism directly. comm_wait_tick() returns the pad byte, same as
 * before; comm_has_rings() below returns the hasRings bit it cached from
 * the same read. */

/* Post-boot 16-bit views of the two halves of MARS_SYS_COMM12's address
 * range; see the comment above for why these cannot reuse mars.h's existing
 * 32-bit MARS_SYS_COMM12 macro post-boot. */
#define COMM_ANIM (*(volatile uint16_t *)0x2000402C)
#define COMM_TICK (*(volatile uint16_t *)0x2000402E)

/* Publish one frame's worth of camera/Sonic state. Called once per observed
 * tick change from s_main.c's game loop, after player_update/path/camera.
 * drawGroupHigh and rotation pack into camY's bits 15 and [14:12] (see
 * COMM6's entry above); pass Player.drawGroupHigh (sh_src/plane_switch.c
 * writes it) and Player.rotation (player.c's ground_rotation/air_gravity)
 * verbatim -- this function does the ROTSTYLE_45DEG snap to dispRot. */
void comm_publish_frame(uint16_t camX, uint16_t camY, int16_t worldX, int16_t worldY,
                         uint16_t frameIndex, uint8_t facing, uint8_t drawGroupHigh,
                         uint16_t rotation);

/* Blocks until the 68000 publishes a new tick, then returns the pad byte
 * that arrived atomically with it. Also caches the hasRings bit (COMM_TICK
 * bit [8], see above) from that same word for comm_has_rings() to read --
 * one word read serves both, never a second register access. */
uint16_t comm_wait_tick(void);

/* The hasRings bit comm_wait_tick() last cached from COMM_TICK: 1 if
 * md_src/rings.c's ring counter was nonzero as of the most recently
 * consumed tick, 0 at exactly zero rings. sh_src/player.c's player_hit()
 * is the one consumer -- see COMM_TICK's own entry above for why this one
 * bit is what the 0-rings-death rule needs, not the count itself. Reads 0
 * before the first comm_wait_tick() call, which is the conservative
 * default (kills on a hit) rather than the permissive one. */
uint8_t comm_has_rings(void);

#endif
