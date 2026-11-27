# kaiman

kaiman is an embeddable C library and one standalone CLI gateway for saved AI
services. It keeps one configuration for several projects, stores credential
references instead of secrets, and exposes configured services through one
OpenAI-compatible HTTP endpoint.

The library is the product. The CLI is a thin frontend built on the same public
API, so another application can manage services without spawning a process.

## Quick start

Configure the installed OpenCode or Codex client:

```sh
kaiman add local-opencode adapter=opencode-local
kaiman add local-codex adapter=codex-local
kaiman start

curl http://127.0.0.1:49380/v1/providers
curl http://127.0.0.1:49380/v1/models
curl http://127.0.0.1:49380/v1/sessions
```

Use `kaiman gui` to manage providers and the gateway from the desktop.

## Documentation

- [Public API](doc/api.md) - C and JavaScript configuration interfaces
- [CLI](doc/cli.md) - command-line and desktop usage
- [HTTP API](doc/http-api.md) - gateway endpoints and model discovery
- [Sessions](doc/sessions.md) - discovery, creation, forks and continuation
- [Providers](doc/providers.md) - supported adapters and default URLs
- [Configuration](doc/configuration.md) - persistence and credentials

## Dependencies

Dependencies are vendored under `lib/kaisarcode`, following the embdex
mechanism: `libhttp.c`, `libnetl.c`, `libtls.c` and `libwvw.c`. No runtime
dependency is installed system-wide. The shared library loads `libtls` from its
own directory; the static CLI links `libtls` statically. The GUI links `libwvw`
statically into the CLI only for `linux/x86_64`, `windows/x86_64`,
`macos/x86_64` and `macos/aarch64`.

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
produces one CLI executable and the embeddable library. GUI targets also
produce a `gui/` asset directory beside the executable. WASM is not offered
because `libnetl.c` does not support it.

Use `make clean` only when a full rebuild is required.

## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
