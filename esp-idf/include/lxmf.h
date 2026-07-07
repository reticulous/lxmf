/**
 * lxmf — LXMF messaging protocol task.
 *
 * Multi-identity (slots 0..LXMF_MAX_IDENTITIES-1); opportunistic single-packet
 * delivery plus on-demand Link + Resource transfer for larger messages. No
 * auto-create at boot — identities are created explicitly (lxmfCreateIdentity),
 * so a node with none runs transport-only. Storage-driven lifecycle for
 * outbound (client writes stage=draft → ready, firmware packs/signs/queues/
 * sends); inbound verification + dedup + storage write.
 *
 * Storage is the API — no DataChannel, no ITS port for clients. Every
 * frontend (browser, CLI, on-device UI) reads/writes the `s.lxmf.*` /
 * `lxmf.*` / `secrets.lxmf.*` keys; the lxmf task subscribes to its own
 * subtree and reacts.
 *
 * Consumer of rnsd over RNSD_PORT_DEST: one hosted-destination (our-dest) connection per
 * identity.
 */
#pragma once

#include "service.h"

/** Bring up the lxmf task. The task is always started — even on a
 *  transport-only node — so the announce-fanout subscription and the
 *  cmd handlers are live. With zero identities allocated it simply
 *  never announces an LXMF destination of its own. */
class LxmfService : public Service {
public:
    void onInit() override;
};

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
 *  our-dest, unsubscribes the per-id cmd handler.
 *
 *  Sync mode polls storage until secrets disappear (or 5 s timeout).
 *  Returns true on success, false on validation error / timeout /
 *  lxmf-side failure. */
bool lxmfDestroyIdentity(int n, bool sync = false);
