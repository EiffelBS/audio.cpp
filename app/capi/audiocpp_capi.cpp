// audiocpp_capi.cpp
// Implementation of the minimal C API over engine_runtime. Everything
// C++ (exceptions, STL, ggml) stays inside the DLL; only this surface is
// exported (see audiocpp_engine.def).
//
// Copyright (C) 2026 EiffelBS. Licensed under AGPLv3.

#include "audiocpp_capi.h"

#include "cli/request.h"

#include "engine/framework/core/backend.h"
#include "engine/framework/io/json.h"
#include "engine/framework/runtime/registry.h"
#include "engine/framework/runtime/session.h"
#include "engine/community_models/minimax_music3/session.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Completes the opaque typedefs from audiocpp_capi.h.
struct acpp_ctx
{
    std::unique_ptr<engine::runtime::ModelRegistry> registry;
    engine::core::BackendConfig backend;
    std::string lastError;
    std::mutex errorMutex;
};

struct acpp_model
{
    acpp_ctx * ctx = nullptr;
    std::unique_ptr<engine::runtime::ILoadedVoiceModel> loaded;
    std::unique_ptr<engine::runtime::IVoiceTaskSession> session;
    engine::runtime::IOfflineVoiceTaskSession * offline = nullptr;
    // Last completed run: backs acpp_last_text / acpp_last_artifact /
    // acpp_run_json_stems (the getters hand out pointers into it, valid
    // until the next run on this model).
    engine::runtime::TaskResult lastResult;
    bool hasLastResult = false;
};

namespace
{

void setLastError (acpp_ctx * ctx, const std::string & message)
{
    if (ctx == nullptr)
        return;
    std::lock_guard<std::mutex> lock (ctx->errorMutex);
    ctx->lastError = message;
}

std::string currentExceptionMessage ()
{
    try
    {
        throw;
    }
    catch (const std::exception & e)
    {
        return e.what();
    }
    catch (...)
    {
        return "unknown error";
    }
}

/** Minimal PCM16 WAV encoder (mono/stereo interleaved, 44-byte header). */
std::vector<uint8_t> encodePcm16Wav (const engine::runtime::AudioBuffer & audio)
{
    const int channels = audio.channels > 0 ? audio.channels : 1;
    const size_t frames = audio.samples.size() / (size_t) channels;
    const uint32_t dataBytes = (uint32_t) (frames * (size_t) channels * 2u);
    const uint32_t byteRate = (uint32_t) audio.sample_rate * (uint32_t) channels * 2u;

    std::vector<uint8_t> out (44 + dataBytes);
    size_t off = 0;
    auto put4 = [&] (const char * tag)
    { std::memcpy (out.data() + off, tag, 4); off += 4; };
    auto put32 = [&] (uint32_t v)
    {
        out[off] = (uint8_t) v; out[off+1] = (uint8_t) (v >> 8);
        out[off+2] = (uint8_t) (v >> 16); out[off+3] = (uint8_t) (v >> 24);
        off += 4;
    };
    auto put16 = [&] (uint16_t v)
    {
        out[off] = (uint8_t) v; out[off+1] = (uint8_t) (v >> 8);
        off += 2;
    };

    put4 ("RIFF");
    put32 (36 + dataBytes);
    put4 ("WAVE");
    put4 ("fmt ");
    put32 (16);
    put16 (1);                            // PCM
    put16 ((uint16_t) channels);
    put32 ((uint32_t) audio.sample_rate);
    put32 (byteRate);
    put16 ((uint16_t) (channels * 2));
    put16 (16);                           // bits
    put4 ("data");
    put32 (dataBytes);

    for (size_t i = 0; i < frames * (size_t) channels; ++i)
    {
        float s = audio.samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        const auto v = (int16_t) std::lroundf (s * 32767.0f);
        out[off++] = (uint8_t) v;
        out[off++] = (uint8_t) ((uint16_t) v >> 8);
    }
    return out;
}

} // namespace

