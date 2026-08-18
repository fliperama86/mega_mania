#include "platform_clock.h"
#include "comm.h"

uint32_t g_platform_tick_md = 0;

static uint8_t lastSeqObserved;
static uint8_t everObserved = 0;

/* See platform_clock.h's own top comment for the full derivation. This
 * function's only job: turn comm_last_seq()'s wrapping 8-bit observation
 * (period 255, not 256 -- sh_src/comm.c's own seq skips the value 0) back
 * into the same wide, monotonic tick count sh_src/platform.c's own
 * g_platform_tick already is. */
void platform_clock_sync(void)
{
	uint8_t seq = comm_last_seq();

	if (seq == 0) return;   /* boot: no frame published yet (should not
	                          * actually happen once main() reaches its
	                          * per-frame loop -- main()'s own boot-time
	                          * comm_read_frame() spin already guarantees a
	                          * real frame is on the bus by then -- kept as a
	                          * defensive no-op rather than assumed. */

	if (!everObserved) {
		/* The very first seq this 68000 EVER observes is, by construction of
		 * the boot handshake, always the slave's tick 1: the slave can only
		 * ever run ONE tick (its own "free" first iteration, sh_src/comm.c's
		 * comm_wait_tick() own -1-sentinel-never-matches first call) before
		 * blocking on comm_wait_tick() for a real tick CHANGE, which cannot
		 * happen until this 68000 reaches ITS OWN main loop and starts
		 * calling comm_send_input() -- well after this 68000's own boot-time
		 * comm_read_frame() spin has already consumed that frozen first
		 * tick. Not a race: the slave is structurally incapable of reaching
		 * tick 2 before this 68000's boot-wait loop has already observed
		 * tick 1. See platform_clock.h's own comment for the matching
		 * guarantee on sh_src/platform.c's side (g_platform_tick starts at 0,
		 * platform_apply() increments it to 1 on that exact same first,
		 * otherwise-input-less tick). */
		everObserved = 1;
		lastSeqObserved = seq;
		g_platform_tick_md = 1;
		return;
	}

	if (seq == lastSeqObserved) return;   /* nothing new since last check */

	{
		int32_t diff = (int32_t)seq - (int32_t)lastSeqObserved;
		if (diff < 0) diff += 255;   /* seq's own period is 255 (1..255, 0 skipped) */
		g_platform_tick_md += (uint32_t)diff;
		lastSeqObserved = seq;
	}
}
