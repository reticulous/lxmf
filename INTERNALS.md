# lxmf — internals

## Task

One FreeRTOS task on **core 1, prio 1, 8 KB PSRAM stack**. Sits on top
of rnsd's byte-array API plus the mailbox and announce-fanout ITS ports.

**Zero mR includes.** mR is an implementation detail of rnsd; lxmf
operates on raw bytes and storage sentinels.

## Storage-as-API

The lxmf task subscribes only to:

- `lxmf.cmd.*` — top-level commands (e.g. `lxmf.cmd.send`).
- `lxmf.id.<n>.cmd.*` — per-identity commands.

Each sentinel is consumed inline. Frontends (browser, CLI, LCD)
write sentinels and watch the resulting state changes flow back
through the storage tree.

Read-only state surfaces:

- `s.lxmf.identities.*` — public per-identity material.
- `secrets.lxmf.identities.*` — private keys (never to browser).
- `lxmf.directory.<32hex>.*` — known peers.
- `lxmf.threads.<peerhash>.*` — message thread metadata.
- `lxmf.messages.<id>` — message bodies (ephemeral; backed by files for
  large content).
- `lxmf.cmd.*.result` — async command results.

This is the single shape the chat UI, the CLI, and the on-device
LXMessenger all read.

## Identities

Multi-identity by design. Each identity has:

- A keypair (Ed25519 sign + X25519 ECDH).
- A display name.
- A set of subscribed announces.

No auto-create at boot. A device with no identities runs as a transport-
only node — it routes packets but cannot send messages.

`lxmf identity create [name]` produces a new identity; the keypair is
generated locally and stored under `secrets.lxmf.identities.<n>.*`.

## Message paths

- **Opportunistic SINGLE** — single-packet delivery used when the
  destination is reachable in one hop and the payload fits. Uses
  X25519 ECDH for forward secrecy.
- **Link + Resource** — for larger payloads. Negotiates a Link, then
  sends a Resource. Both go through rnsd's typed conn-openers
  (`rnsdLinkOpen`, `rnsdLinkRequest`).

## Delivery proofs (stage `sent` → `delivered`)

`sent` means egressed / transfer accepted; `delivered` means a
cryptographic delivery proof (or the proof-grade Resource ACK) came
back. A proof timeout leaves the stage at `sent` with
`last_error = "no delivery proof"` — never `failed` (the message may
have arrived). How each path settles:

- **Opportunistic** — rnsd emits `OUT_RESULT` twice per `send_id`:
  `SENT` immediately on egress, then `DELIVERED` or `PROOF_TIMEOUT`
  when the packet receipt concludes. The outbox slot is held (still
  counted against the 8-slot table) until the second result;
  `resolveDirectSends` carries a 90 s backstop in case it's lost.
- **DIRECT, one Link packet** — `resolveDirectSends` writes `sent` on
  egress (link active / `tx_packets` ≥ 1) as before, then keeps the
  slot and watches `rnsd.links.<tag>.tx_proven` / `.proof_timeouts`
  against baselines captured at send time. An increment of `tx_proven`
  settles `delivered`; a `proof_timeouts` increment, link death, or
  the 60 s deadline settles no-proof. Holding the slot keeps
  `convBusy()` true, so sends stay serialized per link — that is what
  makes the per-link counters unambiguous.
- **DIRECT, Resource** — the transfer ACK
  (`RNSD_LINK_RESOURCE_OUTBOUND_DONE`, or the
  `resource.state == "sent"` fallback) is proof-grade → stage
  `delivered` directly.

The conversation link is kept on success / proof timeout and dropped
only on real failures, exactly as before.

## UI glyphs (outbound bubbles, both UIs)

| stage               | glyph                  |
| ------------------- | ---------------------- |
| `queued`/`sending`  | small grey `…`         |
| `sent`              | one grey checkmark     |
| `delivered`         | two green checkmarks   |
| `failed`/`cancelled`| red ✕                  |

Web (`MessageBubble.vue`) shows `last_error` as the glyph tooltip; the
LCD slice (`lxmf_lcd.cpp` `addBubble`) uses `LV_SYMBOL_OK` /
`LV_SYMBOL_CLOSE` from the chrome font and re-renders on stage changes
via its `s.lxmf.id` storage subscription.

## LCD slice

`esp-idf/lcd/src/lxmf_lcd.cpp` is the on-device LXMessenger program.
Picked up by the `spangap-lcd` activator, folded into this straddle's
component, registered with `lcdRegister("LXMessenger", "msg",
lxmfLcdRun)`.

The slice's body is wrapped in `#if CONFIG_SPANGAP_LCD` so the
straddle compiles cleanly without `spangap-lcd` in the graph.

## CLI surface

- `lxmf identity create [name]`
- `lxmf identity ls`
- `lxmf send <peerhash> <msg>`
- `lxmf threads`
- `lxmf peers`
- `lxmf inbox`

## Activator caveat

The LCD slice is hard-coded into the firmware `CMakeLists.txt` for now —
the activator-driven source-list exclusion is a future build-CLI
feature, so there's no manifest field to declare it yet. The slice's
`.cpp` is wrapped in `#if CONFIG_SPANGAP_LCD` until then.

## Status

LXMF 0.9.8, hardware-verified against upstream Python LXMF. Per-
contact threads, Link + Resource transfer working. Full storage-key
schema documented in
[hw-tdeck/docs/internals/lxmf.md](../hw-tdeck/docs/internals/lxmf.md).
