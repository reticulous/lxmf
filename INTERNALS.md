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
