/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief AI TAPI: the helper, the request spec, and reading the result
 *
 * The provider differences live in the python helper; this file puts it
 * on the agent, builds the neutral request spec it consumes, runs it
 * with the key in the environment, and reads back the small JSON the
 * helper prints.
 */

#define TE_LGR_USER "TAPI AI"

#include "te_config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_cfg_base.h"
#include "tapi_file.h"
#include "tapi_job_opt.h"

#include "tapi_devtool_run.h"
#include "tapi_ai.h"
#include "tapi_ai_internal.h"

/* See description in tapi_ai.h */
const char *
tapi_ai_provider2str(tapi_ai_provider provider)
{
    switch (provider)
    {
        case TAPI_AI_OPENAI:
            return "openai";
        case TAPI_AI_OPENAI_COMPATIBLE:
            return "openai-compatible";
        case TAPI_AI_XAI:
            return "xai";
        case TAPI_AI_ANTHROPIC:
            return "anthropic";
        case TAPI_AI_GEMINI:
            return "gemini";
        default:
            return "unknown";
    }
}

/* See description in tapi_ai_internal.h */
const tapi_ai_provider_info *
tapi_ai_provider_info_get(tapi_ai_provider provider)
{
    static const tapi_ai_provider_info table[] = {
        [TAPI_AI_OPENAI] = {
            .shape = TAPI_AI_SHAPE_OPENAI,
            .base_url = "https://api.openai.com/v1",
            .default_model = "gpt-4o-mini",
            .key_env = "OPENAI_API_KEY",
        },
        [TAPI_AI_OPENAI_COMPATIBLE] = {
            .shape = TAPI_AI_SHAPE_OPENAI,
            .base_url = NULL,   /* the caller must give one */
            .default_model = NULL,
            .key_env = "OPENAI_API_KEY",
        },
        [TAPI_AI_XAI] = {
            .shape = TAPI_AI_SHAPE_OPENAI,
            .base_url = "https://api.x.ai/v1",
            .default_model = "grok-2-latest",
            .key_env = "XAI_API_KEY",
        },
        [TAPI_AI_ANTHROPIC] = {
            .shape = TAPI_AI_SHAPE_ANTHROPIC,
            .base_url = "https://api.anthropic.com",
            .default_model = "claude-3-5-sonnet-latest",
            .key_env = "ANTHROPIC_API_KEY",
        },
        [TAPI_AI_GEMINI] = {
            .shape = TAPI_AI_SHAPE_GEMINI,
            .base_url = "https://generativelanguage.googleapis.com/v1beta",
            .default_model = "gemini-1.5-flash",
            .key_env = "GEMINI_API_KEY",
        },
    };

    if ((unsigned)provider >= TE_ARRAY_LEN(table))
        return NULL;

    return &table[provider];
}

/** The helper's @c shape string for a provider. */
static const char *
ai_shape_str(tapi_ai_provider provider)
{
    switch (tapi_ai_provider_info_get(provider)->shape)
    {
        case TAPI_AI_SHAPE_ANTHROPIC:
            return "anthropic";
        case TAPI_AI_SHAPE_GEMINI:
            return "gemini";
        default:
            return "openai";
    }
}

/* See description in tapi_ai_internal.h */
void
tapi_ai_arg(te_vec *args, const char *fmt, ...)
{
    te_string built = TE_STRING_INIT;
    char *arg;
    va_list ap;

    va_start(ap, fmt);
    te_string_append_va(&built, fmt, ap);
    va_end(ap);

    arg = built.ptr != NULL ? built.ptr : TE_STRDUP("");
    TE_VEC_APPEND(args, arg);
}

/* See description in tapi_ai_internal.h */
const char *
tapi_ai_conn_ta(const tapi_ai_conn *conn)
{
    if (conn->ta == NULL)
        ERROR("Cannot determine the agent behind the job factory");

    return conn->ta;
}

