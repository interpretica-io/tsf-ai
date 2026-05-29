/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Talking to an AI provider from a test
 *
 * @defgroup tapi_ai AI providers (tapi_ai)
 * @{
 *
 * Sending a prompt to a large-language-model provider from a Test
 * Agent, with a token, and reading the answer back: OpenAI, Anthropic
 * (Claude), xAI (Grok), Google (Gemini), and anything that speaks the
 * OpenAI Chat Completions shape - Cursor, Groq, Together, DeepSeek,
 * Mistral, Ollama, a local gateway - through one provider-neutral API.
 *
 * - @ref tapi_ai - providers, the connection, one-shot and multi-turn
 *   requests, the answer and its token usage;
 * - @ref tapi_ai_file - attaching a file (an image, a PDF) to a
 *   message;
 * - @ref tapi_ai_stream - a streaming session: the answer arriving as
 *   it is generated, token by token.
 *
 * Everything runs on the agent behind a job factory. The request goes
 * out from the agent, the attached files are the agent's, and the key
 * is read on the agent - so a test drives whichever model the agent's
 * network and credentials can reach.
 *
 * @section tapi_ai_shapes One API, three wire shapes
 *
 * The providers do not agree on the wire. Three shapes cover them, and
 * a provider is mapped to one:
 *
 * - **OpenAI Chat Completions** - @c /chat/completions, a Bearer
 *   token. OpenAI itself, and every OpenAI-compatible endpoint: xAI's
 *   Grok, Cursor, Groq, Together, DeepSeek, Mistral, Ollama, a local
 *   proxy. Reach those with @ref TAPI_AI_OPENAI_COMPATIBLE and a base
 *   URL, or the named @ref TAPI_AI_XAI.
 * - **Anthropic Messages** - @c /v1/messages, an @c x-api-key header
 *   and @c anthropic-version. Claude.
 * - **Google Gemini** - @c :generateContent, an @c x-goog-api-key.
 *
 * The differences that matter to a test - how a system prompt is
 * carried, how an image is attached, where the token counts are in the
 * answer - are hidden behind this API; tapi_ai_supports() says what a
 * provider can do.
 *
 * @section tapi_ai_key The token never touches the command line
 *
 * The key is given to tapi_ai_conn_init() and travels to the agent in
 * an environment variable, not in @c argv: a key on a command line is
 * visible to anyone who runs @c ps on the agent. A test reads the key
 * from its own configuration (an environment variable of the run, a
 * Configurator value) and hands it over once.
 *
 * @code
 * tapi_ai_conn conn;
 * tapi_ai_reply reply;
 *
 * CHECK_RC(tapi_ai_conn_init(factory, TAPI_AI_OPENAI, NULL,
 *                            getenv("OPENAI_API_KEY"), &conn));
 * if (!tapi_ai_available(&conn, 10000))
 *     TEST_SKIP("The agent cannot reach the provider");
 *
 * CHECK_RC(tapi_ai_ask(&conn, "You are terse.", "Say hello.", NULL,
 *                      60000, &reply));
 * RING("%s (%d in, %d out)", reply.text, reply.usage.input_tokens,
 *      reply.usage.output_tokens);
 * tapi_ai_reply_free(&reply);
 * tapi_ai_conn_fini(&conn);
 * @endcode
 */

#ifndef __TSF_TAPI_AI_H__
#define __TSF_TAPI_AI_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default timeout for one AI request, ms. Models are slow. */
#define TAPI_AI_TIMEOUT_MS 120000

/** Which provider, and so which wire shape and defaults. */
typedef enum tapi_ai_provider {
    /** OpenAI. */
    TAPI_AI_OPENAI = 0,
    /**
     * Any OpenAI-compatible endpoint. Needs a base URL in
     * tapi_ai_conn_init(); this is how Cursor, Groq, Together,
     * DeepSeek, Mistral, Ollama and local gateways are reached.
     */
    TAPI_AI_OPENAI_COMPATIBLE,
    /** xAI's Grok (OpenAI-compatible at @c api.x.ai). */
    TAPI_AI_XAI,
    /** Anthropic's Claude. */
    TAPI_AI_ANTHROPIC,
    /** Google's Gemini. */
    TAPI_AI_GEMINI,
} tapi_ai_provider;

/** What a provider can do. */
#define TAPI_AI_FEAT_CHAT       (1u << 0)  /**< A prompt and an answer. */
#define TAPI_AI_FEAT_SYSTEM     (1u << 1)  /**< A system prompt. */
#define TAPI_AI_FEAT_MULTITURN  (1u << 2)  /**< A conversation. */
#define TAPI_AI_FEAT_IMAGE      (1u << 3)  /**< Attach an image. */
#define TAPI_AI_FEAT_DOCUMENT   (1u << 4)  /**< Attach a PDF/document. */
#define TAPI_AI_FEAT_STREAM     (1u << 5)  /**< Stream the answer. */
#define TAPI_AI_FEAT_USAGE      (1u << 6)  /**< Report token counts. */

/** The role of a message in a conversation. */
typedef enum tapi_ai_role {
    /** The user. */
    TAPI_AI_ROLE_USER = 0,
    /** The model, in a multi-turn history. */
    TAPI_AI_ROLE_ASSISTANT,
} tapi_ai_role;

/** A file attached to a message. */
typedef struct tapi_ai_attachment {
    /** Path to the file on the agent. */
    char *path;
    /** Its MIME type, e.g. @c "image/png", @c "application/pdf". */
    char *media_type;
} tapi_ai_attachment;

/** One message in a conversation. */
typedef struct tapi_ai_message {
    /** Who said it. */
    tapi_ai_role role;
    /** Its text. */
    char *text;
    /** Vector of #tapi_ai_attachment; empty for text-only. */
    te_vec attachments;
} tapi_ai_message;

/** Options for a request. Zero means "the provider's default". */
typedef struct tapi_ai_opts {
    /**
     * Model name, or @c NULL for the provider's default. The exact
     * string the provider expects, e.g. @c "gpt-4o",
     * @c "claude-opus-4-8", @c "grok-2", @c "gemini-1.5-pro".
     */
    const char *model;
    /** System prompt, or @c NULL. */
    const char *system;
    /** Cap on the answer's length in tokens; @c 0 for the default. */
    unsigned int max_tokens;
    /**
     * Sampling temperature times 100 (so 70 is 0.7), or @c -1 for the
     * default. An integer to keep the option struct free of floats.
     */
    int temperature_pct;
    /** Ask the provider for deterministic JSON, when it supports it. */
    bool json_mode;
} tapi_ai_opts;

/** Initializer for #tapi_ai_opts: all defaults. */
#define TAPI_AI_OPTS_INIT { .model = NULL, .temperature_pct = -1 }

/** What a request cost, in tokens. */
typedef struct tapi_ai_usage {
    /** Prompt tokens, or @c -1 when the provider did not say. */
    int input_tokens;
    /** Completion tokens, or @c -1. */
    int output_tokens;
    /** Total, or @c -1. */
    int total_tokens;
} tapi_ai_usage;

/** The answer to a request. */
typedef struct tapi_ai_reply {
    /** The model's text. */
    char *text;
    /** The model that answered, as the provider named it; may be @c NULL. */
    char *model;
    /** Why it stopped: @c "stop", @c "length", @c "content_filter"... */
    char *finish_reason;
    /** Token usage. */
    tapi_ai_usage usage;
} tapi_ai_reply;

/** A connection to a provider: everything a request needs but the prompt. */
typedef struct tapi_ai_conn {
    /** Job factory. */
    tapi_job_factory_t *factory;
    /** The provider. */
    tapi_ai_provider provider;
    /** Base URL (a copy), or @c NULL for the provider's default. */
    char *base_url;
    /** API key (a copy). */
    char *key;
    /** Path to the helper on the agent, once put there; else @c NULL. */
    char *helper;
    /** The agent name. */
    const char *ta;
} tapi_ai_conn;

/**
 * Open a connection to a provider.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  provider     The provider.
 * @param[in]  base_url      Base URL, or @c NULL for the provider's
 *                          default. Required for
 *                          @ref TAPI_AI_OPENAI_COMPATIBLE.
 * @param[in]  key          The API token. Copied; kept off the command
 *                          line. May be @c NULL for a keyless local
 *                          endpoint (e.g. Ollama).
 * @param[out] conn         The connection; release with
 *                          tapi_ai_conn_fini().
 *
 * @return Status code.
 * @retval TE_EINVAL        A compatible provider without a base URL.
 */
extern te_errno tapi_ai_conn_init(tapi_job_factory_t *factory,
                                  tapi_ai_provider provider,
                                  const char *base_url, const char *key,
                                  tapi_ai_conn *conn);

/**
 * Is the provider reachable from the agent, and is python3 there?
 *
 * Sends nothing to the model: it checks the helper runs and the base
 * URL's host answers a TCP connection. A @c true does not promise the
 * key is valid - only a real request tells you that.
 *
 * @param conn          Connection.
 * @param timeout_ms    Timeout, ms.
 *
 * @return @c true when a request could be attempted.
 */
extern bool tapi_ai_available(tapi_ai_conn *conn, int timeout_ms);

/**
 * What a provider can do.
 *
 * @param provider      The provider.
 *
 * @return A mask of @c TAPI_AI_FEAT_*.
 */
extern unsigned int tapi_ai_features(tapi_ai_provider provider);

/**
 * Can this provider do this?
 *
 * @param provider      The provider.
 * @param feature       One or more @c TAPI_AI_FEAT_*.
 *
 * @return @c true when all of @p feature are supported.
 */
extern bool tapi_ai_supports(tapi_ai_provider provider,
                             unsigned int feature);

/**
 * Ask once: one user prompt, one answer.
 *
 * @param[in]  conn         Connection.
 * @param[in]  system       System prompt, or @c NULL.
 * @param[in]  prompt       The user's message.
 * @param[in]  opts         Options, or @c NULL. @a system in @p opts,
 *                          if set, overrides the @p system argument.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] reply        The answer; release with tapi_ai_reply_free().
 *
 * @return Status code.
 * @retval TE_EACCES        The provider rejected the key (HTTP 401/403).
 * @retval TE_ETIMEDOUT     No answer in time.
 * @retval TE_ECOMM         The endpoint could not be reached.
 * @retval TE_EPROTO        The answer was not what the provider promises.
 * @retval TE_ESHCMD        The provider returned an error (bad model,
 *                          rate limit, ...); the detail is logged.
 */
extern te_errno tapi_ai_ask(tapi_ai_conn *conn, const char *system,
                            const char *prompt, const tapi_ai_opts *opts,
                            int timeout_ms, tapi_ai_reply *reply);

/**
 * Ask with a built conversation and per-message attachments.
 *
 * @param[in]  conn         Connection.
 * @param[in]  messages     Vector of #tapi_ai_message, oldest first,
 *                          ending on a user message.
 * @param[in]  opts         Options, or @c NULL.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] reply        The answer; release with tapi_ai_reply_free().
 *
 * @return Status code, as tapi_ai_ask().
 */
extern te_errno tapi_ai_chat(tapi_ai_conn *conn, const te_vec *messages,
                             const tapi_ai_opts *opts, int timeout_ms,
                             tapi_ai_reply *reply);

/**
 * Start a message with text.
 *
 * @param[out] message      Message to initialize.
 * @param[in]  role         Its role.
 * @param[in]  text         Its text.
 */
extern void tapi_ai_message_init(tapi_ai_message *message,
                                 tapi_ai_role role, const char *text);

/**
 * Release a message.
 *
 * @param message       Message.
 */
extern void tapi_ai_message_free(tapi_ai_message *message);

/**
 * Release a vector of messages.
 *
 * @param messages      Vector of #tapi_ai_message.
 */
extern void tapi_ai_messages_free(te_vec *messages);

/**
 * Write a reply into the log.
 *
 * @param reply         Reply.
 */
extern void tapi_ai_reply_log(const tapi_ai_reply *reply);

/**
 * Release a reply.
 *
 * @param reply         Reply.
 */
extern void tapi_ai_reply_free(tapi_ai_reply *reply);

/**
 * Close a connection.
 *
 * @param conn          Connection.
 */
extern void tapi_ai_conn_fini(tapi_ai_conn *conn);

/**
 * Spell out a provider.
 *
 * @param provider      The provider.
 *
 * @return A static string, never @c NULL.
 */
extern const char *tapi_ai_provider2str(tapi_ai_provider provider);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_AI_H__ */

/**@} <!-- END tapi_ai --> */
