# License

This repository, **reticulous-lxmf** (LXMF messaging on spangap;
multi-identity, opportunistic + Link + Resource delivery, storage-as-API), is
released under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml` and `browser/package.json`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |
| Browser peer deps (Vue, Quasar, Pinia, vue-router) | npm | MIT |

LXMF is a protocol developed by Mark Qvist (`markqvist/LXMF`, MIT).
This implementation is independent; no LXMF source code is incorporated
from the upstream reference.