/* See description in tapi_ai_internal.h */
void
tapi_ai_json_quote(te_string *dest, const char *value)
{
    te_string_append(dest, "\"");
    for (; value != NULL && *value != '\0'; value++)
    {
        unsigned char c = (unsigned char)*value;

        switch (c)
        {
            case '"':
                te_string_append(dest, "\\\"");
                break;
            case '\\':
                te_string_append(dest, "\\\\");
                break;
            case '\n':
                te_string_append(dest, "\\n");
                break;
            case '\r':
                te_string_append(dest, "\\r");
                break;
            case '\t':
                te_string_append(dest, "\\t");
                break;
            default:
                if (c < 0x20)
                    te_string_append(dest, "\\u%04x", c);
                else
                    te_string_append(dest, "%c", c);
                break;
        }
    }
    te_string_append(dest, "\"");
}

/* See description in tapi_ai_internal.h */
te_errno
tapi_ai_put_helper(tapi_ai_conn *conn)
{
    const char *ta = tapi_ai_conn_ta(conn);
    te_string path = TE_STRING_INIT;
    char *tmp_dir;
    te_errno rc;

    if (conn->helper != NULL)
        return 0;
    if (ta == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    tmp_dir = tapi_cfg_base_get_ta_dir(ta, TAPI_CFG_BASE_TA_DIR_TMP);
    if (tmp_dir == NULL)
    {
        ERROR("Failed to get the temporary directory of TA %s", ta);
        return TE_RC(TE_TAPI, TE_EFAIL);
    }
    tapi_file_make_custom_pathname(&path, tmp_dir, "-ai.py");
    free(tmp_dir);

    rc = tapi_file_create_ta(ta, path.ptr, "%s", tapi_ai_helper_py);
    if (rc != 0)
    {
        ERROR("Failed to put the AI helper on TA %s: %r", ta, rc);
        te_string_free(&path);
        return rc;
    }

    conn->helper = path.ptr;

    return 0;
}

/** Append one message object to the spec's messages array. */
static void
ai_spec_message(const tapi_ai_message *m, te_string *spec)
{
    const tapi_ai_attachment *att;
    bool first = true;

    te_string_append(spec, "{\"role\":\"%s\",\"text\":",
                     m->role == TAPI_AI_ROLE_ASSISTANT ? "assistant" :
                     "user");
    tapi_ai_json_quote(spec, m->text != NULL ? m->text : "");
    te_string_append(spec, ",\"attachments\":[");
    TE_VEC_FOREACH(&m->attachments, att)
    {
        te_string_append(spec, "%s{\"path\":", first ? "" : ",");
        tapi_ai_json_quote(spec, att->path);
        te_string_append(spec, ",\"media_type\":");
        tapi_ai_json_quote(spec, att->media_type);
        te_string_append(spec, "}");
        first = false;
    }
    te_string_append(spec, "]}");
}

/* See description in tapi_ai_internal.h */
te_errno
tapi_ai_build_spec(const tapi_ai_conn *conn, const te_vec *messages,
                   const tapi_ai_opts *opts, bool stream, te_string *spec)
{
    const tapi_ai_provider_info *info = tapi_ai_provider_info_get(
                                                    conn->provider);
    const char *base = conn->base_url != NULL ? conn->base_url :
                       info->base_url;
    const char *model = opts != NULL && opts->model != NULL ? opts->model :
                        info->default_model;
    const tapi_ai_message *m;
    bool first = true;

    if (base == NULL)
    {
        ERROR("No base URL for %s", tapi_ai_provider2str(conn->provider));
        return TE_RC(TE_TAPI, TE_EINVAL);
    }
    if (model == NULL)
    {
        ERROR("No model given and %s has no default",
              tapi_ai_provider2str(conn->provider));
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    te_string_append(spec, "{\"shape\":\"%s\",\"base_url\":",
                     ai_shape_str(conn->provider));
    tapi_ai_json_quote(spec, base);
    te_string_append(spec, ",\"model\":");
    tapi_ai_json_quote(spec, model);
    te_string_append(spec, ",\"stream\":%s", stream ? "true" : "false");

    if (opts != NULL && opts->system != NULL)
    {
        te_string_append(spec, ",\"system\":");
        tapi_ai_json_quote(spec, opts->system);
    }
    if (opts != NULL && opts->max_tokens != 0)
        te_string_append(spec, ",\"max_tokens\":%u", opts->max_tokens);
    if (opts != NULL && opts->temperature_pct >= 0)
    {
        te_string_append(spec, ",\"temperature\":%d.%02d",
                         opts->temperature_pct / 100,
                         opts->temperature_pct % 100);
    }
    if (opts != NULL && opts->json_mode)
        te_string_append(spec, ",\"json_mode\":true");

    te_string_append(spec, ",\"messages\":[");
    TE_VEC_FOREACH((te_vec *)messages, m)
    {
        if (!first)
            te_string_append(spec, ",");
        ai_spec_message(m, spec);
        first = false;
    }
    te_string_append(spec, "]}");

    return 0;
}

/** A plain argument vector for tapi_devtool_run. */
typedef struct ai_args {
    size_t n_args;
    const char **args;
} ai_args;

static const tapi_job_opt_bind ai_arg_binds[] = TAPI_JOB_OPT_SET(
    TAPI_JOB_OPT_ARRAY_PTR(ai_args, n_args, args,
        TAPI_JOB_OPT_CONTENT(TAPI_JOB_OPT_STRING, NULL, false))
);

/**
 * Write @p spec to a temp file on the agent and build the argv/env that
 * runs the helper reading it. Shared by the one-shot and streaming
 * paths. On success @p *spec_path is the heap path of the spec file.
 */
static te_errno
ai_prepare(const tapi_ai_conn *conn, const char *spec, te_vec *args,
           te_string *passwd, const char **env, char **spec_path)
{
    const char *ta = conn->ta;
    te_string path = TE_STRING_INIT;
    char *tmp_dir;
    size_t n_env = 0;
    te_errno rc;

    tmp_dir = tapi_cfg_base_get_ta_dir(ta, TAPI_CFG_BASE_TA_DIR_TMP);
    if (tmp_dir == NULL)
        return TE_RC(TE_TAPI, TE_EFAIL);
    tapi_file_make_custom_pathname(&path, tmp_dir, "-ai-req.json");
    free(tmp_dir);

    rc = tapi_file_create_ta(ta, path.ptr, "%s", spec);
    if (rc != 0)
    {
        ERROR("Failed to put the AI request on TA %s: %r", ta, rc);
        te_string_free(&path);
        return rc;
    }

    /* Key in the environment (TSF_AI_KEY), never in argv (ps sees it). */
    env[n_env++] = "PATH=/usr/bin:/bin:/usr/local/bin";
    if (conn->key != NULL)
    {
        te_string_append(passwd, "TSF_AI_KEY=%s", conn->key);
        env[n_env++] = te_string_value(passwd);
    }
    env[n_env] = NULL;

    tapi_ai_arg(args, "-c");
    tapi_ai_arg(args, "exec python3 \"$0\" < \"$1\"");
    tapi_ai_arg(args, "%s", conn->helper);
    tapi_ai_arg(args, "%s", path.ptr);

    *spec_path = path.ptr;

    return 0;
}

/* See description in tapi_ai_internal.h */
te_errno
tapi_ai_run_start(tapi_ai_conn *conn, const char *spec,
                  tapi_devtool_run *run, char **spec_path)
{
    te_vec args = TE_VEC_INIT(char *);
    te_string passwd = TE_STRING_INIT;
    const char *env[3] = { NULL, NULL, NULL };
    ai_args opt;
    te_errno rc;

    *spec_path = NULL;
    *run = (tapi_devtool_run)TAPI_DEVTOOL_RUN_INIT;

    if (conn->ta == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    rc = ai_prepare(conn, spec, &args, &passwd, env, spec_path);
    if (rc != 0)
        goto out;

    opt.n_args = te_vec_size(&args);
    opt.args = (const char **)te_vec_get(&args, 0);

    rc = tapi_devtool_run_init_env(run, conn->factory, "ai", "/bin/sh",
                                   ai_arg_binds, &opt, NULL, env);
    if (rc == 0)
        rc = tapi_devtool_run_start(run);
    if (rc != 0)
    {
        tapi_devtool_run_fini(run);
        if (*spec_path != NULL)
            tapi_file_ta_unlink_fmt(conn->ta, "%s", *spec_path);
        free(*spec_path);
        *spec_path = NULL;
    }

out:
    te_vec_deep_free(&args);
    te_string_free(&passwd);

    return rc;
}

/* See description in tapi_ai_internal.h */
te_errno
tapi_ai_run(const tapi_ai_conn *conn, const char *spec, int timeout_ms,
            te_string *out, te_string *err, int *exit_code)
{
    const char *ta = tapi_ai_conn_ta(conn);
    te_string passwd = TE_STRING_INIT;
    te_vec args = TE_VEC_INIT(char *);
    const char *env[3] = { NULL, NULL, NULL };
    tapi_devtool_output output;
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    char *spec_path = NULL;
    ai_args opt;
    te_errno rc;

    if (ta == NULL)
        return TE_RC(TE_TAPI, TE_EINVAL);

    /*
     * The spec goes to the agent as a file and is fed to the helper on
     * stdin, not as an argument: a request can be large and argv has
     * limits. ai_prepare writes the file, builds the argv and the env
     * (the key in TSF_AI_KEY, never in argv).
     */
    rc = ai_prepare(conn, spec, &args, &passwd, env, &spec_path);
    if (rc != 0)
        goto out;

    opt.n_args = te_vec_size(&args);
    opt.args = (const char **)te_vec_get(&args, 0);

    rc = tapi_devtool_run_init_env(&run, conn->factory, "ai", "/bin/sh",
                                   ai_arg_binds, &opt, NULL, env);
    if (rc == 0)
        rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);
    if (rc != 0)
    {
        if (TE_RC_GET_ERROR(rc) == TE_EINPROGRESS)
            rc = TE_RC(TE_TAPI, TE_ETIMEDOUT);
        tapi_devtool_run_fini(&run);
        goto out;
    }

    tapi_devtool_run_get_output(&run, &output);
    if (out != NULL && output.out != NULL)
        te_string_append(out, "%s", output.out);
    if (err != NULL && output.err != NULL)
        te_string_append(err, "%s", output.err);
    if (exit_code != NULL)
    {
        *exit_code = output.status.type == TAPI_JOB_STATUS_EXITED ?
                     output.status.value : -1;
    }

    rc = tapi_devtool_run_fini(&run);

out:
    if (spec_path != NULL)
        tapi_file_ta_unlink_fmt(ta, "%s", spec_path);
    free(spec_path);
    te_string_free(&passwd);
    te_vec_deep_free(&args);

    return rc;
}

/**
 * A python one-liner that opens a TCP connection to the host and port
 * of its argument (a URL) and exits 0 on success. Sends nothing.
 */
static const char ai_probe_py[] =
    "import sys,socket,urllib.parse as u;"
    "p=u.urlsplit(sys.argv[1]);"
    "port=p.port or (443 if p.scheme=='https' else 80);"
    "socket.create_connection((p.hostname,port),5).close()";

/* See description in tapi_ai_internal.h */
te_errno
tapi_ai_tcp_probe(tapi_ai_conn *conn, const char *url, int timeout_ms,
                  bool *ok)
{
    te_vec args = TE_VEC_INIT(char *);
    const char *env[2] = { "PATH=/usr/bin:/bin:/usr/local/bin", NULL };
    tapi_devtool_output output;
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    ai_args opt;
    te_errno rc;

    *ok = false;

    tapi_ai_arg(&args, "-c");
    tapi_ai_arg(&args, "exec python3 -c \"$0\" \"$1\"");
    tapi_ai_arg(&args, "%s", ai_probe_py);
    tapi_ai_arg(&args, "%s", url);

    opt.n_args = te_vec_size(&args);
    opt.args = (const char **)te_vec_get(&args, 0);

    rc = tapi_devtool_run_init_env(&run, conn->factory, "ai-probe",
                                   "/bin/sh", ai_arg_binds, &opt, NULL, env);
    if (rc == 0)
        rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);
    if (rc == 0)
    {
        tapi_devtool_run_get_output(&run, &output);
        *ok = output.status.type == TAPI_JOB_STATUS_EXITED &&
              output.status.value == 0;
    }
    else if (TE_RC_GET_ERROR(rc) == TE_EINPROGRESS)
    {
        rc = TE_RC(TE_TAPI, TE_ETIMEDOUT);
    }

    tapi_devtool_run_fini(&run);
    te_vec_deep_free(&args);

    return rc;
}

/** Find "key" in a flat JSON object and return what follows the colon. */
static const char *
ai_json_value(const char *object, const char *key)
{
    te_string quoted = TE_STRING_INIT;
    const char *found;

    te_string_append(&quoted, "\"%s\"", key);
    found = strstr(object, quoted.ptr);
    te_string_free(&quoted);

    if (found == NULL)
        return NULL;
    found = strchr(found, ':');
    if (found == NULL)
        return NULL;
    found++;
    while (*found == ' ' || *found == '\t')
        found++;

    return found;
}

/* See description in tapi_ai_internal.h */
bool
tapi_ai_json_str(const char *object, const char *key, te_string *dest)
{
    const char *value = ai_json_value(object, key);

    if (value == NULL || *value != '"')
        return false;

    value++;
    for (; *value != '\0' && *value != '"'; value++)
    {
        if (*value != '\\')
        {
            te_string_append(dest, "%c", *value);
            continue;
        }
        value++;
        switch (*value)
        {
            case 'n':
                te_string_append(dest, "\n");
                break;
            case 't':
                te_string_append(dest, "\t");
                break;
            case 'r':
                break;
            case 'u':
            {
                unsigned int code = 0;
                int i;

                for (i = 1; i <= 4 && value[i] != '\0'; i++)
                {
                    char c = value[i];

                    code <<= 4;
                    if (c >= '0' && c <= '9')
                        code |= c - '0';
                    else if (c >= 'a' && c <= 'f')
                        code |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        code |= c - 'A' + 10;
                }
                value += i - 1;
                /* ASCII only; a wider code point becomes '?'. */
                te_string_append(dest, "%c", code < 0x80 ? (char)code : '?');
                break;
            }
            case '\0':
                return true;
            default:
                te_string_append(dest, "%c", *value);
                break;
        }
    }

    return true;
}

/* See description in tapi_ai_internal.h */
bool
tapi_ai_json_int(const char *object, const char *key, long *value)
{
    const char *found = ai_json_value(object, key);
    char *end;
    long v;

    if (found == NULL || strncmp(found, "null", 4) == 0)
        return false;

    /*
     * The value is followed by JSON punctuation - a comma or a brace -
     * so a whole-string parser (te_strtol_silent) rejects it. strtol
     * stops at the first non-digit, which is exactly what is wanted.
     */
    v = strtol(found, &end, 10);
    if (end == found)
        return false;

    *value = v;

    return true;
}

/* See description in tapi_ai_internal.h */
bool
tapi_ai_parse_usage(const char *text, tapi_ai_usage *usage)
{
    const char *line = text;
    const char *best = NULL;

    memset(usage, 0, sizeof(*usage));
    usage->input_tokens = -1;
    usage->output_tokens = -1;
    usage->total_tokens = -1;

    /* The last "final" line carries the usage; scan for the last one. */
    while ((line = strstr(line, "\"t\":\"final\"")) != NULL)
    {
        best = line;
        line += 1;
    }
    if (best == NULL)
        return false;

    /* Narrow to the usage object so a stray sibling key is not read. */
    {
        const char *u = strstr(best, "\"usage\"");
        long v;

        if (u == NULL)
            return false;
        if (tapi_ai_json_int(u, "input", &v))
            usage->input_tokens = (int)v;
        if (tapi_ai_json_int(u, "output", &v))
            usage->output_tokens = (int)v;
        if (tapi_ai_json_int(u, "total", &v))
            usage->total_tokens = (int)v;
    }

    return true;
}
