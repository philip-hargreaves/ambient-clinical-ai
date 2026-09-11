#include "adapters/guidance/guidance_lane.hpp"

#include <cstdio>
#include <exception>
#include <utility>

namespace ambient::guidance {

GuidanceLane::GuidanceLane(IGuidanceRetriever& retriever) : retriever_(retriever) {}

GuidanceLane::~GuidanceLane() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void GuidanceLane::Prepare() {
    std::lock_guard<std::mutex> lock(mutex_);
    prepare_ = true;
    Start();
    wake_.notify_all();
}

void GuidanceLane::Run(SearchRequest request) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(request);
    Start();
    wake_.notify_all();
}

void GuidanceLane::Start() {
    if (!worker_.joinable()) worker_ = std::thread([this] { Work(); });
}

void GuidanceLane::Work() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return stop_ || prepare_ || pending_.has_value(); });
        if (stop_) return;
        if (prepare_) {
            prepare_ = false;
            lock.unlock();
            try {
                retriever_.Prepare();
                int available = 0, unavailable = 0;
                for (const auto& corpus : retriever_.Corpora()) {
                    ++(corpus.unavailable.empty() ? available : unavailable);
                }
                std::fprintf(stderr, "ambient-engine: guidance ready, %d corpora, %d unavailable\n",
                             available, unavailable);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ambient-engine: guidance unavailable: %s\n", e.what());
            }
            lock.lock();
            continue;
        }
        SearchRequest request = std::move(*pending_);
        pending_.reset();
        lock.unlock();
        try {
            const Results results = retriever_.Search(request.note, request.limit);
            if (request.on_ready) request.on_ready(results);
        } catch (const std::exception& e) {
            if (request.on_failed) request.on_failed(e.what());
        } catch (...) {
            if (request.on_failed) request.on_failed("guidance search failed");
        }
        lock.lock();
    }
}

}  // namespace ambient::guidance
