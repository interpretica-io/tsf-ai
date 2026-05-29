/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Attaching a file to an AI message
 *
 * @defgroup tapi_ai_file File attachments
 * @ingroup tapi_ai
 * @{
 *
 * A message can carry files - an image for a vision model, a PDF for a
 * document one. The file is on the agent; the helper reads it there,
 * base64-encodes it and places it in the shape the provider wants (a
 * data URL for OpenAI, a @c source block for Anthropic, @c inline_data
 * for Gemini). tapi_ai_attach_from_engine() copies a file the test
 * carries with it onto the agent first.
 *
 * @code
 * tapi_ai_message msg;
 * tapi_ai_reply reply;
 * te_vec conv = TE_VEC_INIT(tapi_ai_message);
 *
 * tapi_ai_message_init(&msg, TAPI_AI_ROLE_USER, "What is in this image?");
 * CHECK_RC(tapi_ai_attach(&msg, "/tmp/chart.png", "image/png"));
 * TE_VEC_APPEND(&conv, msg);
 * CHECK_RC(tapi_ai_chat(&conn, &conv, NULL, 60000, &reply));
 * @endcode
 */

#ifndef __TSF_TAPI_AI_FILE_H__
#define __TSF_TAPI_AI_FILE_H__

#include "te_defs.h"
#include "te_errno.h"
#include "tapi_job.h"

#include "tapi_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attach a file that is on the agent to a message.
 *
 * @param[in,out] message   The message.
 * @param[in]     path      Path to the file on the agent.
 * @param[in]     media_type MIME type, e.g. @c "image/png". When
 *                          @c NULL it is guessed from the extension.
 *
 * @return Status code.
 * @retval TE_EINVAL        The type could not be guessed and none given.
 */
extern te_errno tapi_ai_attach(tapi_ai_message *message, const char *path,
                               const char *media_type);

/**
 * Copy a file from the engine to the agent and attach it.
 *
 * @param[in]     conn        Connection (for the agent).
 * @param[in,out] message     The message.
 * @param[in]     engine_path File on the engine.
 * @param[in]     media_type  MIME type, or @c NULL to guess.
 *
 * @return Status code.
 */
extern te_errno tapi_ai_attach_from_engine(tapi_ai_conn *conn,
                                           tapi_ai_message *message,
                                           const char *engine_path,
                                           const char *media_type);

/**
 * Ask once with one attached file - the common vision/document case.
 *
 * @param[in]  conn         Connection.
 * @param[in]  system       System prompt, or @c NULL.
 * @param[in]  prompt       The user's message.
 * @param[in]  path         File on the agent to attach.
 * @param[in]  media_type   MIME type, or @c NULL to guess.
 * @param[in]  opts         Options, or @c NULL.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] reply        The answer; release with tapi_ai_reply_free().
 *
 * @return Status code.
 * @retval TE_EOPNOTSUPP    The provider takes no files of that kind.
 */
extern te_errno tapi_ai_ask_with_file(tapi_ai_conn *conn,
                                      const char *system,
                                      const char *prompt, const char *path,
                                      const char *media_type,
                                      const tapi_ai_opts *opts,
                                      int timeout_ms, tapi_ai_reply *reply);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_AI_FILE_H__ */

/**@} <!-- END tapi_ai_file --> */
