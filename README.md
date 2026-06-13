# lxmf

## What is this?

**lxmf** is LXMF messaging on
[rns](../rns): multi-identity opportunistic
single-packet delivery plus Link + Resource transfer for larger
messages. DIRECT delivery uses **persistent per-peer conversation
Links** (upstream-LXMF style): the first direct send to a peer opens a
Link that is then kept and reused for the whole chat — for our sends
and for the peer's deliveries back over it — and left open (set
`s.lxmf.link.idle_s` to close idle links after that many seconds;
default 0 = never — LRU eviction at the 4-link cap and Reticulum's own
STALE teardown bound the cost). While a
conversation Link is warm, even small messages ride it instead of going
opportunistic. Storage is the API — every frontend (browser, CLI,
on-device LCD) reads/writes the `s.lxmf.*` / `lxmf.*` / `secrets.lxmf.*`
keys, and the lxmf task subscribes to its own subtree. No frontend ever
talks to lxmf via ITS directly.

[LXMF](https://github.com/markqvist/LXMF) is Mark Qvist's
"Lightweight Extensible Message Format" — the canonical
human-messaging layer on Reticulum.

## What this straddle owns

```
lxmf/
├── esp-idf/
│   ├── include/lxmf.h
│   ├── src/lxmf.cpp         the lxmf task: multi-identity, mailbox, peers
│   └── lcd/src/lxmf_lcd.cpp  on-device LXMessenger LVGL program (slice)
└── browser/
    └── src/
        ├── modules/lxmf.ts             Pinia + RPC
        ├── panels/LxmfPanel.vue        Settings → LXMF
        ├── panels/MessagesWindow.vue   chat window (FloatingWindow)
        ├── panels/AnnouncesWindow.vue  live announce feed
        └── components/lxmf/            ConversationList, Composer,
                                        MessageBubble, PeerPicker, …
```

The LCD slice (`esp-idf/lcd/src/lxmf_lcd.cpp`) is the on-device
LXMessenger program — picked up by the [spangap-lcd](../../s/spangap-lcd)
activator and wrapped in `#if CONFIG_SPANGAP_LCD`.

## How others use it

```cpp
lxmfInit();    // after rnsdInit
```

The **API is storage**. Frontends:

- Read peer / message lists from `lxmf.directory.<32hex>.<field>`,
  `lxmf.threads.<peerhash>.*`.
- Write commands to `lxmf.cmd.*` and `lxmf.id.<n>.cmd.*` sentinels;
  lxmf subscribes only to those subtrees and processes each sentinel
  inline.
- Identities are under `s.lxmf.identities.*` (public) and
  `secrets.lxmf.identities.*` (private keys).

**No auto-create at boot.** Devices without identities run as transport-
only nodes. An identity is created explicitly via `lxmf identity
create` (CLI), or via the Settings panel.

## Message stages & delivery proofs

Outbound message records (`s.lxmf.id.<n>.msgs.<peer>.<key>.stage`) move
through:

| stage       | meaning                                                        |
| ----------- | -------------------------------------------------------------- |
| `draft`     | composed, not yet handed to the task                           |
| `queued`    | accepted; waiting (e.g. for a busy conversation link)          |
| `sending`   | in flight (path request, link establishment, transfer)         |
| `sent`      | **egressed / transfer accepted** — no proof of arrival (yet)   |
| `delivered` | **cryptographic delivery proof received**, or the Resource transfer ACK (which is proof-grade) |
| `failed`    | terminal error (`last_error` says why)                         |
| `cancelled` | user cancelled                                                 |
| `received`  | inbound messages                                               |

A proof timeout is **not** a failure: the message may well have arrived
(the peer may not prove incoming packets, or the proof was lost). The
stage stays `sent` and `last_error` is set to `no delivery proof`.

Both UIs render the stage on outbound bubbles:

- `queued` / `sending` — small grey `…`
- `sent` — one grey checkmark
- `delivered` — two green checkmarks
- `failed` / `cancelled` — red ✕ (web shows `last_error` as tooltip)

## Dependencies

- [rns](../rns)
- (transports are decoupled — pick whichever ones suit the deployment)

## Read next

- [INTERNALS.md](INTERNALS.md) — task layout, sentinel cmd surface,
  storage shape, multi-identity model.
- Deep-dive in the consuming app:
  [docs/lxmf.md](../hw-tdeck/docs/lxmf.md) — black-box view;
  [docs/internals/lxmf.md](../hw-tdeck/docs/internals/lxmf.md)
  — the reach-inside view (upstream LXMF 0.9.8 summary, our deltas,
  full schema, phasing).
