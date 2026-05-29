/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Talking to an AI provider from a test
 */

#define TE_LGR_USER "TAPI AI"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_file.h"

#include "tapi_ai.h"
#include "tapi_ai_internal.h"

/* See description in tapi_ai.h */
unsigned int
tapi_ai_features(tapi_ai_provider provider)
{
    const tapi_ai_provider_info *info = tapi_ai_provider_info_get(provider);
    unsigned int feat = TAPI_AI_FEAT_CHAT | TAPI_AI_FEAT_SYSTEM |
                        TAPI_AI_FEAT_MULTITURN | TAPI_AI_FEAT_IMAGE |
                        TAPI_AI_FEAT_STREAM | TAPI_AI_FEAT_USAGE;

    if (info == NULL)
        return 0;

    /*
     * Documents (PDF) as an attachment are Anthropic's and OpenAI's;
     * Gemini takes them as inline data too, so all three shapes accept
     * a document - but only the two that were exercised claim it, so a
     * test skips rather than sends a PDF where it was never checked.
     */
    if (info->shape == TAPI_AI_SHAPE_ANTHROPIC ||
        info->shape == TAPI_AI_SHAPE_OPENAI ||
        info->shape == TAPI_AI_SHAPE_GEMINI)
    {
        feat |= TAPI_AI_FEAT_DOCUMENT;
    }

    return feat;
}

/* See description in tapi_ai.h */
bool
tapi_ai_supports(tapi_ai_provider provider, unsigned int feature)
{
    return (tapi_ai_features(provider) & feature) == feature;
}

