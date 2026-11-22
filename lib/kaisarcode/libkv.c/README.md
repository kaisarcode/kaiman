# libkv.c - In-memory key-value collection

`libkv.c` manages binary values addressed by unique string keys. It supports
insertion, lookup, replacement, deletion and ordered iteration without a
server or filesystem.

The collection owns its entries in memory. It can export a portable binary
snapshot and restore that snapshot later. The library operates entirely in
memory and leaves persistence to the application.

## Quick start

```c
#include "libkv.h"
#include <stdio.h>

kc_kv_t *store = NULL;

if (kc_kv_open(&store, NULL) == KC_KV_OK) {
    const void *value = NULL;
    size_t size = 0;

    kc_kv_set(store, "theme", "dark", 4);
    if (kc_kv_get(store, "theme", &value, &size) == KC_KV_OK)
        printf("%.*s\n", (int)size, (const char *)value);
    kc_kv_close(store);
}
```

Keys are nonempty NUL-terminated strings compared bytewise and case-sensitively.
Values are arbitrary binary bytes and may contain NUL. A present, zero-length
value is distinct from a missing key. `set()` copies key and value bytes; `get()`
and `at()` borrow bytes from the collection. Replacing or deleting an entry
invalidates pointers borrowed from that entry, and closing the collection
invalidates them all.

`at()` enumerates entries in ascending key order. Lookup uses binary search;
insertions and deletions may move entries in the sorted collection. This
implementation favors small, inspectable in-memory datasets over large mutable
databases.

## Binary snapshots

```c
void *bytes = NULL;
size_t length = 0;

if (kc_kv_export(store, &bytes, &length) == KC_KV_OK) {
    kc_kv_options_t options = {.data = bytes, .size = length};
    kc_kv_t *restored = NULL;

    if (kc_kv_open(&restored, &options) == KC_KV_OK)
        kc_kv_close(restored);
}
```

`kc_kv_open()` restores a new collection from exported bytes without
requiring the source collection. Exported memory belongs to the original
collection until its next export or close; no manual release is required. Snapshots preserve binary values and
are independent of host pointer sizes and endianness. Restoration validates record
boundaries, duplicate or unordered keys, and trailing data. Snapshot export is
not a filesystem transaction: atomic persistence, synchronization and recovery
belong to the application.

## CLI

The `kv` command projects the same collection operations into composable
binary streams. It does not store a collection or choose its persistence medium.
`new`, `set` and `del` write a complete portable binary collection to stdout;
`get` and `at` write the selected value bytes. `count` and `list` print text.
Every operation except `new` reads the collection from stdin.

```sh
kv new | kv set language es | kv set theme dark > config.kv
kv get language < config.kv
kv count < config.kv
kv list < config.kv
kv at 0 < config.kv
kv del theme < config.kv > updated.kv
```

Values are binary, including embedded NUL and empty values. `set <key> <text>`
accepts a text argument; `set <key> --value-file <path>` reads the exact bytes
from a file instead:

```sh
kv set picture --value-file image.png < config.kv > updated.kv
kv get picture < updated.kv > restored.png
```

`--value-file` uses a separate input because stdin is reserved for the binary
collection. Keys are nonempty strings. `list` escapes backslashes, newlines,
tabs, carriage returns and control bytes (`\xHH`) so every key occupies a
single line. `at` uses zero-based sorted key order. A missing key, malformed
collection or invalid argument produces a nonzero exit status.

The binary output may be held in memory, sent to another program, or persisted
externally. The CLI does not write collection files itself.

## JavaScript

The WebAssembly target provides `libkv.js`, `libkv.mjs` and `libkv.wasm`.
Values and snapshots are `Uint8Array`, and returned values are copied.

```js
import {kv} from "./libkv.js";

const store = kv.open();
store.set("theme", new TextEncoder().encode("dark"));
const bytes = store.export();
const restored = kv.open(bytes);
console.log(new TextDecoder().decode(restored.get("theme")));
restored.close();
store.close();
```

## Build

From this project directory:

```sh
make
make test
```

Use `make all` to build supported targets when the required toolchains are
available. `make test wasm` runs C and JavaScript contracts under Emscripten
and Node.js, with one numbered report and one aggregate summary.

## Notice

No guarantees are provided regarding stability or future support. You may test,
use, and modify it.

Questions can be sent to [kaisar@kaisarcode.com](mailto:kaisar@kaisarcode.com).
## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
