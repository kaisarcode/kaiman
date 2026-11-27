# Providers

A provider identifies a supported adapter type. A service is a configured
instance of a provider. The HTTP API exposes these separately through
`/v1/providers` and `/v1/services`. The provider slug matches the `adapter`
configuration field.

## Default URLs

| Adapter | Default URL |
| --- | --- |
| `openai` | `https://api.openai.com/v1` |
| `opencode` | `http://127.0.0.1:4096` |
| `opencode-local` | Automatically discovered |
| `codex-local` | Not applicable (local stdio) |
| `opencode-cloud` | `https://opencode.ai/inference` |
| `anthropic` | `https://api.anthropic.com/v1` |
| `gemini` | `https://generativelanguage.googleapis.com/v1beta` |
| `responses` | `https://api.openai.com/v1` |
| `ollama` | `http://127.0.0.1:11434` |
| `bedrock` | `https://bedrock-runtime.us-east-1.amazonaws.com` |

Override the OpenCode Server URL when its port differs (see
`opencode service status`). Override the Bedrock URL to select another region.

Local adapters require the corresponding desktop executable on PATH (or
OpenCode in its usual per-user installation directory). The GUI hides URL and
authentication for these adapters. Configure them with:

```sh
kaiman add local-opencode adapter=opencode-local
kaiman add local-codex adapter=codex-local
```

OpenCode must already have a running local service and Codex must already be
configured to use its provider. Local subprocess adapters are unavailable on
Android and iOS. Native Windows executables are required on Windows.

## Adapter behavior

- `openai` forwards chat requests to an OpenAI-compatible `/chat/completions`
    endpoint, removing the service prefix from the requested model and
    restoring the full identifier in responses. Model discovery uses `/models`.
- `opencode-local` (OpenCode Local) runs the installed OpenCode executable
    to discover its running loopback service with `opencode service status`
    and `opencode service get password`. It uses the same session and model
    APIs as OpenCode Server without storing credentials or a URL.
- `codex-local` (Codex Local) runs `codex app-server` over private stdin/stdout
    pipes, checks the existing authentication with `account/read` and
    discovers models using paginated `model/list` requests. Text requests
    create ephemeral threads when no session is specified; an explicit session
    resumes a saved thread. Kaiman uses a read-only sandbox with no approvals.
    Codex owns its login and token refresh. The gateway returns completed
    assistant text, including buffered SSE for streaming clients.
- `opencode` (OpenCode Server) uses the server HTTP API at its root URL.
    Discovery uses `/api/model`, and model IDs use
    `service/providerID/modelID`. Loopback servers reuse a persisted session,
    switch to the requested model before prompting, and retain bounded
    history. Remote servers create a temporary session, generate text and
    remove the session unless a session is explicitly selected. Use basic
    authentication if the server requires it.
    `stream:true` emits the completed reply as SSE chunks.
- `opencode-cloud` (OpenCode Cloud) uses the Console inference API. Set the
    base URL to `https://opencode.ai/inference` for OpenCode Models or
    `https://opencode.ai/inference/go` for Go, with a bearer API key.
    Discovery uses `/v1/models`; chat uses `/openai/v1/chat/completions`.
    An explicit session ID is forwarded as `x-opencode-session`.
    Non-chat model types are omitted. Free-tier chat models can require
    the OpenCode application.
- `anthropic` uses the Messages API (`/messages`). Set the base URL to the API
    version root and use `scheme=bearer` for the API key.
- `gemini` uses `generateContent` with the model in the URL. Set the base URL
    to the API version root and use `scheme=bearer` for the API key.
- `responses` uses `/responses` with a bearer credential.
- `ollama` uses `/api/chat` from the server root, normally without authentication.
- `bedrock` uses `/model/{modelId}/converse` from the regional runtime root
    with an Amazon Bedrock bearer API key. Discovery queries the control-plane
    foundation model and inference profile catalogs. The credential must allow
    those operations. AWS SigV4 is not supported.

The five native text adapters accept system, user and assistant text messages,
maximum output tokens, temperature and top-p. Streaming requests emit the
completed text and finish reason as SSE frames, rather than forwarding live
provider events. Tool calls, multimodal input and structured output requests
are not supported by these adapters. Upstream model-specific parameter
constraints still apply.

See [Sessions](sessions.md) for the capabilities of each adapter.