/* See description in tapi_ai.h */
te_errno
tapi_ai_conn_init(tapi_job_factory_t *factory, tapi_ai_provider provider,
                  const char *base_url, const char *key, tapi_ai_conn *conn)
{
    const tapi_ai_provider_info *info = tapi_ai_provider_info_get(provider);

    memset(conn, 0, sizeof(*conn));

    if (info == NULL)
    {
        ERROR("Unknown AI provider %d", (int)provider);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }
    if (base_url == NULL && info->base_url == NULL)
    {
        ERROR("%s needs a base URL", tapi_ai_provider2str(provider));
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    conn->factory = factory;
    conn->provider = provider;
    conn->base_url = base_url != NULL ? TE_STRDUP(base_url) : NULL;
    conn->key = key != NULL ? TE_STRDUP(key) : NULL;
    conn->ta = tapi_job_factory_ta(factory);
    if (conn->ta == NULL)
    {
        ERROR("Cannot determine the agent behind the job factory");
        tapi_ai_conn_fini(conn);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    return 0;
}

/* See description in tapi_ai.h */
bool
tapi_ai_available(tapi_ai_conn *conn, int timeout_ms)
{
    const tapi_ai_provider_info *info = tapi_ai_provider_info_get(
                                                    conn->provider);
    const char *base = conn->base_url != NULL ? conn->base_url :
                       (info != NULL ? info->base_url : NULL);
    bool ok = false;

    if (base == NULL)
        return false;
    /* python3 must be there at all - putting the helper needs it. */
    if (tapi_ai_put_helper(conn) != 0)
        return false;

    /* Then the base URL's host must answer a TCP connect (no request). */
    if (tapi_ai_tcp_probe(conn, base, timeout_ms, &ok) != 0)
        return false;

    return ok;
}

/* See description in tapi_ai.h */
void
tapi_ai_message_init(tapi_ai_message *message, tapi_ai_role role,
                     const char *text)
{
    memset(message, 0, sizeof(*message));
    message->role = role;
    message->text = TE_STRDUP(text != NULL ? text : "");
    message->attachments = (te_vec)TE_VEC_INIT(tapi_ai_attachment);
}

/* See description in tapi_ai.h */
void
tapi_ai_message_free(tapi_ai_message *message)
{
    tapi_ai_attachment *att;

    TE_VEC_FOREACH(&message->attachments, att)
    {
        free(att->path);
        free(att->media_type);
    }
    te_vec_free(&message->attachments);
    free(message->text);
    memset(message, 0, sizeof(*message));
}

/* See description in tapi_ai.h */
void
tapi_ai_messages_free(te_vec *messages)
{
    tapi_ai_message *m;

    TE_VEC_FOREACH(messages, m)
        tapi_ai_message_free(m);
    te_vec_free(messages);
}

/**
 * Turn the helper's error line into a status code, and log it.
 */
static te_errno
ai_error_from(const char *out)
{
    const char *err = strstr(out, "\"t\":\"error\"");
    te_string message = TE_STRING_INIT;
    long status = 0;
    te_errno rc;

    if (err == NULL)
    {
        ERROR("The AI helper printed no result: %s", out);
        return TE_RC(TE_TAPI, TE_EPROTO);
    }

    tapi_ai_json_str(err, "message", &message);
    tapi_ai_json_int(err, "status", &status);
    ERROR("AI provider error (HTTP %ld): %s", status,
          te_string_value(&message));

    if (status == 401 || status == 403)
        rc = TE_RC(TE_TAPI, TE_EACCES);
    else if (status == -1)
        rc = TE_RC(TE_TAPI, TE_ECOMM);
    else
        rc = TE_RC(TE_TAPI, TE_ESHCMD);

    te_string_free(&message);

    return rc;
}

/** Read the single "final" line of a non-streamed result into a reply. */
static te_errno
ai_reply_from(const char *out, tapi_ai_reply *reply)
{
    const char *final = strstr(out, "\"t\":\"final\"");
    te_string s = TE_STRING_INIT;

    memset(reply, 0, sizeof(*reply));
    reply->usage.input_tokens = -1;
    reply->usage.output_tokens = -1;
    reply->usage.total_tokens = -1;

    if (final == NULL)
        return ai_error_from(out);

    if (tapi_ai_json_str(final, "text", &s))
        reply->text = TE_STRDUP(s.ptr);
    te_string_reset(&s);
    if (tapi_ai_json_str(final, "model", &s) && s.len != 0)
        reply->model = TE_STRDUP(s.ptr);
    te_string_reset(&s);
    if (tapi_ai_json_str(final, "finish_reason", &s) && s.len != 0)
        reply->finish_reason = TE_STRDUP(s.ptr);
    te_string_free(&s);

    tapi_ai_parse_usage(out, &reply->usage);

    if (reply->text == NULL)
        reply->text = TE_STRDUP("");

    return 0;
}

/* See description in tapi_ai.h */
te_errno
tapi_ai_chat(tapi_ai_conn *conn, const te_vec *messages,
             const tapi_ai_opts *opts, int timeout_ms, tapi_ai_reply *reply)
{
    te_string spec = TE_STRING_INIT;
    te_string out = TE_STRING_INIT;
    te_string err = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    memset(reply, 0, sizeof(*reply));

    rc = tapi_ai_put_helper(conn);
    if (rc != 0)
        return rc;

    rc = tapi_ai_build_spec(conn, messages, opts, false, &spec);
    if (rc != 0)
        goto out;

    rc = tapi_ai_run(conn, spec.ptr, timeout_ms, &out, &err, &code);
    if (rc != 0)
        goto out;

    if (code == TAPI_AI_EXIT_NOT_FOUND)
    {
        ERROR("There is no python3 on the agent to run the AI helper");
        rc = TE_RC(TE_TAPI, TE_ENOENT);
        goto out;
    }

    rc = ai_reply_from(te_string_value(&out), reply);

out:
    te_string_free(&spec);
    te_string_free(&out);
    te_string_free(&err);

    return rc;
}

/* See description in tapi_ai.h */
te_errno
tapi_ai_ask(tapi_ai_conn *conn, const char *system, const char *prompt,
            const tapi_ai_opts *opts, int timeout_ms, tapi_ai_reply *reply)
{
    te_vec messages = TE_VEC_INIT(tapi_ai_message);
    tapi_ai_opts local = TAPI_AI_OPTS_INIT;
    tapi_ai_message m;
    te_errno rc;

    if (opts != NULL)
        local = *opts;
    /* The system argument is a convenience; opts.system wins if set. */
    if (local.system == NULL)
        local.system = system;

    tapi_ai_message_init(&m, TAPI_AI_ROLE_USER, prompt);
    TE_VEC_APPEND(&messages, m);

    rc = tapi_ai_chat(conn, &messages, &local, timeout_ms, reply);

    tapi_ai_messages_free(&messages);

    return rc;
}

/* See description in tapi_ai.h */
void
tapi_ai_reply_log(const tapi_ai_reply *reply)
{
    RING("AI reply (%s, finish=%s): %d in / %d out / %d total tokens\n%s",
         reply->model != NULL ? reply->model : "?",
         reply->finish_reason != NULL ? reply->finish_reason : "?",
         reply->usage.input_tokens, reply->usage.output_tokens,
         reply->usage.total_tokens,
         reply->text != NULL ? reply->text : "");
}

/* See description in tapi_ai.h */
void
tapi_ai_reply_free(tapi_ai_reply *reply)
{
    free(reply->text);
    free(reply->model);
    free(reply->finish_reason);
    memset(reply, 0, sizeof(*reply));
}

/* See description in tapi_ai.h */
void
tapi_ai_conn_fini(tapi_ai_conn *conn)
{
    if (conn->helper != NULL && conn->ta != NULL)
        tapi_file_ta_unlink_fmt(conn->ta, "%s", conn->helper);
    free(conn->helper);
    free(conn->base_url);
    free(conn->key);
    memset(conn, 0, sizeof(*conn));
}
