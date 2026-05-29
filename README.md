# tsf-ai

Talking to AI providers from a test suite, packaged as an external Test
Environment (TE) repository.

Library:

- `tapi_ai` — engine-side, built as a shared library: sending a prompt
  to a large-language-model provider from a Test Agent with a token,
  attaching files, and streaming the answer.
  - `tapi_ai` — providers, the connection, one-shot and multi-turn
    requests, the answer and its token usage;
  - `tapi_ai_file` — attaching an image or a PDF to a message;
  - `tapi_ai_stream` — a streaming session: the answer arriving as it
    is generated.

TE has nothing for talking to an AI service.

## Providers, through one API

| Provider | `tapi_ai_provider` | Wire shape |
|---|---|---|
| OpenAI | `TAPI_AI_OPENAI` | OpenAI Chat Completions |
| xAI (Grok) | `TAPI_AI_XAI` | OpenAI-compatible |
| Cursor, Groq, Together, DeepSeek, Mistral, Ollama, local… | `TAPI_AI_OPENAI_COMPATIBLE` + a base URL | OpenAI-compatible |
| Anthropic (Claude) | `TAPI_AI_ANTHROPIC` | Anthropic Messages |
| Google (Gemini) | `TAPI_AI_GEMINI` | Gemini generateContent |

The providers do not agree on the wire — how a system prompt is
carried, how an image is attached, where the token counts sit in the
answer — so three shapes cover them and each provider is mapped to one.
`tapi_ai_supports()` says what a provider can do; everything else is the
same call whichever provider answers.

```c
tapi_ai_conn conn;
tapi_ai_reply reply;

CHECK_RC(tapi_ai_conn_init(factory, TAPI_AI_OPENAI, NULL,
                           getenv("OPENAI_API_KEY"), &conn));
if (!tapi_ai_available(&conn, 10000))
    TEST_SKIP("The agent cannot reach the provider");

CHECK_RC(tapi_ai_ask(&conn, "You are terse.", "Say hello.", NULL,
                     60000, &reply));
RING("%s (%d in, %d out)", reply.text, reply.usage.input_tokens,
     reply.usage.output_tokens);
tapi_ai_reply_free(&reply);
tapi_ai_conn_fini(&conn);
```

## The token never touches the command line

The key given to `tapi_ai_conn_init()` travels to the agent in an
environment variable (`TSF_AI_KEY`), not in `argv` — a key on a command
line is visible to anyone who runs `ps` on the agent, the same reason
tsf-smb keeps a password out of `argv`. A test reads the key from its
own configuration and hands it over once. A keyless local endpoint
(Ollama) takes `NULL`.

## Files

A message carries files — an image for a vision model, a PDF for a
document one:

```c
tapi_ai_ask_with_file(&conn, NULL, "What is in this chart?",
                      "/tmp/chart.png", "image/png", NULL, 60000, &reply);
```

The file is on the agent; the helper reads it there, base64-encodes it
and places it in the shape the provider wants — a data URL for OpenAI,
a `source` block for Anthropic, `inline_data` for Gemini.
`tapi_ai_attach_from_engine()` copies a file the test carries with it
onto the agent first. An attachment a provider cannot take is refused
with `TE_EOPNOTSUPP` before anything is sent.

## Streaming

`tapi_ai_stream_*` opens the provider's streaming endpoint and hands
back the answer as it is generated — for asserting that a long answer
*begins* before it is whole, or watching a long generation without
waiting on a timeout for the whole of it:

```c
tapi_ai_stream stream;
te_string piece = TE_STRING_INIT;

CHECK_RC(tapi_ai_stream_start(&conn, "Count to twenty.", NULL, &stream));
while (tapi_ai_stream_next(&stream, 10000, &piece) == 0) {
    RING("delta: %s", piece.ptr);
    te_string_reset(&piece);
}
CHECK_RC(tapi_ai_stream_finish(&stream, &usage));
tapi_ai_stream_free(&stream);
```

The token counts arrive at the end of the stream, the same as a
non-streamed request.

## How it works

