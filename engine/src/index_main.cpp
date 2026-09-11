// Builds a guidance corpus from a build spec with the staged embedder, so index
// and query embeddings come from the same code.
//
//   ambient_index <spec.json> <out-dir> [--models <root>] [--verify]

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "adapters/guidance/corpus_store.hpp"
#include "adapters/guidance/embedder.hpp"
#include "adapters/guidance/indexer.hpp"
#include "adapters/models/model_store.hpp"

namespace {

constexpr const char* kBuilder = "ambient_index 1";

std::string NowUtc() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    gmtime_s(&utc, &now);
    char out[32];
    std::strftime(out, sizeof out, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return out;
}

// Beside the executable, as the engine finds its models; one level up for
// packaged debug layouts
std::filesystem::path DefaultModelsRoot() {
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    auto root = exe_dir / "models";
    if (!std::filesystem::exists(root)) root = exe_dir.parent_path() / "models";
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::filesystem::path models_root = DefaultModelsRoot();
    bool verify = false;
    std::vector<std::string> positional;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--models" && i + 1 < args.size()) {
            models_root = args[++i];
        } else if (args[i] == "--verify") {
            verify = true;
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) {
        std::fprintf(stderr,
                     "usage: ambient_index <spec.json> <out-dir> [--models <root>] [--verify]\n");
        return 2;
    }
    try {
        const auto spec = ambient::guidance::ReadBuildSpec(positional[0]);
        const std::filesystem::path out_dir = positional[1];
        const ambient::models::ModelStore store(models_root);
        std::printf("embedder: loading from %s\n", models_root.string().c_str());
        const auto embedder = ambient::guidance::Embedder::Load(store);
        const auto& identity = embedder->Identity();
        std::printf("embedder: %s %s, %d dimensions\n", identity.id.c_str(),
                    identity.rev.substr(0, 12).c_str(), identity.dim);

        const auto start = std::chrono::steady_clock::now();
        std::size_t last_reported = 0;
        const auto report = ambient::guidance::IndexCorpus(
            spec, *embedder, out_dir, NowUtc(), kBuilder, [&](std::size_t done, std::size_t total) {
                if (done - last_reported >= 500 || done == total) {
                    const auto seconds =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                            .count();
                    std::printf("  %zu/%zu chunks, %.0f s\n", done, total, seconds);
                    std::fflush(stdout);
                    last_reported = done;
                }
            });
        std::printf("built %s: %zu chunks, %zu over %d tokens, -> %s\n", spec.corpus.id.c_str(),
                    report.chunks, report.truncated, identity.max_tokens, out_dir.string().c_str());

        if (verify) {
            std::string reason;
            const auto opened = ambient::guidance::CorpusStore::Open(out_dir, identity, reason);
            if (!opened) {
                std::fprintf(stderr, "verify: refused: %s\n", reason.c_str());
                return 1;
            }
            std::printf("verify: %zu chunks loaded, every guard passed\n", opened->Size());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ambient_index: %s\n", e.what());
        return 1;
    }
}
