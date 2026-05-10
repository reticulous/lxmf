/**
 * lxmf — LXMF messaging protocol task.
 *
 * Sits on top of rnsd, consumed via ITS (RNSD_PORT_DEST for destination
 * binding, RNSD_PORT_DGRAM for opportunistic datagrams). Owns the inbox,
 * contacts, dedup, send queue.
 *
 * See docs/component-plan.md §4 / §9.
 */
#pragma once

/** Bring up the lxmf task. */
void lxmfInit(void);