Everything runs on the agent behind a job factory: the request goes out
from the agent, the files are the agent's, the key is read on the agent.
The provider differences live in a small **python3 helper** (standard
library only — `urllib`, `json`, `base64`, `ssl`) that this library
puts on the agent; the C code builds a provider-neutral request spec,
runs the helper with the key in the environment, and reads back a small
JSON result. Keeping the shaping in the helper is what lets one code
path serve OpenAI, Anthropic and Gemini. The helper is generated into
`tapi_ai_helper.c` from `tapi_ai_helper.py.in` (the source of truth) so
the build needs no code-generation step — the choice tsf-emu makes for
its QMP helper. The agent needs `python3`; a real request also needs the
agent to reach the provider's network.

## Usage

Declare the repository in an external libraries catalog and pass it to
`dispatcher.sh --external=external.yml`:

```yaml
repositories:
  - name: tsf_devtool
    url: https://github.com/interpretica-io/tsf-devtool.git
    ref: <tag>
    libs:
      - tapi_devtool
  - name: tsf_ai
    url: https://github.com/interpretica-io/tsf-ai.git
    ref: <tag>
    libs:
      - tapi_ai
```

Bind them in `builder.conf`:

```
TE_EXT_REPO_USE([tsf_devtool], [], [tapi_devtool])
TE_EXT_REPO_USE([tsf_ai], [], [tapi_ai])
```

Then add `tapi_ai` to `te_libs` in the suite's `meson.build`. Requires
TE with `TE_EXT_REPO` support and an **RPC** job factory
(`ta_rpcprovider`): the helper's output is read back, which needs output
channels.

## What was verified, and what was not

**Verified through a Test Agent, against a mock provider.** The library
was exercised by a TE suite (`ai-ts`, forked from `dummy-cybersec-ts`)
that stands up a local mock endpoint speaking the OpenAI, Anthropic and
Gemini shapes — request parsing, streaming (SSE) and error statuses — so
the request building, the file encoding, the streaming reader, the usage
accounting and the error mapping are checked deterministically and
offline, without a key or outbound network. Every test ran green on a
live agent (`./scripts/run.sh docker guess`):

- **`ask`** — all three wire shapes reach the mock, which echoes back
  what it parsed (system prompt, message count, model); the key arrives
  in the provider's auth header (`Authorization: Bearer` / `x-api-key` /
  `x-goog-api-key`, from `TSF_AI_KEY`, never in `argv`); the token usage
  comes back as the mock's fixed 11 in / 22 out / 33 total, and
  `tapi_ai_supports()` reports each provider's capabilities.
- **`files`** — an attached PNG is encoded as an image and a PDF as a
  document in each shape (the mock counts `img=1 doc=0`, `img=0 doc=1`,
  and for Anthropic `img=1 doc=1`); a `.png` with no explicit type is
  recognised by extension.
- **`stream`** — the SSE answer arrives in several deltas (the mock
  chunks by 8 chars), they reassemble into the same text a non-streamed
  request gives, `tapi_ai_stream_next()` ends with `TE_ENODATA`, and the
  end-of-stream usage is the same 11/22; `tapi_ai_stream_collect()`
  returns the whole answer at once.
- **`errors`** — a rejected key (mock 401) maps to `TE_EACCES`, an
  unreachable endpoint makes `tapi_ai_available()` false and `ask` return
  `TE_ECOMM`, and an OpenAI-compatible connection without a base URL is
  refused `TE_EINVAL`.

**Not verified against the real services.** No request was made to
OpenAI, Anthropic, xAI or Google — that needs a key and the network.
The wire shapes are built from each provider's public API
documentation; the first suite to point `tapi_ai` at a real endpoint
should expect to adjust a model name or a field.

## Scope

- **A request costs money and leaves the network.** A test that points
  at a real provider spends tokens and sends its prompt (and any
  attached file) to that provider. The verification here sent
  everything to a mock on the agent itself.
- **The library reads a key; it does not manage one.** Where the key
  comes from — a run's environment, a Configurator value, a secret
  store — is the suite's business.
