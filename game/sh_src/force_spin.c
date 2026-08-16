#include <stdint.h>
#include "force_spin.h"
#include "trig.h"

/* ForceSpin.c's marker fields, cut down to what this port's table needs.
 * x,y: px, ForceSpin->position pre-TO_FIXED. angle: byte-scale,
 * ForceSpin->angle. halfLen: px, ForceSpin->size*8 (the size<<19 term in
 * ForceSpin_Update's containment check is exactly TO_FIXED(size*8), so the
 * table stores the already-multiplied pixel value rather than a second
 * "size" column). dir: ForceSpin->direction, edited as FLIP_NONE(0)
 * ("Enter From Right, Exit From Left") or FLIP_X(1) ("Enter From Left, Exit
 * From Right") -- ForceSpin_EditorLoad's only two enum rows, so this is
 * always 0 or 1, never any other FlipFlags bit. */
typedef struct {
	int16_t x, y;
	uint8_t angle;
	uint8_t halfLen;
	uint8_t dir;
} ForceSpinDef;

/* GHZ Scene1.bin's ForceSpin entities (Mania filter). */
static const ForceSpinDef k_markers[] = {
	/*    x,    y, angle, halfLen, dir */
	{  6156,  632,   0, 32, 0 },
	{  6904, 1140,   0, 32, 1 },
	{  6664,  376,   0, 32, 0 },
	{  6904,  632,   0, 32, 1 },
	{ 12680,  600, 192, 32, 0 },
	{  8696, 1736,   0, 32, 0 },
	{  8744, 1768,  64, 32, 0 },
	{  6672,  888,   0, 32, 0 },
	{  6388,  884,   0, 32, 1 },
	{ 13880, 1720,   0, 32, 1 },
	{ 13868, 1464,   0, 32, 1 },
	{ 14868, 1288,   0, 16, 0 },
	{ 14796, 1000,   0, 16, 1 },
};
#define MARKER_COUNT (sizeof(k_markers) / sizeof(k_markers[0]))

#define TO_FIXED(x) ((int32_t)(x) << 16)

static int32_t iabs(int32_t v)
{
	return v < 0 ? -v : v;
}

/* Zone_RotateOnPivot (SonicMania/Objects/Global/Zone.c:506-512): rotate
 * (*px,*py) around (ox,oy) by angle. The >>8-before-multiply order (rather
 * than this port's usual (a*trig)>>8, e.g. path.c/player.c's groundVel*cos256
 * >>8) is deliberate, not a transcription slip: shifting the *position*
 * delta down first is what keeps world-scale coordinates inside int32 -- the
 * trig tables are already scaled by 256, so (delta>>8)*trig lands back on
 * the same 16.16 scale as a plain (delta*trig)>>16 would, without ever
 * forming that wider intermediate. */
static void rotate_on_pivot(int32_t *px, int32_t *py, int32_t ox, int32_t oy, uint8_t angle)
{
	int32_t x = (*px - ox) >> 8;
	int32_t y = (*py - oy) >> 8;
	*px = ox + y * sin256(angle) + x * cos256(angle);
	*py = oy + y * cos256(angle) - x * sin256(angle);
}

/* ForceSpin_SetPlayerState (ForceSpin.c:96-120). player->pushing and the
 * PlaySfx/next{Ground,Air}State assignments are not ported: no pushing
 * feature, no audio hooks at this layer, and no StateMachine_None analog
 * needed since this port has nothing that reads a "next state" field. */
static void set_tube_state(Player *p, uint8_t dir)
{
	if (p->state == PSTATE_TUBE_ROLL || p->state == PSTATE_TUBE_AIR)
		return;

	if (p->animator.anim != ANI_JUMP) {
		sonic_set_anim(&p->animator, ANI_JUMP, 0, 0);
		if (p->e.collisionMode == CMODE_FLOOR && p->e.onGround)
			p->e.y += PHYS_JUMP_OFFSET;
	}

	p->state = p->e.onGround ? PSTATE_TUBE_ROLL : PSTATE_TUBE_AIR;

	if (iabs(p->e.groundVel) < 0x10000)
		p->e.groundVel = dir ? -PHYS_TUBE_LAUNCH_SPEED : PHYS_TUBE_LAUNCH_SPEED;
}

/* ForceSpin_Update (ForceSpin.c:12-53), one player (foreach_active(Player,
 * player) only ever visits the one entity this port has), all ForceSpin
 * entities collapsed to this static table looped every frame rather than
 * scene objects with their own per-entity Update call. The rotated velocity
 * copy (pivotVel, ForceSpin.c:19/22) is not computed here: in the original
 * it is written and never read again by anything in the function (only
 * pivotPos feeds the containment/side test below), so it is a no-op even in
 * the source this ports from -- not a simplification specific to this port's
 * configuration, unlike the invertGravity-gated drops elsewhere in this
 * task, so nothing is lost by skipping the computation entirely here. */
void force_spin_apply(Player *p)
{
	uint32_t i;

	for (i = 0; i < MARKER_COUNT; i++) {
		const ForceSpinDef *m = &k_markers[i];
		int32_t mx = TO_FIXED(m->x);
		int32_t my = TO_FIXED(m->y);
		int32_t rx = p->e.x;
		int32_t ry = p->e.y;
		uint8_t negAngle = (uint8_t)(0 - m->angle);   /* ForceSpin_Create:75 */

		rotate_on_pivot(&rx, &ry, mx, my, negAngle);

		if (iabs(rx - mx) >= TO_FIXED(24)) continue;
		if (iabs(ry - my) >= TO_FIXED(m->halfLen)) continue;

		/* ForceSpin.c:24-49's nested "if (rx>=x) { if(dir) RELEASE; else
		 * FORCE; } else { if(!dir) RELEASE; else FORCE; }" collapses to one
		 * boolean: FORCE fires on (rx>=mx,dir==0) or (rx<mx,dir==1), which is
		 * exactly "the side test agrees with dir==0" -- both truth values
		 * flip together, so a plain == captures both original branches. */
		if ((rx >= mx) == (m->dir == 0)) {
			set_tube_state(p, m->dir);
		} else if (p->state == PSTATE_TUBE_ROLL || p->state == PSTATE_TUBE_AIR) {
			p->state = p->e.onGround ? PSTATE_ROLL : PSTATE_NORMAL;
		}
	}
}
