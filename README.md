# kaiman

kaiman is a unified AI service manager and gateway. Connect local and remote
AI services, manage them from a desktop interface or the command line, and
access their models through a single OpenAI-compatible API.

![kaiman desktop interface](screenshot.png)

## Quick start

Connect an installed OpenCode client and start the gateway:

```sh
kaiman add local-opencode adapter=opencode-local
kaiman start
curl http://127.0.0.1:49380/v1/models
```

Run `kaiman gui` to manage services from the desktop. To install the desktop
application, run `installer` on Linux or `installer.exe` on Windows.

## Documentation

- [CLI](doc/cli.md) - command-line and desktop usage
- [Providers](doc/providers.md) - supported AI services
- [Configuration](doc/configuration.md) - settings and credentials
- [Sessions](doc/sessions.md) - persistent conversations
- [HTTP API](doc/http-api.md) - application integration
- [Public API](doc/api.md) - C and JavaScript integration
- [Build](doc/build.md) - build targets, dependencies and tests

## Build

```sh
make
make test
```

## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