extern "C" {

acpp_ctx * acpp_init (int backend, int device, int threads)
{
    auto * ctx = new acpp_ctx();
    ctx->backend.type = backend == 0 ? engine::core::BackendType::Cpu
                      : backend == 1 ? engine::core::BackendType::Cuda
                                     : engine::core::BackendType::BestAvailable;
    ctx->backend.device = device;
    ctx->backend.threads = threads > 0 ? threads : 1;
    try
    {
        ctx->registry = std::make_unique<engine::runtime::ModelRegistry> (
            engine::runtime::make_default_registry());
    }
    catch (...)
    {
        setLastError (ctx, currentExceptionMessage());
        delete ctx;
        return nullptr;
    }
    return ctx;
}

void acpp_shutdown (acpp_ctx * ctx)
{
    delete ctx;   // registry destruction releases loaded models
}

acpp_model * acpp_model_load (acpp_ctx * ctx, const char * family,
                              const char * model_path, const char * task,
                              const char * spec_override_path)
{
    if (ctx == nullptr || family == nullptr || model_path == nullptr
        || task == nullptr)
        return nullptr;
    try
    {
        engine::runtime::ModelLoadRequest request;
        request.model_path = std::filesystem::path (model_path);
        request.family_hint = std::string (family);
        if (spec_override_path != nullptr && spec_override_path[0] != '\0')
            request.model_spec_override = std::filesystem::path (spec_override_path);

        auto loaded = ctx->registry->load (request);
        const engine::runtime::TaskSpec spec {
            engine::runtime::parse_voice_task_kind (std::string (task)),
            engine::runtime::RunMode::Offline,
        };
        engine::runtime::SessionOptions options;
        options.backend = ctx->backend;
        auto session = loaded->create_task_session (spec, options);
        auto * offline = dynamic_cast<engine::runtime::IOfflineVoiceTaskSession *>(
            session.get());
        if (offline == nullptr)
            throw std::runtime_error ("model does not provide offline execution");

        auto * model = new acpp_model();
        model->ctx = ctx;
        model->loaded = std::move (loaded);
        model->session = std::move (session);
        model->offline = offline;
        return model;
    }
    catch (...)
    {
        setLastError (ctx, currentExceptionMessage());
        return nullptr;
    }
}

void acpp_model_unload (acpp_model * model)
{
    delete model;   // session + loaded model destruction releases VRAM
}

int acpp_run_json (acpp_model * model, const char * request_json,
                   acpp_audio_cb cb, void * userdata)
{
    if (model == nullptr || request_json == nullptr || cb == nullptr)
        return -1;
    try
    {
        const auto value = engine::io::json::parse (request_json);
        auto request = minitts::cli::build_request_from_json (
            value, std::filesystem::path{});
        model->session->prepare (
            engine::runtime::build_preparation_request (request));
        auto result = model->offline->run (request);
        model->lastResult = result;
        model->hasLastResult = true;
        if (! result.audio_output.has_value()
            || result.audio_output->samples.empty())
            return 1;
        const auto wav = encodePcm16Wav (*result.audio_output);
        cb (wav.data(), (int) wav.size(),
            result.audio_output->sample_rate, result.audio_output->channels,
            userdata);
        return 0;
    }
    catch (...)
    {
        setLastError (model->ctx, currentExceptionMessage());
        return -1;
    }
}

namespace
{

const char * artifactKindName (engine::runtime::ArtifactKind kind)
{
    using engine::runtime::ArtifactKind;
    switch (kind)
    {
    case ArtifactKind::SpeakerEmbedding:    return "speaker_embedding";
    case ArtifactKind::StyleEmbedding:      return "style_embedding";
    case ArtifactKind::PromptEmbedding:     return "prompt_embedding";
    case ArtifactKind::AcousticTokens:      return "acoustic_tokens";
    case ArtifactKind::Midi:                return "midi";
    case ArtifactKind::TranscriptAlignment: return "transcript_alignment";
    case ArtifactKind::DiarizationState:    return "diarization_state";
    case ArtifactKind::VadState:            return "vad_state";
    case ArtifactKind::Custom:              return "custom";
    }
    return "custom";
}

} // namespace

const char * acpp_last_text (acpp_model * model, const char ** lang_out)
{
    if (lang_out != nullptr)
        *lang_out = nullptr;
    if (model == nullptr || ! model->hasLastResult)
        return nullptr;
    const auto & text = model->lastResult.text_output;
    if (! text.has_value() || text->text.empty())
        return nullptr;
    if (lang_out != nullptr)
        *lang_out = text->language.c_str();
    return text->text.c_str();
}

int acpp_last_artifact (acpp_model * model, const void ** bytes_out,
                        const char ** kind_out, const char ** id_out)
{
    if (bytes_out != nullptr)
        *bytes_out = nullptr;
    if (kind_out != nullptr)
        *kind_out = nullptr;
    if (id_out != nullptr)
        *id_out = nullptr;
    if (model == nullptr || ! model->hasLastResult)
        return 0;
    const engine::runtime::VoiceArtifact * artifact = nullptr;
    if (model->lastResult.artifact_output.has_value())
        artifact = &(*model->lastResult.artifact_output);
    else if (! model->lastResult.output_artifacts.empty())
        artifact = &model->lastResult.output_artifacts.front();
    if (artifact == nullptr || artifact->payload.empty())
        return 0;
    if (bytes_out != nullptr)
        *bytes_out = (const void *) artifact->payload.data();
    if (kind_out != nullptr)
        *kind_out = artifactKindName (artifact->kind);
    if (id_out != nullptr)
        *id_out = artifact->id.c_str();
    return (int) artifact->payload.size();
}

int acpp_run_json_stems (acpp_model * model, const char * request_json,
                         acpp_stem_cb cb, void * userdata)
{
    if (model == nullptr || request_json == nullptr || cb == nullptr)
        return -1;
    try
    {
        const auto value = engine::io::json::parse (request_json);
        auto request = minitts::cli::build_request_from_json (
            value, std::filesystem::path{});
        model->session->prepare (
            engine::runtime::build_preparation_request (request));
        auto result = model->offline->run (request);
        model->lastResult = result;
        model->hasLastResult = true;
        int count = 0;
        for (const auto & named : result.named_audio_outputs)
        {
            if (named.audio.samples.empty())
                continue;
            const auto wav = encodePcm16Wav (named.audio);
            cb (named.id.c_str(), wav.data(), (int) wav.size(),
                named.audio.sample_rate, named.audio.channels, userdata);
            ++count;
        }
        return count;
    }
    catch (...)
    {
        setLastError (model->ctx, currentExceptionMessage());
        return -1;
    }
}

const char * acpp_last_error (acpp_ctx * ctx)
{
    if (ctx == nullptr)
        return nullptr;
    std::lock_guard<std::mutex> lock (ctx->errorMutex);
    return ctx->lastError.c_str();
}

int acpp_run_json_progress (acpp_model * model, const char * request_json,
                            acpp_audio_cb cb, void * userdata,
                            acpp_progress_cb pcb, void * progress_userdata)
{
    if (model == nullptr || request_json == nullptr || cb == nullptr)
        return -1;
    // Progress is a MiniMax Music 3 capability on the mainline fork. The
    // dev-branch runtime exposes no session progress sink yet, so this
    // function degrades to the plain path (no progress events) rather
    // than failing the render - OpenTimbre falls back to indeterminate
    // progress. Same return contract either way.
    (void) pcb;
    (void) progress_userdata;
    return acpp_run_json (model, request_json, cb, userdata);
}

} // extern "C"
