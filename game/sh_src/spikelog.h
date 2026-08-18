#ifndef SPIKELOG_H
#define SPIKELOG_H

#include "player.h"

/* SpikeLog, ported from SonicMania/Objects/GHZ/SpikeLog.c: GHZ1's 61
 * rotating log hazards, applied every frame in slot order after
 * player_update() has settled this frame's position.
 *
 * COMPROMISE, stated up front: SpikeLog_State_Main's `player->shield !=
 * SHIELD_FIRE` branch (SpikeLog.c:65-100, hurt-on-touch during the danger
 * window) is the only one transcribed here. The `else` branch (:101-112,
 * player carrying SHIELD_FIRE burns the log, chains to touching neighbours
 * via SpikeLog_State_Burn, unlocks the GHZ achievement) is PERMANENTLY
 * INERT in this port: there is no shield system at all (player.h's own
 * top comment -- "no shield/star-power invincibleTimer... not ported, no
 * shields exist"), so `player->shield != SHIELD_FIRE` is unconditionally
 * true and the burn/chain mechanic can never fire. Not approximated with
 * some other trigger -- flagged, not silently dropped. */
void spikelog_apply(Player *p);

#endif
