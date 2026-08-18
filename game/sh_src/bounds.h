#ifndef BOUNDS_H
#define BOUNDS_H

/* Zone's world bounds (Zone->cameraBoundsB/T/L/R and playerBoundsB/T/L/R,
 * declared at Zone.c ~913-920), narrowed to what GHZ Act 1 actually uses:
 * L/R never move after stage load (BoundsMarker only ever writes T/B, and
 * GHZSetup_HandleActTransition's left-bound rewrite, GHZSetup.c:148, is
 * gated to Act 2 only by "if (Zone->actID)" at GHZSetup.c:57), so they are
 * computed once by bounds_init and read as plain constants thereafter. T/B
 * are re-derived every frame by bounds_apply_markers, same as the
 * original's BoundsMarker_Update (BoundsMarker.c:12-20) feeding
 * Zone->cameraBoundsT/B and Zone->playerBoundsT/B.
 *
 * deathBoundsB (Zone->deathBoundary, Zone.c:232: `TO_FIXED(Zone->
 * cameraBoundsB[s])`) is set once, here, at stage load and never touched
 * again -- k_markers below only ever writes playerBoundsB/cameraBoundsB
 * (BoundsMarker_Update, Collision.cpp -- the original never routes a
 * BoundsMarker write through deathBoundary either), matching the original's
 * own deathBoundary field, which BoundsMarker_ApplyBounds never assigns. */
typedef struct {
	int32_t cameraBoundsL, cameraBoundsR; /* px, constant for the act */
	int32_t cameraBoundsT, cameraBoundsB; /* px, what camera_apply_vbounds
	                                       * eases the camera's own bounds
	                                       * toward */
	int32_t playerBoundsL, playerBoundsR; /* 16.16, constant for the act */
	int32_t playerBoundsT, playerBoundsB; /* 16.16, fed to
	                                       * player_apply_world_bounds. T is
	                                       * maintained (BOUNDSMARKER_BELOW_Y
	                                       * writes it) but never consumed:
	                                       * Zone_StageLoad leaves
	                                       * playerBoundActiveT off and
	                                       * nothing in GHZ1 turns it on, and
	                                       * this port's player_apply_world_
	                                       * bounds has no top case either
	                                       * (see player.h). */
	int32_t deathBoundsB;                 /* 16.16, constant for the act --
	                                       * see this struct's own comment
	                                       * above and player.c's death-
	                                       * boundary comment for why a
	                                       * single constant is provably
	                                       * enough for this act's own
	                                       * bounds.c marker table. */
} ZoneBounds;

/* Zone_StageLoad's defaults (Zone.c:221-235): L=0, T=0, R/B from the FG Low
 * layer size in px (g_map_w/g_map_h are in 16px blocks, assets.h, so *16
 * converts to px). Then runs one marker pass against (playerX, playerY),
 * mirroring BoundsMarker_Create's setPos=true pass (BoundsMarker.c:28-47):
 * every marker already in the scene applies once before the first frame, so
 * the act starts pinned by whichever markers cover the spawn rather than
 * opening on the full-layer default. playerX/playerY are 16.16 world
 * pixels, matching EntityPlayer::position's scale. */
void bounds_init(ZoneBounds *z, int32_t playerX, int32_t playerY);

/* BoundsMarker_Update (BoundsMarker.c:12-20), the per-frame pass: every
 * marker in the scene's slot order (bounds.c's table) is re-checked against
 * the player's current position and, if in range, overwrites the T or B
 * pair it owns. Later slots win where two active markers disagree, matching
 * entity slot order (later slot's BoundsMarker_ApplyBounds call simply runs
 * after and clobbers the earlier write). playerX/playerY are 16.16 world
 * pixels. */
void bounds_apply_markers(ZoneBounds *z, int32_t playerX, int32_t playerY);

#endif
