# lxmf

## What is this?

**lxmf** is LXMF messaging on
[rns](../rns): multi-identity opportunistic
single-packet delivery plus on-demand Link + Resource transfer for
larger messages. Storage is the API — every frontend (browser, CLI,
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
