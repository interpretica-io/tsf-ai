/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief AI TAPI: internal helpers
 *
 * Internal to tsf-ai; not installed.
 */

#ifndef __TSF_TAPI_AI_INTERNAL_H__
#define __TSF_TAPI_AI_INTERNAL_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"
#include "tapi_devtool_run.h"

#include "tapi_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Exit status of a POSIX shell that could not find the program. */
#define TAPI_AI_EXIT_NOT_FOUND 127

/** The embedded python helper source; see tapi_ai_helper.c (generated). */
extern const char tapi_ai_helper_py[];

/** The request shape a provider speaks on the wire. */
typedef enum tapi_ai_shape {
    /** OpenAI Chat Completions: @c /chat/completions, Bearer auth. */
    TAPI_AI_SHAPE_OPENAI = 0,
    /** Anthropic Messages: @c /v1/messages, @c x-api-key + version. */
    TAPI_AI_SHAPE_ANTHROPIC,
    /** Google Gemini: @c :generateContent, @c x-goog-api-key. */
    TAPI_AI_SHAPE_GEMINI,
} tapi_ai_shape;

/** What a provider is, resolved from #tapi_ai_provider. */
typedef struct tapi_ai_provider_info {
    /** Wire shape. */
    tapi_ai_shape shape;
    /** Default base URL, e.g. @c "https://api.openai.com/v1". */
    const char *base_url;
    /** A sensible default model when the caller gives none. */
    const char *default_model;
    /** The environment variable its SDK conventionally reads the key from. */
    const char *key_env;
} tapi_ai_provider_info;

/** Resolve a provider to its wire facts. */
extern const tapi_ai_provider_info *tapi_ai_provider_info_get(
                                                tapi_ai_provider provider);

/** Append one argument to a vector, taking ownership of it. */
extern void tapi_ai_arg(te_vec *args, const char *fmt, ...)
    TE_LIKE_PRINTF(2, 3);

/**
 * Put the AI helper (python3) on the agent, once per connection.
 *
 * @param[in]  conn     Connection.
 *
 * @return Status code.
 * @retval TE_ENOENT    There is no python3 on the agent.
 */
extern te_errno tapi_ai_put_helper(tapi_ai_conn *conn);

/**
 * Build the JSON request specification the helper consumes.
 *
 * It is not the provider's wire body: it is a neutral description -
 * provider, model, base URL, system, the messages with their
 * attachments, and the options - that the helper turns into whatever
 * the provider wants. Keeping the shaping in the helper is what lets
 * one code path serve OpenAI, Anthropic and Gemini.
 *
 * @param[in]  conn     Connection.
 * @param[in]  messages Vector of #tapi_ai_message.
 * @param[in]  opts     Options, or @c NULL.
 * @param[in]  stream   @c true to ask the helper to stream.
 * @param[out] spec     String to append the JSON to.
 *
 * @return Status code.
 */
extern te_errno tapi_ai_build_spec(const tapi_ai_conn *conn,
                                   const te_vec *messages,
                                   const tapi_ai_opts *opts, bool stream,
                                   te_string *spec);

/**
 * Run the helper with @p spec on its standard input and wait for it.
 *
 * The API key travels in the environment (@c TSF_AI_KEY), never on the
 * command line: a key in @c argv is visible to everyone who runs
 * @c ps on the agent.
 *
 * @param[in]  conn         Connection.
 * @param[in]  spec         The request JSON.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] out          Standard output (the helper's JSON result).
 * @param[out] err          Standard error, or @c NULL.
 * @param[out] exit_code    Exit status, or @c NULL.
 *
 * @return Status code of running the helper.
 */
extern te_errno tapi_ai_run(const tapi_ai_conn *conn, const char *spec,
                            int timeout_ms, te_string *out, te_string *err,
                            int *exit_code);

/**
 * Start the helper for a streaming request and leave it running.
 *
 * Writes @p spec to a file on the agent and starts the helper reading
 * it, without waiting. The caller drives @p run (expect/wait) and, when
 * done, removes @p spec_path and frees it.
 *
 * @param[in]  conn         Connection.
 * @param[in]  spec         The request JSON (with @c stream true).
 * @param[out] run          Run handle, started.
 * @param[out] spec_path    The spec file path on the agent (heap).
 *
 * @return Status code.
 */
extern te_errno tapi_ai_run_start(tapi_ai_conn *conn, const char *spec,
                                  tapi_devtool_run *run, char **spec_path);

/** Pull one JSON string field out of a flat object. */
extern bool tapi_ai_json_str(const char *object, const char *key,
                             te_string *dest);

/** Pull one JSON number field out of a flat object. */
extern bool tapi_ai_json_int(const char *object, const char *key,
                             long *value);

/** Append @p value to @p dest as a JSON string literal, quotes included. */
extern void tapi_ai_json_quote(te_string *dest, const char *value);

/**
 * Read the trailing @c "usage" line the helper prints.
 *
 * The helper ends its output with one line
 * @c {"tsf_usage":{"input":N,"output":M,...}}, so a streamed run - whose
 * earlier lines are text deltas - carries the token counts too.
 *
 * @param[in]  text     The helper's whole output.
 * @param[out] usage    Usage to fill; zeroed first.
 *
 * @return @c true when the usage line was found.
 */
extern bool tapi_ai_parse_usage(const char *text, tapi_ai_usage *usage);

/** The agent a connection runs on, for putting files there. */
extern const char *tapi_ai_conn_ta(const tapi_ai_conn *conn);

/**
 * Open a TCP connection to the host and port of @p url and close it,
 * to tell whether the endpoint is reachable. Sends nothing to it.
 *
 * @param[in]  conn         Connection.
 * @param[in]  url          The base URL.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] ok           @c true when the connection succeeded.
 *
 * @return Status code of running the probe.
 */
extern te_errno tapi_ai_tcp_probe(tapi_ai_conn *conn, const char *url,
                                  int timeout_ms, bool *ok);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_AI_INTERNAL_H__ */
