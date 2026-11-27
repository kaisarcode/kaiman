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

## Test connection

The desktop service editor provides a Test connection button beside Save.
The check uses the current form, including unsaved connection settings and
credentials, and displays the model count or an error below the buttons.
It works while the gateway is stopped. It does not send an inference request
or save the service.

## System tray

The system tray uses dedicated padded icons. The icon is grayscale while the
gateway is stopped and colored while it is running. The tray follows the
gateway state, including changes made through the CLI.

## API endpoints

Open the API endpoints dialog using the code icon in the Gateway panel.
The dialog shows the configured API base URL and endpoint paths. Copy buttons
copy full HTTP URLs, using `127.0.0.1` for IPv4 wildcard listeners and `[::1]`
for IPv6 wildcard listeners. Specific interface addresses remain unchanged.
The base URL includes `/v1`; the health endpoint is at `/health` on the origin.
Restart a running gateway after saving a new listen address to apply it.

## Service status

The service list shows a green dot when model discovery succeeds and a gray
dot when a check fails or has not finished. The tooltip reports the last
result and model count or error. Saved services are checked independently
at startup and 30 seconds after each completed check, including while the
gateway is stopped. Edits invalidate the previous result. Automatic checks
cap the configured upstream timeout at ten seconds per operation and do not
send inference requests or modify credentials.

## Desktop installation

Run `installer` on Linux or `installer.exe` on Windows from the
desktop build.
The installer copies the application executable, shared runtime libraries and
GUI assets to the selected directory. The default is
`~/.local/opt/kaisarcode/kaiman` on Linux and
`%LOCALAPPDATA%\\kaisarcode\\kaiman` on Windows. On Linux, the menu category selector uses standard XDG identifiers and defaults
to Programming (`Development`). On Windows, the Start Menu folder is editable
and defaults to Development. The menu entry launches `kaiman gui`.
Existing application configuration and credentials are left unchanged.
The installer copies the uninstaller and its menu library into the installed
runtime. Menu registration is supported on Linux and Windows by libmenu.
The installed directory is recorded in `install.path` under the private state
directory (`$XDG_STATE_HOME/kaiman` or `$HOME/.local/state/kaiman`).
Only one installation per user is allowed.

## Software updates

The installed desktop application checks GitHub releases at startup, including
pre-releases. A download icon appears beside the installed version when a newer
release is available for the current platform. Open the update dialog and
confirm the update to download and verify the release.

The update uses the registered installation directory and preserves the menu
entry, configuration, credentials and session state. It stops the application
and gateway before replacing runtime files, retains a temporary backup for
rollback, and opens the application again. The gateway can be started from the
updated application.

Updates require numeric release tags such as `v0.1.1` and a compatible ZIP
asset with a SHA-256 digest published by GitHub.

## Desktop uninstallation

Run `uninstaller` or `uninstaller.exe` from the installed directory or a
desktop build. The registered installation path is read from `install.path`.
Uninstallation is unavailable when no installation is registered.
The uninstaller stops the GUI and gateway, removes the menu entry, and
removes the installed runtime files. The directory is removed if empty.
Configuration, credentials and local session references are retained by
default and can be removed using the optional checkbox. Provider-side
sessions and external agent credentials are left unchanged. On Windows,
runtime cleanup is deferred until the uninstaller exits. The installation
record is removed after runtime cleanup.
