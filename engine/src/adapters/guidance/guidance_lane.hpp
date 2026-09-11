#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "ports/guidance_retriever.hpp"

namespace ambient::guidance {

struct SearchRequest {
    std::string note;
    int limit = 0;
    std::function<void(const Results&)> on_ready;
    std::function<void(const std::string& detail)> on_failed;
};

// One worker over the retriever: loads in the background, runs one search at a
// time and calls the request's callbacks on the worker. A request arriving
// while one runs replaces any still waiting, so the latest note is always the
// one searched and an outdated one never is
class GuidanceLane {
   public:
    explicit GuidanceLane(IGuidanceRetriever& retriever);
    ~GuidanceLane();

    void Prepare();
    void Run(SearchRequest request);

   private:
    void Start();  // under mutex_
    void Work();

    IGuidanceRetriever& retriever_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool prepare_ = false;
    bool stop_ = false;
    std::optional<SearchRequest> pending_;
};

}  // namespace ambient::guidance
