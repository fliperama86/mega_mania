#ifndef __CD_H__
#define __CD_H__

/* Mega CD Mode 1 bring-up over the 32X cart adapter. Only the 68000 can
 * reach the CD hardware, so the whole detect/reset/handshake sequence lives
 * here; the SH-2s stay idle. Every step reports through cd_print() (see
 * text.h). Never returns: ends either in a harmless idle loop (no CD found)
 * or the steady-state re-ping loop (step 10 of the spec). */
void cd_run(void);

#endif
