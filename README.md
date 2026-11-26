# kaiman

kaiman is an embeddable C library and one standalone CLI gateway for saved AI
services. It keeps one configuration for several projects, stores credential
references instead of secrets, and exposes configured services through one
OpenAI-compatible HTTP endpoint.

The library is the product. The CLI is a thin frontend built on the same public
API, so another application can manage services without spawning a process.

## Library API

Include `src/libkaiman.h` and link `libkaiman`.

- `kc_kaiman_open()`, `kc_kaiman_load()` and `kc_kaiman_close()` manage the
    configuration lifetime.
- `kc_kaiman_get_services()`, `kc_kaiman_get_service()`,
    `kc_kaiman_add_service()`, `kc_kaiman_service_set()`,
    `kc_kaiman_service_get()`, `kc_kaiman_service_timeout()` and
    `kc_kaiman_service_remove()` manage saved services.
- `kc_kaiman_listen()`, `kc_kaiman_address()` and `kc_kaiman_path()` inspect or
    change gateway settings.
- `kc_kaiman_gateway_open()`, `kc_kaiman_gateway_run()`,
    `kc_kaiman_gateway_stop()`, `kc_kaiman_gateway_address()` and
    `kc_kaiman_gateway_close()` run the HTTP gateway in the current process.
- `kc_kaiman_start()`, `kc_kaiman_stop()` and `kc_kaiman_status()` manage the
    detached gateway process used by the CLI.
- `kc_kaiman_strerror()`, `kc_kaiman_error()` and `kc_kaiman_version()` report
    status and version information.

Every mutation persists immediately. Status codes follow the kclib convention:
`KC_KAIMAN_OK`, `KC_KAIMAN_NOT_FOUND`, and negative error codes. Handles are
borrowed. A removed service handle stays allocated but becomes invalid.

A service descriptor accepts a unique `name`, an `adapter` (`openai` or
`opencode`), an `http://` or `https://` URL, an upstream `model`, an optional
credential reference (`env` or `token-command`), an `auth` scheme (`none`,
`bearer` or `basic`), an optional `user`, and a `timeout` from 1 to 600
seconds. Credentials themselves are never stored.

## JavaScript projection

`src/libkaiman.js` exposes the configuration-level API to JavaScript:

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

The projection is configuration-only. No WASM artifact is produced because
`libnetl.c` has no WASM target.

## CLI

```sh
kaiman -h
kaiman --help
kaiman -v
kaiman --version

kaiman add local-opencode adapter=opencode \
  url=http://127.0.0.1:49374 model=opencode/big-pickle \
  scheme=basic user=opencode token-command="opencode service get password"
kaiman list
kaiman edit local-opencode model=opencode/big-pickle
kaiman remove local-opencode

kaiman start
kaiman status
kaiman stop
```

`add` and `edit` accept the service keys `adapter`, `url`, `model`, `env`,
`token-command`, `scheme`, `user` and `timeout`. `start` launches the gateway
as a detached process, `status` prints its state and address, and `stop`
terminates it.

## HTTP gateway

The gateway listens on `127.0.0.1:49380` by default. Change `listen` in the
`[gateway]` section of the configuration file to use a different host or port.

| Method | Path | Behavior |
| --- | --- | --- |
| `GET` | `/health` | Returns `{"status":"ok"}`. |
| `GET` | `/v1/models` | Lists configured services as OpenAI-compatible models. |
| `POST` | `/v1/chat/completions` | Selects a service by `model` and forwards the chat request. |

A chat request needs an object body with a `model` that matches a configured
service name and a `messages` array. Other JSON fields are passed through.
`stream:true` returns a chunked SSE response with `data:` frames and a final
`data: [DONE]` frame.

Invalid JSON, missing credentials or an unusable request returns `400`. An
unknown path or unknown model returns `404`; an oversized body returns `413`;
and an unreachable or malformed upstream returns `502`. Upstream responses with
status `400` or higher are propagated with their status, content type and body.

Two adapters are supported:

- `openai` forwards chat requests to an OpenAI-compatible `/chat/completions`
    endpoint, replacing the request `model` with the configured upstream model
    and rewriting response models back to the requested name.
- `opencode` uses the local OpenCode API. Its model must be
    `providerID/modelID`. It reuses one persisted session per upstream server,
    keeps a bounded conversation history, and serializes requests that share
    that session. `stream:true` emits the completed reply as SSE chunks.

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

Persisted OpenCode session state and the detached gateway PID live under
`$XDG_STATE_HOME/kaiman`, or `$HOME/.local/state/kaiman`.

## Dependencies

Dependencies are vendored under `lib/kaisarcode`, following the embdex
mechanism: `libhttp.c`, `libnetl.c` and `libtls.c`. No runtime dependency is
installed system-wide. The shared library loads `libtls` from its own directory;
the static CLI links `libtls` statically.

## Build

```sh
kcs
make
make test
make test wine
```

`make` builds the native target and its dependencies. `make test` uses those
artifacts without rebuilding them. For Wine, run `make windows/x86_64` first.
`make all` builds every target supported by all dependencies. Each target
produces one CLI executable and the embeddable library. WASM is not offered
because `libnetl.c` does not support it.

Use `make clean` only when a full rebuild is required.

## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
