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

```ini
[gateway]
listen=127.0.0.1:49380

[service.local-opencode]
adapter=opencode-local
url=
scheme=none
timeout=60

[service.local-codex]
adapter=codex-local
url=
scheme=none
timeout=60
```

Persisted OpenCode session state and the detached gateway PID live under
`$XDG_STATE_HOME/kaiman`, or `$HOME/.local/state/kaiman`.

See [Providers](providers.md) for default URLs and automatic local connections.
