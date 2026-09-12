// audiocpp_capi.h
// Minimal C API over the audio.cpp engine runtime, for in-process hosts
// (EiffelBS / OpenTimbre). Built as a SHARED library (audiocpp_engine.dll)
// so the vendored ggml stays symbol-isolated inside the DLL - hosts that
// link their own ggml (whisper.cpp, minimaxmusic.cpp ports) must not see
// these symbols.
//
// Copyright (C) 2026 EiffelBS. Licensed under AGPLv3 (matches audio.cpp).
//
// Contract notes:
//  - request_json uses the audiocpp_cli request-sequence JSON format
//    ({"text":..., "lyrics":..., "voice_ref":..., "speaker":...,
//     "duration_seconds":..., "seed":..., "instruct":...,
//     "reference_text":..., "options":{...}}) parsed by
//     minitts::cli::build_request_from_json - the same parser the
//     audiocpp_server uses for /v1/tasks/run.
//  - All exceptions are caught at the boundary; acpp_last_error returns
//    the message of the LAST failure on the given context (any thread).
//  - One run at a time per model handle (sessions are single-threaded).

#pragma once

#ifdef _WIN32
  #ifdef AUDIOCPP_ENGINE_EXPORTS
    #define ACPP_API __declspec(dllexport)
  #else
    #define ACPP_API __declspec(dllimport)
  #endif
#else
  #define ACPP_API __attribute__((visibility("default")))
#endif

extern "C" {

typedef struct acpp_ctx acpp_ctx;
typedef struct acpp_model acpp_model;

/** PCM16 WAV delivery for acpp_run_json. wav_bytes stays valid until the
    callback returns (copy out if needed). */
typedef void (*acpp_audio_cb)(const void * wav_bytes, int wav_size,
                              int sample_rate, int channels, void * userdata);

/** Per-phase progress for acpp_run_json_progress. phase is "ar", "flow"
    or "vocoder" (MiniMax Music 3); done/total count real completed units.
    Invoked from the run worker thread - keep it cheap and thread-safe. */
typedef void (*acpp_progress_cb)(const char * phase, int done, int total,
                                 void * userdata);

/** Per-stem delivery for acpp_run_json_stems. id is the stem name
    ("vocals", "drums", ...); wav_bytes is one PCM16 WAV image, valid
    until the callback returns (copy out if needed). */
typedef void (*acpp_stem_cb)(const char * id, const void * wav_bytes,
                             int wav_size, int sample_rate, int channels,
                             void * userdata);

/** backend: 0 = cpu, 1 = cuda, anything else = best available. */
ACPP_API acpp_ctx *   acpp_init (int backend, int device, int threads);
ACPP_API void         acpp_shutdown (acpp_ctx * ctx);

/** Load a model. family/task are the audio.cpp names (e.g. family
    "higgs_audio_tts", task "tts"; family "minimax_music3", task "gen").
    spec_override_path may be NULL (spec resolved from model_specs/ next
    to the binary or embedded in the GGUF). Returns NULL on error. */
ACPP_API acpp_model * acpp_model_load (acpp_ctx * ctx, const char * family,
                                       const char * model_path,
                                       const char * task,
                                       const char * spec_override_path);
ACPP_API void         acpp_model_unload (acpp_model * model);

/** Run one request. Returns 0 = audio delivered through the callback,
    1 = task completed without audio (e.g. text-only tasks), -1 = error. */
ACPP_API int          acpp_run_json (acpp_model * model,
                                     const char * request_json,
                                     acpp_audio_cb cb, void * userdata);

/** Run one request with per-phase progress (MiniMax Music 3 only: other
    families run WITHOUT progress rather than failing). Same return
    contract as acpp_run_json. pcb may be NULL (= plain acpp_run_json). */
ACPP_API int          acpp_run_json_progress (acpp_model * model,
                                              const char * request_json,
                                              acpp_audio_cb cb, void * userdata,
                                              acpp_progress_cb pcb,
                                              void * progress_userdata);

/** Run one request, delivering EACH named_audio_output through cb
    (stems separation: vocals/drums/...). Returns the stem count
    (>= 0), -1 on error. */
ACPP_API int          acpp_run_json_stems (acpp_model * model,
                                           const char * request_json,
                                           acpp_stem_cb cb, void * userdata);

/** Text of the last run's text_output (NULL when the run produced
    none). lang_out receives the language tag (may be NULL). Pointers
    stay valid until the next run on the same model (copy out if
    needed). */
ACPP_API const char * acpp_last_text (acpp_model * model,
                                      const char ** lang_out);

/** Payload of the last run's artifact_output (first of output_artifacts
    when artifact_output is empty). Returns the payload byte count
    (0 when the run produced no artifact). kind_out/id_out receive the
    kind name ("midi", ...) and the artifact id. Same lifetime rule as
    acpp_last_text. */
ACPP_API int          acpp_last_artifact (acpp_model * model,
                                          const void ** bytes_out,
                                          const char ** kind_out,
                                          const char ** id_out);

/** Message of the last failure on this context (NULL when none). */
ACPP_API const char * acpp_last_error (acpp_ctx * ctx);

} // extern "C"
