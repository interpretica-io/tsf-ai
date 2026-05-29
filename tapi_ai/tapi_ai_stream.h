/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Streaming an AI answer
 *
 * @defgroup tapi_ai_stream Streaming sessions
 * @ingroup tapi_ai
 * @{
 *
 * The answer arriving as it is generated rather than all at once. The
 * helper opens the provider's streaming endpoint (server-sent events),
 * turns each provider's delta format into plain text, and prints the
 * pieces as they come; this side reads them and hands them to the test.
 * The token counts arrive at the end, the same as a non-streamed
 * request.
 *
 * A test uses a stream for two reasons: to assert the answer *begins*
 * before it is whole - that time-to-first-token is real, that a long
 * answer is not buffered - and to watch a long generation without
 * waiting for a timeout on the whole of it.
 *
 * @code
 * tapi_ai_stream stream;
 * te_string piece = TE_STRING_INIT;
 * te_string whole = TE_STRING_INIT;
 *
 * CHECK_RC(tapi_ai_stream_start(&conn, "Count to twenty.", NULL,
 *                               &stream));
 * while (tapi_ai_stream_next(&stream, 10000, &piece) == 0)
 * {
 *     RING("delta: %s", piece.ptr);
 *     te_string_append(&whole, "%s", piece.ptr);
 *     te_string_reset(&piece);
 * }
 * CHECK_RC(tapi_ai_stream_finish(&stream, &usage));
 * tapi_ai_stream_free(&stream);
 * @endcode
 */

#ifndef __TSF_TAPI_AI_STREAM_H__
#define __TSF_TAPI_AI_STREAM_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "tapi_job.h"

#include "tapi_ai.h"
#include "tapi_devtool_run.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A running streaming session. */
typedef struct tapi_ai_stream {
    /** The helper process, kept running while the answer streams. */
    tapi_devtool_run run;
    /** The connection it belongs to. */
    tapi_ai_conn *conn;
    /** How far into the helper's output the events have been consumed. */
    size_t cursor;
    /** @c true once the stream has ended. */
    bool done;
    /** @c true when the stream ended in a provider/transport error. */
    bool errored;
    /** The request spec file on the agent, removed on free. */
    char *spec_path;
} tapi_ai_stream;

/**
 * Start a streaming request for one prompt.
 *
 * @param[in]  conn         Connection.
 * @param[in]  prompt       The user's message.
 * @param[in]  opts         Options, or @c NULL.
 * @param[out] stream       The session; release with tapi_ai_stream_free().
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The provider does not stream.
 */
extern te_errno tapi_ai_stream_start(tapi_ai_conn *conn, const char *prompt,
                                     const tapi_ai_opts *opts,
                                     tapi_ai_stream *stream);

/**
 * Start a streaming request for a built conversation.
 *
 * @param[in]  conn         Connection.
 * @param[in]  messages     Vector of #tapi_ai_message.
 * @param[in]  opts         Options, or @c NULL.
 * @param[out] stream       The session.
 *
 * @return Status code.
 */
extern te_errno tapi_ai_stream_start_chat(tapi_ai_conn *conn,
                                          const te_vec *messages,
                                          const tapi_ai_opts *opts,
                                          tapi_ai_stream *stream);

/**
 * Wait for the next piece of the answer.
 *
 * @param[in]  stream       The session.
 * @param[in]  timeout_ms   How long to wait for the next piece, ms.
 * @param[out] piece        String to append the text to; reset it
 *                          first if you want only the new piece.
 *
 * @return Status code.
 * @retval 0                A piece arrived.
 * @retval TE_ENODATA       The answer is complete; call
 *                          tapi_ai_stream_finish().
 * @retval TE_ETIMEDOUT     Nothing arrived in @p timeout_ms, and the
 *                          answer is not done - the caller decides
 *                          whether to keep waiting.
 */
extern te_errno tapi_ai_stream_next(tapi_ai_stream *stream, int timeout_ms,
                                    te_string *piece);

/**
 * Read the whole answer at once, waiting for the stream to finish.
 *
 * For when a test wants a stream on the wire but not the pieces.
 *
 * @param[in]  stream       The session.
 * @param[in]  timeout_ms   Overall timeout, ms.
 * @param[out] text         String to append the whole answer to.
 *
 * @return Status code.
 */
extern te_errno tapi_ai_stream_collect(tapi_ai_stream *stream,
                                       int timeout_ms, te_string *text);

/**
 * Finish a stream and read the token usage.
 *
 * Call once tapi_ai_stream_next() returned @ref TE_ENODATA, or after
 * tapi_ai_stream_collect().
 *
 * @param[in]  stream       The session.
 * @param[out] usage        Token usage, or @c NULL.
 *
 * @return Status code.
 * @retval TE_ESHCMD        The stream ended in a provider error.
 */
extern te_errno tapi_ai_stream_finish(tapi_ai_stream *stream,
                                      tapi_ai_usage *usage);

/**
 * Release a streaming session, stopping the helper if it still runs.
 *
 * @param stream        The session.
 */
extern void tapi_ai_stream_free(tapi_ai_stream *stream);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_AI_STREAM_H__ */

/**@} <!-- END tapi_ai_stream --> */
