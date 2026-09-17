// capi_smoke.cpp - standalone proof for audiocpp_engine.dll.
//
// usage: capi_smoke <family> <model_path> <task> <spec_override|-> <request_json> <out.wav> [backend]
//
// Renders one request through the C API and writes the PCM16 WAV to
// out.wav. Exit 0 = audio delivered. Pair it with the same request on
// the audiocpp server for a byte-level parity check (same seed).

#include "audiocpp_capi.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
struct Sink
{
    std::ofstream file;
    int sampleRate = 0;
    int channels = 0;
    long long bytes = 0;
};

void writeWav (const void * wavBytes, int wavSize, int sampleRate,
               int channels, void * userdata)
{
    auto * sink = (Sink *) userdata;
    sink->file.write ((const char *) wavBytes, wavSize);
    sink->sampleRate = sampleRate;
    sink->channels = channels;
    sink->bytes = wavSize;
}
}

int main (int argc, char ** argv)
{
    if (argc < 7)
    {
        std::printf ("usage: capi_smoke <family> <model_path> <task>"
                     " <spec_override|-> <request_json> <out.wav> [backend]\n");
        return 1;
    }
    const char * family = argv[1];
    const char * modelPath = argv[2];
    const char * task = argv[3];
    const char * specOverride = std::string (argv[4]) == "-" ? nullptr : argv[4];
    const char * outPath = argv[6];
    const int backend = argc > 7 ? std::atoi (argv[7]) : 0;

    // request_json may be @file or the ACPP_REQUEST_JSON env var: PowerShell
    // strips embedded quotes when passing args, so scripts hand the JSON
    // over through either channel instead.
    std::string requestJson (argv[5]);
    if (const char * envReq = std::getenv ("ACPP_REQUEST_JSON"))
        if (! std::string (envReq).empty())
            requestJson = envReq;
    if (! requestJson.empty() && requestJson[0] == '@')
    {
        std::ifstream in (requestJson.substr (1), std::ios::binary);
        requestJson.assign (std::istreambuf_iterator<char> (in),
                            std::istreambuf_iterator<char>());
    }
    std::printf ("request: %.60s\n", requestJson.c_str());
    std::fflush (stdout);
    std::fprintf (stderr, "smoke: init\n");
    std::fflush (stderr);

    acpp_ctx * ctx = acpp_init (backend, 0, 4);
    std::fprintf (stderr, "smoke: init done\n");
    std::fflush (stderr);
    if (ctx == nullptr)
    {
        std::printf ("FAIL: acpp_init: %s\n",
                     acpp_last_error (nullptr) ? "" : "(null ctx)");
        return 1;
    }

    std::fprintf (stderr, "smoke: model load\n");
    std::fflush (stderr);
    acpp_model * model = acpp_model_load (ctx, family, modelPath, task,
                                          specOverride);
    std::fprintf (stderr, "smoke: model load done\n");
    std::fflush (stderr);
    if (model == nullptr)
    {
        std::printf ("FAIL: acpp_model_load: %s\n", acpp_last_error (ctx));
        acpp_shutdown (ctx);
        return 1;
    }

    Sink sink;
    sink.file.open (outPath, std::ios::binary);
    if (! sink.file.is_open())
    {
        std::printf ("FAIL: cannot open %s\n", outPath);
        acpp_model_unload (model);
        acpp_shutdown (ctx);
        return 1;
    }

    std::fprintf (stderr, "smoke: run\n");
    std::fflush (stderr);
    const int rc = acpp_run_json (model, requestJson.c_str(), writeWav, &sink);
    std::fprintf (stderr, "smoke: run done rc=%d\n", rc);
    std::fflush (stderr);
    // Text/artifact outputs accompany rc=0 (audio + text) and rc=1 (text or
    // artifact only, e.g. midi tasks deliver no wav). Print both on any
    // non-error return so the smoke can gate artifact-producing tasks.
    if (rc >= 0)
    {
        const char * textLang = nullptr;
        if (const char * text = acpp_last_text (model, &textLang))
            std::printf ("text_output: lang=%s bytes=%zu head=%.80s\n",
                         textLang != nullptr ? textLang : "(null)",
                         std::string (text).size(), text);
        const void * bytes = nullptr;
        const char * kind = nullptr;
        const char * id = nullptr;
        const int n = acpp_last_artifact (model, &bytes, &kind, &id);
        if (n > 0 && bytes != nullptr)
        {
            std::printf ("artifact: kind=%s id=%s bytes=%d head=%.4s\n",
                         kind != nullptr ? kind : "(null)",
                         id != nullptr ? id : "(null)",
                         n, (const char *) bytes);
            std::ofstream art (std::string (outPath) + ".artifact.bin",
                               std::ios::binary);
            art.write ((const char *) bytes, n);
        }
    }
    if (rc != 0)
    {
        std::printf ("FAIL: acpp_run_json rc=%d: %s\n", rc,
                     acpp_last_error (ctx));
        acpp_model_unload (model);
        acpp_shutdown (ctx);
        return 1;
    }
    sink.file.close();

    // Surface the model's text_output (YuE2 planned ABC score recovery):
    // a gen run with cot != off must expose the planned score here.
    const char * textLang = nullptr;
    if (const char * text = acpp_last_text (model, &textLang))
    {
        std::printf ("text_output: lang=%s bytes=%zu head=%.80s\n",
                     textLang != nullptr ? textLang : "(null)",
                     std::string (text).size(), text);
    }

    std::printf ("PASS: %lld wav bytes, %d Hz, %d ch -> %s\n",
                 sink.bytes, sink.sampleRate, sink.channels, outPath);
    acpp_model_unload (model);
    acpp_shutdown (ctx);
    return 0;
}
