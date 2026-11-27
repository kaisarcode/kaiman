# Sessions

Kaiman exposes session-aware providers through a common HTTP contract.
A session ID has the form `provider/session`, where `provider` is a configured
service name and the second component is the native session or thread ID.

| Adapter | List | Create | Fork | Continue |
| --- | --- | --- | --- | --- |
| `opencode-local` | Yes | Yes | Yes | Yes |
| `opencode` | Yes | Yes | Yes | Yes |
| `codex-local` | Yes | Yes | Yes | Yes |
| `opencode-cloud` | No | No | No | Stable routing ID |
| Other adapters | No | No | No | No |

## List sessions

```sh
curl http://127.0.0.1:49380/v1/sessions
curl "http://127.0.0.1:49380/v1/sessions?provider=local-opencode,local-codex"
```

The optional provider filter accepts configured service names, repeated
parameters or comma-separated values, like the model catalog. Kaiman queries
only matching session-capable services and follows upstream pagination. Archived
sessions and ephemeral Codex threads are excluded. Codex lists local history
visible to the authenticated account. Providers that do not list sessions are
omitted. If no session-capable provider matches, the result is an empty list.
If all matching session-capable providers fail, the gateway returns 502.

Response:

```json
{
    "data": [
        {
            "id": "local-opencode/ses_abc123",
            "provider": "local-opencode",
            "title": "GitView",
            "status": "idle"
        }
    ]
}
```

Session metadata includes the namespaced ID, configured provider name, title
(or null), and runtime status. Status is `idle`, `active`, `error` or
`unknown`. OpenCode activity comes from its server's active-session map.
Codex status comes from the app-server. An unloaded thread has unknown runtime
status. Activity is reported by the connected server, not all running
instances of the provider.

## Create a session

Send the selected model ID from `GET /v1/models` and an optional title:

```sh
curl -X POST http://127.0.0.1:49380/v1/sessions \
    -H "Content-Type: application/json" \
    -d '{
        "model": "local-opencode/opencode/big-pickle",
        "title": "GitView"
    }'
```

The response is a single session object with the same fields as the catalog
entries. Save its `id` in the client application's own configuration. Kaiman
does not bind an explicit session to a model permanently.

Codex creates a non-ephemeral thread and keeps its app-server alive. A newly
created, unused thread may not appear in the persistent thread catalog and
may not survive an app-server restart until Codex has persisted its history.

## Fork a session

Create a new session with the existing session's history:

```sh
curl -X POST http://127.0.0.1:49380/v1/sessions/fork \
    -H "Content-Type: application/json" \
    -d '{"session":"local-opencode/ses_abc123"}'
```

The original session is unchanged. The response contains a new session ID.
For OpenCode, the optional `before` field is an upstream message ID; history
before that message is copied. For Codex, the optional `last_turn` field is an
upstream turn ID; history through that turn is copied. Without a boundary,
the provider forks the available history. The boundary formats are
provider-specific.

## Continue a session

Pass the namespaced session ID as the optional `session` property of a Chat
Completions request:

```sh
curl -X POST http://127.0.0.1:49380/v1/chat/completions \
    -H "Content-Type: application/json" \
    -d '{
        "model": "local-opencode/opencode/big-pickle",
        "session": "local-opencode/ses_abc123",
        "messages": [
            {"role": "user", "content": "Generate a commit message"}
        ]
    }'
```

The provider prefix of the model must match the session's provider. Send
only new messages for stateful OpenCode or Codex requests; the provider
already has the preceding conversation. Explicit sessions are not replaced
or deleted by Kaiman. An active selected session reported by the upstream
server is rejected with HTTP 409, without interrupting its agent.

When no explicit session is supplied, OpenCode loopback connections retain
the existing automatic session behavior, remote OpenCode connections use a
temporary session, and Codex creates an ephemeral thread for that request.

OpenCode Cloud treats `session` as a client-selected routing identifier.
For example, `console/ses_gitview` sends `x-opencode-session: ses_gitview`
upstream. The client must still send its conversation messages as required
by Chat Completions. A Console routing ID is not a retrievable server-side
conversation and is not included in the catalog.

Kaiman rejects a session parameter for adapters that have no session support.
Session extension requests with invalid identifiers, unsupported adapters
or incompatible provider prefixes return 400. A provider not found during
creation or forking returns 404; upstream failures return 502. Reusing sessions
does not change the provider's quota rules.

## Provider APIs

- [OpenCode API](https://opencode.ai/docs/api)
- [OpenCode Console harnesses](https://opencode.ai/docs/console/harnesses/)
- [Codex app-server](https://github.com/openai/codex/tree/main/codex-rs/app-server)

The HTTP gateway has no client authentication of its own. Keep session access
within a trusted environment; the gateway uses the configured account's
permissions. Session IDs are not credentials and do not restrict provider
access.
