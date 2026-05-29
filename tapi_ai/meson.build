# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# External TE library (see TE_EXT_REPO): engine-side client for AI
# providers - OpenAI, Anthropic (Claude), xAI (Grok), Google (Gemini)
# and any OpenAI-compatible endpoint - reached from an agent with a
# token, with file attachments and streaming sessions. Built as a shared
# library on the engine platform only, like tapi_bt: it calls tapi and
# tapi_job, engine libraries with no static variant, so a static build
# of it would fail the configure.

if get_option('engine')
    build_lib_shared = true
endif
build_lib_static = false
install_lib = install_dev

headers += files(
    'tapi_ai.h',
    'tapi_ai_file.h',
    'tapi_ai_stream.h',
)

# tapi_ai_internal.h is internal to this library and is not installed.
# tapi_ai_helper.c is generated from tapi_ai_helper.py.in; the .py.in is
# the source of truth (see the file's header).
sources += files(
    'tapi_ai.c',
    'tapi_ai_cmd.c',
    'tapi_ai_file.c',
    'tapi_ai_helper.c',
    'tapi_ai_stream.c',
)

te_libs += [
    'tapi',
    'tapi_job',
    'tapi_devtool',
]
