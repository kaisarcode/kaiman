# Configuration

The default file is `$XDG_CONFIG_HOME/kaiman/services.ini`, or
`$HOME/.config/kaiman/services.ini`. It is written atomically with restricted
permissions. The GUI stores API keys and passwords using the system credential
store: Credential Manager on Windows, Keychain on Apple platforms, and Secret
Service through the optional system libsecret on desktop Linux. The configuration
contains only a credential reference. If secure storage is unavailable, saving
a new credential fails. Existing environment and command sources remain usable
through the library and CLI. Editing a service without entering a new secret
preserves its existing credential when the authentication scheme is unchanged.
New services use `scheme=none` until a credential scheme is selected explicitly.

```ini
[gateway]
listen=127.0.0.1:49380

[service.local-opencode]
display-name=Local OpenCode
adapter=opencode-local
url=
scheme=none
timeout=60

[service.local-codex]
display-name=Local Codex
adapter=codex-local
url=
scheme=none
timeout=60
```

The `[service.<slug>]` section name is the stable identifier used by the CLI
and API. `display-name` is the human readable name shown by the GUI. The GUI
generates the slug from that name unless the slug field is edited explicitly,
and adds a numeric suffix when the slug is already in use.

Persisted OpenCode session state and the detached gateway PID live under
`$XDG_STATE_HOME/kaiman`, or `$HOME/.local/state/kaiman`.

See [Providers](providers.md) for default URLs and automatic local connections.
