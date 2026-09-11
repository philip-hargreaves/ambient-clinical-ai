#pragma once

#include <filesystem>
#include <functional>
#include <variant>

#include "adapters/audio/capture_devices.hpp"
#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/guidance/guidance_lane.hpp"
#include "adapters/ipc/messages.hpp"
#include "adapters/ipc/pipe_server.hpp"
#include "adapters/models/model_store.hpp"
#include "core/session_controller.hpp"
#include "ports/note_lane.hpp"

namespace ambient::models {
class OvRuntime;
}  // namespace ambient::models

namespace ambient::translate {
class ITranslator;
class TranslateLane;
}  // namespace ambient::translate

namespace ambient::ipc {

std::variant<json, Error> HandleHello(const json& params);

std::variant<json, Error> HandleEcho(const json& params);

// Every staged model; `active` marks the one each role loads, the note
// role by its configured tier
json HandleModels(const ambient::models::ModelStore& models,
                  const std::string& note_tier = "default");

json NoteModelJson(const ambient::note::NoteModelState& state);

// note/tier: the shell names a tier, the lane resolves and loads it.
// Refused during a consultation; an unknown or unstaged tier is a
// parameter error naming what is staged
std::variant<json, Error> HandleNoteTier(ambient::note::INoteLane* lane, bool session_active,
                                         const json& params);

json HandleAudioInputs(const std::vector<ambient::audio::CaptureDevice>& devices);

// The clinician's voiceprint: where it came from and how many consultations
// refined it. Clearing is refused while a session runs
json HandleAnchorStatus(const ambient::diar::AnchorStore& anchors);
std::variant<json, Error> HandleAnchorClear(ambient::diar::AnchorStore& anchors,
                                            bool session_active);

json HandleSessionList(ambient::store::ISessionStore& sessions);

std::variant<json, Error> HandleSessionNote(ambient::store::ISessionStore& sessions,
                                            const json& params);

std::variant<json, Error> HandleSessionPatient(ambient::store::ISessionStore& sessions,
                                               const json& params);

std::variant<json, Error> HandleSessionTranscript(ambient::store::ISessionStore& sessions,
                                                  const json& params);

// Appraisal reflections on a stored session
std::variant<json, Error> HandleReflectionGet(ambient::store::ISessionStore& sessions,
                                              const json& params);
std::variant<json, Error> HandleReflectionUpdate(ambient::store::ISessionStore& sessions,
                                                 const json& params);
std::variant<json, Error> HandleReflectionDelete(ambient::store::ISessionStore& sessions,
                                                 const json& params);
json HandleReflectionList(ambient::store::ISessionStore& sessions);
std::variant<json, Error> HandleSessionDelete(ambient::store::ISessionStore& sessions,
                                              const json& params);

// Seed data from demo_dir: a no-op while present; cleared without touching real sessions
std::variant<json, Error> HandleDemoSeed(ambient::store::ISessionStore& sessions,
                                         const std::filesystem::path& demo_dir);
json HandleDemoClear(ambient::store::ISessionStore& sessions);

// Guidance: the panel shows the top three
inline constexpr int kGuidanceLimit = 3;
using Notify = std::function<void(const std::string& method, json params)>;
json GuidanceResultsJson(const std::string& session, const ambient::guidance::Results& results);
json GuidanceCorporaJson(const std::vector<ambient::guidance::Corpus>& corpora);
// The lane request behind every search: results go out as guidance/ready, a
// failure as guidance/failed, both naming the session (null for free text)
ambient::guidance::SearchRequest GuidanceSearchRequest(const std::string& session, std::string note,
                                                       int limit, Notify notify);
// guidance/search: the stored note of session id, or free text, through the
// lane. The reply is immediate; the results arrive as a notification
std::variant<json, Error> HandleGuidanceSearch(ambient::store::ISessionStore& sessions,
                                               ambient::guidance::GuidanceLane& lane,
                                               const json& params, const Notify& notify);
void RegisterGuidanceMethods(PipeServer& server, ambient::store::ISessionStore& sessions,
                             ambient::guidance::IGuidanceRetriever& retriever,
                             ambient::guidance::GuidanceLane& lane);

// Every method the engine serves. first_use: model caches were cold at
// launch, so the one-off compiles are running and readiness reports them.
void RegisterMethods(PipeServer& server, ambient::audio::SessionController& controller,
                     const ambient::models::ModelStore& models,
                     ambient::store::ISessionStore& sessions,
                     ambient::metrics::Registry* metrics = nullptr,
                     ambient::models::OvRuntime* runtime = nullptr,
                     ambient::translate::ITranslator* translator = nullptr,
                     ambient::translate::TranslateLane* translate_lane = nullptr,
                     bool first_use = false, ambient::diar::AnchorStore* anchors = nullptr,
                     ambient::note::INoteLane* note_lane = nullptr, bool stray_note_host = false,
                     const std::filesystem::path& demo_dir = {});

}  // namespace ambient::ipc
