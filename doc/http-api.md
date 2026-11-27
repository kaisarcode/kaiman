# HTTP API

The gateway listens on `127.0.0.1:49380` by default. Change `listen` in the
`[gateway]` section of the configuration file to use a different host or port.

| Method | Path | Behavior |
| --- | --- | --- |
| `GET` | `/health` | Returns `{"status":"ok"}`. |
| `GET` | `/v1/providers` | Lists supported provider types. |
| `GET` | `/v1/services` | Lists configured services. |
| `GET` | `/v1/models` | Discovers models from configured services. |
| `GET` | `/v1/sessions` | Discovers service sessions. |
| `POST` | `/v1/sessions` | Creates a session. |
| `POST` | `/v1/sessions/fork` | Forks a session. |
| `POST` | `/v1/chat/completions` | Selects a service by `model` and forwards the chat request. |

## Providers and services

A provider is a supported adapter type. `/v1/providers` lists every supported
provider with its display `name` and adapter `slug`, independently of the
configured services. For example, `{"name":"OpenAI","slug":"openai"}`.

A service is a configured instance of a provider. `/v1/services` returns a
list of objects containing `name` (display name, falling back to the slug),
`slug` (configured service identifier), and `provider` (adapter slug).
For example:

```json
{"data":[{"name":"Local OpenAI","slug":"local","provider":"openai"}]}
```

Both catalogs require no upstream requests and expose no credentials or
connection URLs. An empty service configuration returns `{"data":[]}`.

## Models

A chat request uses a model identifier returned by the model catalog and a
`messages` array. Each identifier has the form `service/model`. Model selection
happens per request, and the gateway removes the service prefix before sending
the request upstream. Multiple services can expose the same upstream model.

```sh
curl http://127.0.0.1:49380/v1/providers
curl http://127.0.0.1:49380/v1/services
curl http://127.0.0.1:49380/v1/models
curl "http://127.0.0.1:49380/v1/models?service=openai"
curl "http://127.0.0.1:49380/v1/models?service=openai,ollama"
```

The optional `service` query parameter selects configured service slugs.
Only selected services are queried. Comma-separated and repeated parameters
are supported. Unmatched names return no models; malformed filters return
400. The obsolete `provider` filter is rejected.

The catalog is refreshed from services on each request. Unreachable services
are omitted when other catalogs succeed; if every selected service fails,
the gateway returns 502. A model catalog does not guarantee that every
listed model supports chat or that account permissions allow inference.

## Chat completions

`stream:true` returns a chunked SSE response with `data:` frames and a final
`data: [DONE]` frame.

Use the optional `session` field to continue a selected provider session. See
[Sessions](sessions.md) for the contract, supported adapters and examples.

## Errors

Invalid JSON, missing credentials or an unusable request returns `400`. An
unknown path or unknown model returns `404`; an oversized body returns `413`;
a busy selected session returns `409`; and an unreachable or malformed
upstream returns `502`. Upstream responses with
status `400` or higher are propagated with their status, content type and body.

See [Providers](providers.md) for adapter-specific behavior and default URLs.
