/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Streaming an AI answer
 *
 * The helper prints one JSON line per event as it reads the provider's
 * server-sent events: @c delta lines with text, then a @c final line
 * with the token usage, or an @c error line. This side keeps the helper
 * running and reads its standard output line by line, waiting for a
 * newline with tapi_devtool_run_expect() and pulling each complete line
 * out of the accumulated output.
 */

#define TE_LGR_USER "TAPI AI"

#include "te_config.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_file.h"

#include "tapi_ai.h"
#include "tapi_ai_stream.h"
#include "tapi_ai_internal.h"

/** Time given to the helper to terminate when the session is freed, ms. */
#define AI_STREAM_TERM_MS 2000

/** Start a streaming helper for a built spec. */
static te_errno
ai_stream_start_spec(tapi_ai_conn *conn, const te_string *spec,
                     tapi_ai_stream *stream)
{
    te_errno rc;

    memset(stream, 0, sizeof(*stream));
    stream->conn = conn;

    rc = tapi_ai_put_helper(conn);
    if (rc != 0)
        return rc;

    return tapi_ai_run_start(conn, spec->ptr, &stream->run,
                             &stream->spec_path);
}

/* See description in tapi_ai_stream.h */
te_errno
tapi_ai_stream_start_chat(tapi_ai_conn *conn, const te_vec *messages,
                          const tapi_ai_opts *opts, tapi_ai_stream *stream)
{
    te_string spec = TE_STRING_INIT;
    te_errno rc;

    if (!tapi_ai_supports(conn->provider, TAPI_AI_FEAT_STREAM))
    {
        ERROR("%s does not stream",
              tapi_ai_provider2str(conn->provider));
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    memset(stream, 0, sizeof(*stream));

    rc = tapi_ai_put_helper(conn);
    if (rc == 0)
        rc = tapi_ai_build_spec(conn, messages, opts, true, &spec);
    if (rc == 0)
        rc = ai_stream_start_spec(conn, &spec, stream);

    te_string_free(&spec);

    return rc;
}

/* See description in tapi_ai_stream.h */
te_errno
tapi_ai_stream_start(tapi_ai_conn *conn, const char *prompt,
                     const tapi_ai_opts *opts, tapi_ai_stream *stream)
{
    te_vec messages = TE_VEC_INIT(tapi_ai_message);
    tapi_ai_message m;
    te_errno rc;

    tapi_ai_message_init(&m, TAPI_AI_ROLE_USER, prompt);
    TE_VEC_APPEND(&messages, m);

    rc = tapi_ai_stream_start_chat(conn, &messages, opts, stream);

    tapi_ai_messages_free(&messages);

    return rc;
}

/**
 * Classify one event line: a delta hands back its text; a final or an
 * error line ends the stream.
 *
 * @return @c 0 for a delta (text appended to @p piece),
 *         @ref TE_ENODATA for a terminal line, and a negative sentinel
 *         (@c -1) for a line that is neither (ignored).
 */
static te_errno
ai_stream_classify(tapi_ai_stream *stream, const char *line, size_t len,
                   te_string *piece)
{
    char *buf = TE_STRNDUP(line, len);
    te_errno rc = TE_RC(TE_TAPI, TE_EAGAIN);   /* "not an event line" */

    if (strstr(buf, "\"t\":\"delta\"") != NULL)
    {
        if (piece != NULL)
            tapi_ai_json_str(buf, "text", piece);
        rc = 0;
    }
    else if (strstr(buf, "\"t\":\"final\"") != NULL)
    {
        stream->done = true;
        rc = TE_RC(TE_TAPI, TE_ENODATA);
    }
    else if (strstr(buf, "\"t\":\"error\"") != NULL)
    {
        stream->done = true;
        stream->errored = true;
        rc = TE_RC(TE_TAPI, TE_ENODATA);
    }

    free(buf);

    return rc;
}

/**
 * Pull the next complete line (up to a newline) out of the helper's
 * accumulated output, advancing the cursor.
 *
 * @return @c true when a line was available.
 */
static bool
ai_stream_take_line(tapi_ai_stream *stream, const char **line, size_t *len,
                    bool allow_partial)
{
    const char *buf = te_string_value(&stream->run.out);
    size_t total = stream->run.out.len;
    const char *start;
    const char *nl;

    if (stream->cursor >= total)
        return false;

    start = buf + stream->cursor;
    nl = memchr(start, '\n', total - stream->cursor);
    if (nl != NULL)
    {
        *line = start;
        *len = (size_t)(nl - start);
        stream->cursor = (size_t)(nl - buf) + 1;
        return true;
    }
    if (allow_partial)
    {
        *line = start;
        *len = total - stream->cursor;
        stream->cursor = total;
        return *len != 0;
    }

    return false;
}

/* See description in tapi_ai_stream.h */
te_errno
tapi_ai_stream_next(tapi_ai_stream *stream, int timeout_ms, te_string *piece)
{
    for (;;)
    {
        const char *line;
        size_t len;
        te_errno rc;

        /* A complete line already buffered? Classify it. */
        while (ai_stream_take_line(stream, &line, &len, false))
        {
            rc = ai_stream_classify(stream, line, len, piece);
            if (TE_RC_GET_ERROR(rc) == TE_EAGAIN)
                continue;   /* not an event line - skip it */
            return rc;      /* 0 for a delta, ENODATA for terminal */
        }

        if (stream->done)
            return TE_RC(TE_TAPI, TE_ENODATA);

        /* Need more: wait for the next newline on the helper's stdout. */
        rc = tapi_devtool_run_expect(&stream->run, "\n", timeout_ms);
        if (rc == 0)
            continue;

        /*
         * expect() returns TE_ETIMEDOUT both on a real timeout and on
         * the stream ending. Tell them apart by asking whether the
         * helper has exited.
         */
        if (tapi_devtool_run_wait(&stream->run, 0) == 0)
        {
            /* Ended: a last line may have no trailing newline. */
            if (ai_stream_take_line(stream, &line, &len, true))
            {
                rc = ai_stream_classify(stream, line, len, piece);
                if (rc == 0)
                    return 0;
            }
            stream->done = true;
            return TE_RC(TE_TAPI, TE_ENODATA);
        }

        return TE_RC(TE_TAPI, TE_ETIMEDOUT);
    }
}

/* See description in tapi_ai_stream.h */
te_errno
tapi_ai_stream_collect(tapi_ai_stream *stream, int timeout_ms,
                       te_string *text)
{
    te_string piece = TE_STRING_INIT;
    te_errno rc;

    for (;;)
    {
        te_string_reset(&piece);
        rc = tapi_ai_stream_next(stream, timeout_ms, &piece);
        if (rc == 0)
        {
            te_string_append(text, "%s", te_string_value(&piece));
            continue;
        }
        break;
    }

    te_string_free(&piece);

    /* ENODATA is the normal end of a stream, not a failure. */
    return TE_RC_GET_ERROR(rc) == TE_ENODATA ? 0 : rc;
}

/* See description in tapi_ai_stream.h */
te_errno
tapi_ai_stream_finish(tapi_ai_stream *stream, tapi_ai_usage *usage)
{
    /* Drain anything left, so the final line's usage is in the output. */
    if (!stream->done)
    {
        te_string sink = TE_STRING_INIT;

        tapi_ai_stream_collect(stream, TAPI_AI_TIMEOUT_MS, &sink);
        te_string_free(&sink);
    }

    /* Make sure the process is reaped. */
    if (tapi_devtool_run_wait(&stream->run, AI_STREAM_TERM_MS) ==
        TE_RC(TE_TAPI, TE_EINPROGRESS))
    {
        tapi_devtool_run_kill(&stream->run, SIGTERM);
        tapi_devtool_run_wait(&stream->run, AI_STREAM_TERM_MS);
    }

    if (usage != NULL)
        tapi_ai_parse_usage(te_string_value(&stream->run.out), usage);

    if (stream->errored)
    {
        ERROR("The stream ended in an error: %s",
              te_string_value(&stream->run.out));
        return TE_RC(TE_TAPI, TE_ESHCMD);
    }

    return 0;
}

/* See description in tapi_ai_stream.h */
void
tapi_ai_stream_free(tapi_ai_stream *stream)
{
    if (stream->run.job != NULL)
    {
        if (tapi_devtool_run_wait(&stream->run, 0) ==
            TE_RC(TE_TAPI, TE_EINPROGRESS))
        {
            tapi_devtool_run_kill(&stream->run, SIGKILL);
            tapi_devtool_run_wait(&stream->run, AI_STREAM_TERM_MS);
        }
        tapi_devtool_run_fini(&stream->run);
    }

    if (stream->spec_path != NULL && stream->conn != NULL &&
        stream->conn->ta != NULL)
    {
        tapi_file_ta_unlink_fmt(stream->conn->ta, "%s", stream->spec_path);
    }
    free(stream->spec_path);
    memset(stream, 0, sizeof(*stream));
}
