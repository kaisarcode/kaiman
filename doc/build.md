# Build

Build the native target and its dependencies from the repository root:

```sh
make
make test
```

Run `make all` to build every supported target. For Windows tests under Wine:

```sh
make windows/x86_64
make test wine
```

Tests use the existing build artifacts without rebuilding them.
Run `kcs` to validate the source and `make clean` when a full rebuild is required.

## Dependencies

Dependencies are vendored under `lib/kaisarcode`: libhttp.c, libnetl.c,
libtls.c, libwvw.c, libtray.c and libmenu.c. The shared library and CLI load
their dependencies from their own directory.

## Output

Each target produces the CLI and embeddable C library under `bin/`.
Desktop targets also include the GUI assets beside the executable.
WASM is unavailable because libnetl.c does not support it.
