/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Attaching a file to an AI message
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
#include "tapi_cfg_base.h"
#include "tapi_file.h"
#include "rcf_api.h"

#include "tapi_ai.h"
#include "tapi_ai_file.h"
#include "tapi_ai_internal.h"

/** Guess a MIME type from a file name's extension. */
static const char *
ai_guess_media_type(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } map[] = {
        { ".png", "image/png" },
        { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },
        { ".webp", "image/webp" },
        { ".bmp", "image/bmp" },
        { ".pdf", "application/pdf" },
        { ".txt", "text/plain" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;

    if (dot == NULL)
        return NULL;

    for (i = 0; i < TE_ARRAY_LEN(map); i++)
    {
        if (strcasecmp(dot, map[i].ext) == 0)
            return map[i].type;
    }

    return NULL;
}

/* See description in tapi_ai_file.h */
te_errno
tapi_ai_attach(tapi_ai_message *message, const char *path,
               const char *media_type)
{
    tapi_ai_attachment att;

    if (media_type == NULL)
        media_type = ai_guess_media_type(path);
    if (media_type == NULL)
    {
        ERROR("Cannot guess the media type of %s; give one", path);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    att.path = TE_STRDUP(path);
    att.media_type = TE_STRDUP(media_type);
    TE_VEC_APPEND(&message->attachments, att);

    return 0;
}

/* See description in tapi_ai_file.h */
te_errno
tapi_ai_attach_from_engine(tapi_ai_conn *conn, tapi_ai_message *message,
                           const char *engine_path, const char *media_type)
{
    const char *ta = tapi_ai_conn_ta(conn);
    te_string remote = TE_STRING_INIT;
    const char *ext;
    char *tmp_dir;
    te_errno rc;

    if (ta == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    if (media_type == NULL)
        media_type = ai_guess_media_type(engine_path);
    if (media_type == NULL)
    {
        ERROR("Cannot guess the media type of %s; give one", engine_path);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    tmp_dir = tapi_cfg_base_get_ta_dir(ta, TAPI_CFG_BASE_TA_DIR_TMP);
    if (tmp_dir == NULL)
        return TE_RC(TE_TAPI, TE_EFAIL);
    /* Keep the extension so a guesser on the far side still works. */
    ext = strrchr(engine_path, '.');
    tapi_file_make_custom_pathname(&remote, tmp_dir, ext);
    free(tmp_dir);

    rc = rcf_ta_put_file(ta, 0, engine_path, remote.ptr);
    if (rc != 0)
    {
        ERROR("Failed to put %s on TA %s: %r", engine_path, ta, rc);
        te_string_free(&remote);
        return rc;
    }

    rc = tapi_ai_attach(message, remote.ptr, media_type);
    te_string_free(&remote);

    return rc;
}

/* See description in tapi_ai_file.h */
te_errno
tapi_ai_ask_with_file(tapi_ai_conn *conn, const char *system,
                      const char *prompt, const char *path,
                      const char *media_type, const tapi_ai_opts *opts,
                      int timeout_ms, tapi_ai_reply *reply)
{
    te_vec messages = TE_VEC_INIT(tapi_ai_message);
    tapi_ai_opts local = TAPI_AI_OPTS_INIT;
    tapi_ai_message m;
    const char *mt = media_type;
    unsigned int need;
    te_errno rc;

    if (mt == NULL)
        mt = ai_guess_media_type(path);
    need = (mt != NULL && strncmp(mt, "image/", 6) == 0) ?
           TAPI_AI_FEAT_IMAGE : TAPI_AI_FEAT_DOCUMENT;
    if (!tapi_ai_supports(conn->provider, need))
    {
        ERROR("%s does not take a %s attachment",
              tapi_ai_provider2str(conn->provider),
              need == TAPI_AI_FEAT_IMAGE ? "image" : "document");
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    if (opts != NULL)
        local = *opts;
    if (local.system == NULL)
        local.system = system;

    tapi_ai_message_init(&m, TAPI_AI_ROLE_USER, prompt);
    rc = tapi_ai_attach(&m, path, media_type);
    if (rc == 0)
    {
        TE_VEC_APPEND(&messages, m);
        rc = tapi_ai_chat(conn, &messages, &local, timeout_ms, reply);
    }
    else
    {
        tapi_ai_message_free(&m);
    }

    tapi_ai_messages_free(&messages);

    return rc;
}
