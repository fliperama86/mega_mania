#ifndef PLATFORM_CLOCK_H
#define PLATFORM_CLOCK_H

#include <stdint.h>

/* THE SHARED CLOCK -- how Platform/Bridge/CollapsingPlatform's moving
 * positions agree EXACTLY between the 68000 (drawing) and the slave SH2
 * (collision, sh_src/platform.c) with no comm register at all, given
 * sh_src/comm.h's own "fully allocated, no spare bits" ceiling.
 *
 * THE PROBLEM: Platform_State_Linear/Swing (Platform.c) are `amplitude *
 * Sin1024(speed*(rotation+Zone->timer))` -- a pure function of a single
 * global tick counter this port never had (no Zone struct). Both CPUs need
 * the IDENTICAL value of that counter for the SAME displayed frame, or the
 * sine phases desync and the drawn platform silently drifts away from the
 * collision surface Sonic is actually standing on.
 *
 * THE CONSTRAINT: sh_src/comm.h's own register map is exactly full. Nothing
 * can be added to carry a tick count across.
 *
 * THE SOLUTION: don't send it -- RECONSTRUCT it from a value that is
 * ALREADY on the bus for an unrelated reason. sh_src/comm.c's
 * comm_publish_frame() already increments a seq counter by exactly 1 every
 * single tick it publishes (skipping only the reserved value 0, sh_src/
 * comm.h's own COMM_ANIM entry) -- that counter's sole documented purpose
 * today is the seqlock's own torn-read detection, but it is, incidentally,
 * EXACTLY a 1:1 count of "how many ticks the slave has processed and
 * published so far". sh_src/platform.c's own g_platform_tick is defined to
 * be that exact count too (incremented once per s_main.c loop iteration --
 * see that file's own top comment). So: g_platform_tick (SH2) and seq's own
 * running total (68000) are the SAME quantity, counted independently on two
 * CPUs that can never talk to each other about it directly, and this file's
 * job is to turn the 68000's own OBSERVATIONS of seq (an 8-bit value that
 * wraps, skipping 0, roughly every 255 ticks) back into that same wide,
 * unwrapped count.
 *
 * WHY THIS IS EXACT, NOT APPROXIMATE: platform_clock_sync() is called once
 * per displayed 68000 frame, from platform_tick() (md_src/platform.c),
 * itself one row's ObjTickFn -- i.e. once per frame, AFTER comm_read_frame()
 * has already run for that frame (main.c's own per-frame order: comm_
 * read_frame(), then every registered type's tick()). comm_last_seq()
 * (md_src/comm.h) therefore returns the seq value belonging to EXACTLY the
 * worldX/worldY/frameIndex this same frame is about to draw everything else
 * from -- seq and those data words are written together, atomically, by the
 * SAME comm_publish_frame() call on the slave (sh_src/comm.c), so there is
 * no possible "seq is one tick ahead/behind the position data" skew to
 * worry about: they are always from the identical tick, by construction of
 * the seqlock itself. Each time this function observes a NEW seq value, it
 * advances g_platform_tick_md by exactly the number of real ticks that
 * elapsed since the last observation (see platform_clock.c's own
 * unwrap-and-delta arithmetic for the exact skip-0 correction), so
 * g_platform_tick_md, read from ANY of this batch's draw()/decide()
 * functions this same frame, is EXACTLY the g_platform_tick value the slave
 * used when it computed and published the data this frame is drawing --
 * not a frame behind, not approximated, not independently free-running.
 *
 * PROOF, NOT JUST ARGUMENT: /private/tmp/.../scratchpad/platform_verify/ (see
 * this batch's own report) runs both sides' exact update rules -- sh_src/
 * platform.c's own g_platform_tick++ once per simulated tick, and this
 * file's own comm_last_seq()-driven resync, fed the SAME simulated seq
 * sequence (including deliberate stale-read repeats and the once-per-255-
 * ticks wrap) -- for 100,000+ simulated ticks and asserts g_platform_tick_md
 * == g_platform_tick on every one where a frame was actually consumed. */
void platform_clock_sync(void);

/* The 68000's own reconstruction of sh_src/platform.c's g_platform_tick --
 * see this file's own top comment for why the two are always numerically
 * equal for whatever frame is currently on the bus. Starts at 0; the very
 * first platform_clock_sync() call (main.c's boot-time comm_read_frame()
 * spin already guarantees a real seq is on the bus by the time any tick()
 * ever runs) sets it to 1, matching sh_src/platform.c's own first tick. */
extern uint32_t g_platform_tick_md;

#endif
