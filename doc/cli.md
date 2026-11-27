# CLI

```sh
kaiman -h
kaiman --help
kaiman -v
kaiman --version

kaiman add local-opencode adapter=opencode-local
kaiman add local-codex adapter=codex-local
kaiman list
kaiman edit local-opencode timeout=120
kaiman remove local-opencode

kaiman start
kaiman status
kaiman stop
kaiman gui
```

`add` and `edit` accept the service keys `display-name`, `adapter`, `url`,
`env`, `token-command`, `scheme`, `user` and `timeout`. `start` launches the
gateway as a detached process, `status` prints its state and address, and `stop`
terminates it. `gui` opens a native WebView manager backed by the same public
library API. The manager lists services and provides add, edit, remove, start,
stop, status and listen-address controls. It is available only in builds with
`libwvw`; elsewhere the command reports that the GUI is unavailable and exits
with status 1. On Windows, libwvw uses
`$XDG_DATA_HOME/webview2-runtime/msedgewebview2.exe` when present, or
`%LOCALAPPDATA%\KaisarCode\webview2-runtime\msedgewebview2.exe`, then falls back
to the installed runtime. `KC_WEBVIEW2_DIR` overrides that default. The runtime
is not placed in `bin/`, which is a distribution directory. See
[WebView2 runtime requirements](../lib/kaisarcode/libwvw.c/doc/webview2-runtime.md)
for the full Windows and Wine checklist.

Wine does not provide WebView2. Download the x64 Fixed Version Runtime CAB from
the [WebView2 download page](https://developer.microsoft.com/microsoft-edge/webview2/)
and extract it into the shared user-data directory:

```sh
runtime="${XDG_DATA_HOME:-$HOME/.local/share}/webview2-runtime"
mkdir -p "$runtime"
cabextract -d "$runtime" Microsoft.WebView2.FixedVersionRuntime.*.x64.cab
fixed=$(find "$runtime" -mindepth 1 -maxdepth 1 -type d \
    -name 'Microsoft.WebView2.FixedVersionRuntime.*' | head -n 1)
mv "$fixed"/* "$runtime"/
rmdir "$fixed"
wine bin/windows/x86_64/kaiman.exe gui
```

The CAB uses a versioned subdirectory; move its contents up so
`$runtime/msedgewebview2.exe` exists. That directory is shared with other
projects, so nothing is copied into `bin/`.
To keep the runtime elsewhere, set `KC_WEBVIEW2_DIR` to the Windows path Wine
exposes, for example `Z:\home\USER\.local\share\webview2-runtime`. Adjust the
`Z:` path when the prefix uses another drive mapping.
