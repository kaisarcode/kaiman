# WebView2 runtime requirements

`libwvw` bundles `WebView2Loader.dll`, the loader and discovery library. It does
not bundle `msedgewebview2.exe`, the browser runtime. Both are needed at run
time: the loader must be beside the executable, and the runtime must be either
installed or supplied as a fixed-version directory.

## Resolution order

libwvw selects the runtime in this order:

1. `KC_WEBVIEW2_DIR` when set. It must be a directory containing
    `msedgewebview2.exe`; an invalid value is an error rather than a silent
    fallback.
2. `${XDG_DATA_HOME:-$HOME/.local/share}/webview2-runtime` on Unix-like hosts,
    or `%LOCALAPPDATA%\KaisarCode\webview2-runtime` on Windows, when that
    directory exists and contains `msedgewebview2.exe`.
3. The installed WebView2 runtime, discovered by `WebView2Loader.dll`.

The selected directory is passed directly to
`GetAvailableCoreWebView2BrowserVersionStringWithOptions()` and
`CreateCoreWebView2EnvironmentWithOptions()`. No global
`WEBVIEW2_BROWSER_EXECUTABLE_FOLDER` variable is required.

## Windows

Windows 11 and sufficiently updated Windows 10 installations normally include
the Evergreen WebView2 Runtime. In that case no variable and no local
fixed-version directory are needed.

If the host has no WebView2 Runtime, either install the Evergreen Runtime from
the [WebView2 download page](https://developer.microsoft.com/microsoft-edge/webview2/)
or extract a fixed-version runtime into the shared user-data directory above
and set `KC_WEBVIEW2_DIR` when it is not in the default location.

`WebView2Loader.dll` is copied beside every Windows executable produced by the
build. If it is missing, libwvw reports `WebView2Loader.dll not found beside the
executable`; that is a packaging problem, not a missing browser runtime.

## Wine

Wine does not install WebView2. A 64-bit prefix is required:

```sh
WINEARCH=win64 wineboot
```

Install `wine`, `winepath`, and `cabextract`. Download the x64 Fixed Version
Runtime CAB from the
[WebView2 download page](https://developer.microsoft.com/microsoft-edge/webview2/)
and extract it outside the repository and outside `bin/`:

```sh
runtime="${XDG_DATA_HOME:-$HOME/.local/share}/webview2-runtime"
mkdir -p "$runtime"
cabextract -d "$runtime" Microsoft.WebView2.FixedVersionRuntime.*.x64.cab
fixed=$(find "$runtime" -mindepth 1 -maxdepth 1 -type d \
    -name 'Microsoft.WebView2.FixedVersionRuntime.*' | head -n 1)
mv "$fixed"/* "$runtime"/
rmdir "$fixed"
```

The CAB stores the runtime inside a versioned directory. Move its contents to
the root so `$runtime/msedgewebview2.exe` exists; that is the directory libwvw
validates. The location is shared by other projects. `make test wine` converts
it with `winepath -w` and exports `KC_WEBVIEW2_DIR` for the test executable; no
runtime is copied into `.build/` or `bin/`.

For a GUI launched directly under Wine, export the Windows path explicitly:

```sh
export KC_WEBVIEW2_DIR='Z:\home\USER\.local\share\webview2-runtime'
wine your-app.exe
```

`Z:` is Wine's default Unix drive mapping. Change it when the prefix uses a
different mapping; `winepath -w "$runtime"` prints the correct value.

The test runner sets `EGL_LOG_LEVEL=fatal`, `WINEDEBUG=-all`, and
`DXVK_LOG_LEVEL=none` to keep graphics noise out of the contract output.

## Error messages

| Message | Meaning |
| --- | --- |
| `WebView2Loader.dll not found beside the executable` | The loader was not copied to the output directory. |
| `KC_WEBVIEW2_DIR does not contain a WebView2 runtime` | The override path is wrong or lacks `msedgewebview2.exe`. |
| `WebView2 runtime unavailable (0x...)` | No installed runtime and no usable fixed-version directory. |
| `WebView2 environment creation failed (0x...)` | The selected runtime or user-data directory could not be used. |

## Verification

From this repository:

```sh
make windows/x86_64
make test wine
```

`make windows/x86_64` verifies cross-compilation and artifact placement.
`make test wine` exercises the real WebView2 runtime under Wine and requires the
fixed-version directory above.
