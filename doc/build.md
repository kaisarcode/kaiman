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

## Distribution

Package previously built targets with `zip`:

```sh
make dist
make dist linux/x86_64
make dist windows/x86_64
make dist all
```

The default target is the native platform. Specify one or more targets to
package selected builds, or use `all` to package every already built target.
Missing builds are skipped with `all`. Explicit targets must already be built.

Each archive contains a top-level `kaiman/` directory with the contents of
the corresponding target directory in `bin/`, including the executable,
libraries and desktop assets when present.
For example, `make dist linux/x86_64` creates
`dist/kaiman-linux-x86_64.zip`.

The `dist/` directory is excluded from version control. Creating an archive
does not compile, publish or upload a release.
