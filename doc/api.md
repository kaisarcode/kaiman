# Public API

Include `src/libkaiman.h` and link `libkaiman`.

- `kc_kaiman_open()`, `kc_kaiman_load()` and `kc_kaiman_close()` manage the
    configuration lifetime.
- `kc_kaiman_get_services()`, `kc_kaiman_get_service()`,
    `kc_kaiman_add_service()`, `kc_kaiman_service_set()`,
    `kc_kaiman_service_get()`, `kc_kaiman_service_timeout()` and
    `kc_kaiman_service_remove()` manage saved services.
- `kc_kaiman_listen()`, `kc_kaiman_address()` and `kc_kaiman_path()` inspect or
    change gateway settings.
- `kc_kaiman_gateway_open()`, `kc_kaiman_gateway_run()`,
    `kc_kaiman_gateway_stop()`, `kc_kaiman_gateway_address()` and
    `kc_kaiman_gateway_close()` run the HTTP gateway in the current process.
- `kc_kaiman_start()`, `kc_kaiman_stop()` and `kc_kaiman_status()` manage the
    detached gateway process used by the CLI.
- `kc_kaiman_strerror()`, `kc_kaiman_error()` and `kc_kaiman_version()` report
    status and version information.

Every mutation persists immediately. Status codes follow the kclib convention:
`KC_KAIMAN_OK`, `KC_KAIMAN_NOT_FOUND`, and negative error codes. Handles are
borrowed. A removed service handle stays allocated but becomes invalid.

A service descriptor accepts a unique `name`, an `adapter` (see the adapter
[provider list](providers.md)), an optional `http://` or `https://` URL override, an optional
credential reference (`env` or `token-command`), an `auth` scheme (`none`,
`bearer` or `basic`), an optional `user`, and a `timeout` from 1 to 600
seconds. Direct credentials are stored separately in the system credential store
using `kc_kaiman_service_authenticate`.

An omitted or empty URL selects the adapter default. `kc_kaiman_default_url()`
returns that default; `kc_kaiman_service_url()` resolves the effective service
URL. The `url` field contains only the explicit override. The GUI displays the
default as a placeholder. Clearing an override restores the adapter default.
Local subprocess adapters have no static default URL. For automatic local
connections, URL resolution happens at request time.

For HTTP clients, see the [HTTP API](http-api.md).

## JavaScript projection

`src/libkaiman.js` exposes the configuration-level API to JavaScript:

```js
import { kaiman } from "./libkaiman.js";

var kmn = kaiman.open({ path: "./services.ini" });
var services = kmn.getServices();
var myserv1 = services[0];
var myserv2 = kmn.getService("local-opencode");

myserv1.set("name", "myserv1");

const myserv3 = kmn.addService({
  name: "openai",
  adapter: "openai",
  url: "https://api.openai.com/v1",
  apiKeyEnv: "OPENAI_API_KEY"
});

kmn.listen("127.0.0.1:10000");
myserv3.get("url");
myserv1.remove();
kmn.close();
```

The projection is configuration-only. No WASM artifact is produced because
`libnetl.c` has no WASM target.
