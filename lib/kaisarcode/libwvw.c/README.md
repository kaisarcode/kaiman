# libwvw.c - Native WebView window

`libwvw.c` creates a native WebView window with a portable C API for navigation,
window state, initialization scripts, and an optional native bridge. Backends
use WebView2 on Windows, WebKitGTK on Linux, and WKWebView on macOS.

## Quick start

```c
#include "libwvw.h"

kc_wvw_t *window = NULL;
int width = 800;
int height = 600;
kc_wvw_options_t options = {
    .url = "about:blank",
    .title = "Example",
    .width = &width,
    .height = &height
};

if (kc_wvw_open(&window, &options) == KC_WVW_OK) {
    kc_wvw_set_title(window, "Ready");
    kc_wvw_show(window);
    kc_wvw_close(window);
}
```

The URL is required. Omitted size values default to 1280x720 and omitted
boolean options are false.

## Execute registered desktop programs

Applications can register the filesystem paths of allowed executables with
`kc_wvw_set_executables()`. JavaScript then calls
`await NativeBridge.exec(["name", "arg1", "arg2"])`. Calls execute without
shell interpretation in a worker thread; a successful process resolves to
merged stdout and stderr, while a nonzero exit rejects with an error carrying
its exit code and captured output. This feature is generic: no executable
needs to be a kclib or a KaisarCode program.

See the [native bridge guide](doc/native-bridge.md) for the full contract,
origin restrictions, limits and error handling.

## Requirements

Linux builds require GTK 3, WebKitGTK 4.1, and `pkg-config`. Windows execution
requires the Microsoft Edge WebView2 Runtime. libwvw uses
`$XDG_DATA_HOME/webview2-runtime/msedgewebview2.exe` when present, or
`%LOCALAPPDATA%\KaisarCode\webview2-runtime\msedgewebview2.exe` on Windows, then
falls back to the installed runtime. Set `KC_WEBVIEW2_DIR` to override that
default with another directory containing `msedgewebview2.exe`. The runtime is
not bundled and never belongs in `bin/`, which is a distribution directory.
See [WebView2 runtime requirements](doc/webview2-runtime.md) for the full
Windows and Wine checklist. macOS uses AppKit, WebKit, and Foundation.

### WebView2 on Wine

Wine does not install the WebView2 Runtime. To run a Windows libwvw build under
Wine, download the x64 Fixed Version Runtime CAB from the
[WebView2 download page](https://developer.microsoft.com/microsoft-edge/webview2/)
and extract it into the shared user-data directory:

```sh
runtime="${XDG_DATA_HOME:-$HOME/.local/share}/webview2-runtime"
mkdir -p "$runtime"
cabextract -d "$runtime" Microsoft.WebView2.FixedVersionRuntime.*.x64.cab
fixed=$(find "$runtime" -mindepth 1 -maxdepth 1 -type d \
    -name 'Microsoft.WebView2.FixedVersionRuntime.*' | head -n 1)
mv "$fixed"/* "$runtime"/
rmdir "$fixed"
```

The CAB uses a versioned subdirectory; move its contents up so
`$runtime/msedgewebview2.exe` exists. That directory is shared by all projects
and is the libwvw default, so `make
test wine` finds it without copying anything into `bin/` or the volatile build
directory. To keep the runtime elsewhere, extract it there and set
`KC_WEBVIEW2_DIR` to the Windows path Wine exposes:

```sh
export KC_WEBVIEW2_DIR='Z:\home\USER\.local\share\webview2-runtime'
```

`Z:` is Wine's default mapping to the Unix filesystem; adjust it when the prefix
uses another drive mapping. If neither the default directory nor
`KC_WEBVIEW2_DIR` is available, `make test wine` stops with instructions instead
of reporting unrelated WebView test failures. Normal Windows hosts already have
the Evergreen runtime installed and do not need a local fixed-version runtime.

## Documentation

Read the [native bridge guide](doc/native-bridge.md) before exposing application
methods, executable commands, or privileged origins to WebView content.

## Build

From this project directory:

```bash
make
make test
```

Use `make all` for the configured cross-platform targets when their required
toolchains are installed.

## License

[![LGPLv3](https://www.gnu.org/graphics/lgplv3-147x51.png)](https://www.gnu.org/licenses/lgpl-3.0.html)

This project is distributed under the **GNU Lesser General Public License version 3 (LGPLv3)**.
