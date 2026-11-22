# kaiman

kaiman is an embeddable C library and command line gateway for saved AI
services. It keeps one configuration for several projects, stores service
references instead of secrets, and will expose those services through one
OpenAI compatible address.

The library is the product. The CLI is a frontend built on the same public
API, so another application can manage services without spawning a process.

## API intention

Include `src/libkaiman.h` and link `libkaiman`. The API manages services as
objects with behavior, not as indexed records:

- `kc_kaiman_open()` opens a manager with a path and listen address.
- `kc_kaiman_get_services()` borrows all managed services.
- `kc_kaiman_get_service()` finds one service by name.
- `kc_kaiman_add_service()` creates a service from a public descriptor.
- `kc_kaiman_listen()` changes the gateway address.
- `kc_kaiman_service_set()`, `kc_kaiman_service_get()` and
    `kc_kaiman_service_remove()` operate on one service.
- `kc_kaiman_close()` releases the manager and every service it owns.

Every mutation persists immediately. Status codes follow the kclib convention:
`KC_KAIMAN_OK`, `KC_KAIMAN_NOT_FOUND`, and negative error codes. Handles are
borrowed. A removed service handle stays allocated but becomes invalid.

## JavaScript projection

`src/libkaiman.js` exposes the same intention to JavaScript:

```js
import { kaiman } from "./libkaiman.js";

var kmn = kaiman.open({ path: "./services.ini" });
var services = kmn.getServices();
var myserv1 = services[0];
var myserv2 = kmn.getService("local-opencode");

myserv1.set("name", "myserv1");
myserv1.set("model", "opencode/big-pickle");

const myserv3 = kmn.addService({
  name: "openai",
  adapter: "openai",
  url: "https://api.openai.com/v1",
  apiKeyEnv: "OPENAI_API_KEY",
  model: "gpt-5"
});

kmn.listen("127.0.0.1:10000");
myserv3.get("model");
myserv1.remove();
kmn.close();
```

The projection requires a configuration-only WASM build. The native gateway
dependency `libnetl.c` has no WASM target, so no WASM artifact is produced by
this project yet.

## CLI

```sh
kaiman -h
kaiman --help
kaiman -v
kaiman --version
kaiman service add local-opencode adapter=opencode \
  url=http://127.0.0.1:49374 model=opencode/big-pickle \
  scheme=basic user=opencode token-command="opencode service get password"
kaiman service list
kaiman service edit local-opencode model=opencode/big-pickle
kaiman service remove local-opencode
kaiman serve --listen 127.0.0.1:49380
```

The gateway transport is not enabled yet. `serve` validates the configuration
and prints the resolved listen address.

## Configuration

The default file is `$XDG_CONFIG_HOME/kaiman/services.ini`, or
`$HOME/.config/kaiman/services.ini`. It is written atomically with restricted
permissions. Tokens are never stored: a service keeps an environment variable
name or a command that prints the token.

```ini
[gateway]
listen=127.0.0.1:49380

[service.local-opencode]
adapter=opencode
url=http://127.0.0.1:49374
model=opencode/big-pickle
env=
token-command=opencode service get password
scheme=basic
user=opencode
timeout=60
```

## Dependencies

Dependencies are vendored under `lib/kaisarcode`, following the embdex
mechanism: `libhttp.c`, `libkv.c` and `libnetl.c`. No runtime dependency is
installed system-wide.

## Build

```sh
kcs
make
make test
make test wine
```

`make` builds the native target and its dependencies. `make test` uses those
artifacts without rebuilding them. For Wine, run `make windows/x86_64` first.
`make all` builds every target supported by all dependencies. WASM is not
offered because `libnetl.c` does not support it.

Use `make clean` only when a full rebuild is required.

## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
