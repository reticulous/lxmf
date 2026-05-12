/**
 * lxmf — LXMF messaging protocol task.
 *
 * Phase 4a scope: single-identity, opportunistic-only. Identity bootstrap
 * on first run; storage-driven lifecycle for outbound (client writes
 * stage=draft → ready, firmware packs/signs/queues/sends); inbound
 * verification + dedup + storage write.
 *
 * Storage is the API — no DataChannel, no ITS port for clients. Every
 * frontend (browser, CLI, on-device UI) reads/writes the `s.lxmf.*` /
 * `lxmf.*` / `secrets.lxmf.*` keys; the lxmf task subscribes to its own
 * subtree and reacts.
 *
 * Consumer of rnsd over RNSD_PORT_DEST: one mailbox connection per
 * identity. See docs/plans/lxmf.md and docs/component-plan.md §15.
 */
#pragma once

/** Bring up the lxmf task. The task is always started — even on a
 *  transport-only node — so the announce-fanout subscription and the
 *  cmd handlers are live. With zero identities allocated it simply
 *  never announces an LXMF mailbox of its own. */
void lxmfInit(void);

/** Create a new LXMF identity with the given display name.
 *
 *  Both modes write the `lxmf.cmd.identity_new` sentinel; the lxmf
 *  task's storage subscription processes it on its own task. Sync
 *  mode then waits (vTaskDelay-polls storage) for the sentinel to
 *  clear before returning. The detailed error reason — if any — lives
 *  in the [lxmf] err()/warn() log on the lxmf task.
 *
 *  Returns:
 *    sync=false : 0 on validation success, -1 if display_name is empty
 *    sync=true  : allocated slot index (0..LXMF_MAX_IDENTITIES-1) on
 *                 success, -1 on validation error, lxmf-side failure,
 *                 or 5 s timeout */
int lxmfCreateIdentity(const char* display_name, bool sync = false);

/** Destroy the identity at slot `n`. Wipes secrets.lxmf.id.<n>.privkey
 *  + every s.lxmf.id.<n>.* and lxmf.id.<n>.* storage key, closes the
 *  mailbox, unsubscribes the per-id cmd handler.
 *
 *  Sync mode polls storage until secrets disappear (or 5 s timeout).
 *  Returns true on success, false on validation error / timeout /
 *  lxmf-side failure. */
bool lxmfDestroyIdentity(int n, bool sync = false);
