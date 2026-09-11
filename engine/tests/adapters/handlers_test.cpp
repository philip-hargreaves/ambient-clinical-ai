#include "adapters/ipc/handlers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "adapters/diarisation/anchor_store.hpp"
#include "adapters/storage/sqlite_session_store.hpp"
#include "core/version.hpp"

namespace ambient::ipc {
namespace {

json HelloParams() {
    return json{
        {"name", "ambient-shell"}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion}};
}

const json& ResultOf(const std::variant<json, Error>& outcome) {
    return std::get<json>(outcome);
}

TEST(Handlers, HelloAnswersWithTheEngineIdentity) {
    const auto outcome = HandleHello(HelloParams());

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const auto& result = ResultOf(outcome);
    EXPECT_EQ(result["name"], ambient::kName);
    EXPECT_EQ(result["version"], ambient::kVersion);
    EXPECT_EQ(result["protocolVersion"], kProtocolVersion);
}

TEST(Handlers, HelloRejectsAPeerItCannotParse) {
    // Each of these fails PeerInfoFromJson for a different reason
    const json cases[] = {
        json::object(),
        json{{"name", "shell"}},
        json{{"name", 1}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion}},
        json{{"name", "shell"}, {"version", "0.1.0"}, {"protocolVersion", kProtocolVersion + 1}},
        json{{"name", "shell"},
             {"version", "0.1.0"},
             {"protocolVersion", kProtocolVersion},
             {"extra", 1}},
    };

    for (const auto& params : cases) {
        const auto outcome = HandleHello(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }
}

TEST(Handlers, HelloIgnoresWhatThePeerClaimsAboutItself) {
    // The reply describes the engine, never the caller
    const auto outcome = HandleHello(
        json{{"name", "impostor"}, {"version", "9.9.9"}, {"protocolVersion", kProtocolVersion}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["name"], ambient::kName);
    EXPECT_EQ(ResultOf(outcome)["version"], ambient::kVersion);
}

TEST(Handlers, EchoReturnsThePayload) {
    const auto outcome = HandleEcho(json{{"payload", "alive"}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["payload"], "alive");
}

TEST(Handlers, EchoPreservesClinicalNonAscii) {
    const std::string clinical = "naïve café-au-lait 東京 µg °C";

    const auto outcome = HandleEcho(json{{"payload", clinical}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome)["payload"], clinical);
}

json LoadFixture(const std::string& name) {
    std::ifstream in(std::string(AMBIENT_FIXTURE_DIR) + "/" + name);
    if (!in.is_open()) throw std::runtime_error("missing fixture: " + name);
    return json::parse(in);
}

TEST(Handlers, ModelsListMatchesTheFixture) {
    const auto root = std::filesystem::temp_directory_path() / "ambient-handlers-models";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "whisper-turbo-int8");
    std::ofstream(root / "whisper-turbo-int8" / "manifest.json")
        << R"({"manifestVersion": 1, "id": "whisper-turbo-int8", "name": "Whisper Large v3 Turbo",)"
        << R"( "task": "asr", "tier": "default", "licence": "MIT", "runtime": {"device": "GPU"},)"
        << R"( "files": {"model.xml": "00"}})";

    const ambient::models::ModelStore store(root);
    const json built = MakeResult(std::int64_t{7}, HandleModels(store));
    EXPECT_EQ(built, LoadFixture("models-list.json"));
    std::filesystem::remove_all(root);
}

// Two note models staged: the configured tier is the active one, whatever
// order the store lists them in
TEST(Handlers, ModelsListMarksTheConfiguredNoteTierActive) {
    const auto root = std::filesystem::temp_directory_path() / "ambient-handlers-tiers";
    std::filesystem::remove_all(root);
    for (const auto& [id, tier] :
         {std::pair{"qwen3.5-9b-int4", "default"}, std::pair{"qwen3.6-35b-a3b-int4", "accuracy"}}) {
        std::filesystem::create_directories(root / id);
        std::ofstream(root / id / "manifest.json")
            << R"({"manifestVersion": 1, "id": ")" << id << R"(", "task": "note", "tier": ")"
            << tier << R"(", "licence": "Apache-2.0", "runtime": {"device": "GPU"},)"
            << R"( "files": {"model.xml": "00"}})";
    }

    const ambient::models::ModelStore store(root);
    const json listed = HandleModels(store, "accuracy");
    for (const auto& model : listed["models"]) {
        EXPECT_EQ(model["active"], model["tier"] == "accuracy") << model.dump();
    }
    std::filesystem::remove_all(root);
}

// A lane that records what it was asked and answers with a state
struct FakeLane : ambient::note::INoteLane {
    std::vector<std::string> configured;
    ambient::note::NoteModelState state;
    std::string refuse;  // Configure throws this as invalid_argument when set

    ambient::note::NoteModelState Configure(const std::string& tier) override {
        if (!refuse.empty()) throw std::invalid_argument(refuse);
        configured.push_back(tier);
        state.tier = tier;
        state.phase = ambient::note::NoteModelState::Phase::kLoading;
        return state;
    }

    ambient::note::NoteModelState State() const override {
        return state;
    }

    void SetListener(Listener) override {}
};

TEST(Handlers, NoteTierConfiguresTheLaneAndAnswersWithItsState) {
    FakeLane lane;
    lane.state.id = "qwen3.6-35b-a3b-int4";
    lane.state.name = "Qwen3.6 35B";

    const auto outcome = HandleNoteTier(&lane, false, json{{"tier", "accuracy"}});

    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const json& result = ResultOf(outcome);
    EXPECT_EQ(lane.configured, std::vector<std::string>{"accuracy"});
    EXPECT_EQ(result["tier"], "accuracy");
    EXPECT_EQ(result["state"], "loading");
    EXPECT_EQ(result["name"], "Qwen3.6 35B");
    EXPECT_EQ(result, LoadFixture("note-tier.json")["response"]["result"]);
}

TEST(Handlers, NoteTierRefusesWhatTheLaneCannotServe) {
    FakeLane lane;

    // Not a tier at all: never reaches the lane
    auto outcome = HandleNoteTier(&lane, false, json{{"tier", "premium"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams);
    EXPECT_TRUE(lane.configured.empty());

    // A tier nothing is staged for: the store's message, as a parameter error
    lane.refuse = "no model for note/accuracy; installed: qwen3.5-9b-int4(note/default)";
    outcome = HandleNoteTier(&lane, false, json{{"tier", "accuracy"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams);
    EXPECT_NE(std::get<Error>(outcome).data->dump().find("qwen3.5-9b-int4"), std::string::npos);

    // Mid-consultation: a switch would end the resident model under a session
    lane.refuse.clear();
    outcome = HandleNoteTier(&lane, true, json{{"tier", "accuracy"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);
    EXPECT_TRUE(lane.configured.empty());

    // No note lane at all (nothing staged, or no host beside the engine)
    outcome = HandleNoteTier(nullptr, false, json{{"tier", "default"}});
    ASSERT_TRUE(std::holds_alternative<Error>(outcome));
    EXPECT_EQ(std::get<Error>(outcome).code, kSessionError);
}

TEST(Handlers, NoteModelNotificationMatchesTheFixture) {
    ambient::note::NoteModelState state;
    state.phase = ambient::note::NoteModelState::Phase::kReady;
    state.tier = "accuracy";
    state.id = "qwen3.6-35b-a3b-int4";
    state.name = "Qwen3.6 35B";
    state.seconds = 27.4;

    const json fixture = LoadFixture("note-model.json");
    EXPECT_EQ(NoteModelJson(state), fixture["params"]);
    EXPECT_EQ(fixture["method"], "note/model");
}

struct SessionStoreFixture {
    std::filesystem::path root;
    std::unique_ptr<ambient::store::SqliteSessionStore> store;

    SessionStoreFixture() {
        root = std::filesystem::temp_directory_path() /
               ("ambient-handlers-sessions-" +
                std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        store = std::make_unique<ambient::store::SqliteSessionStore>(root, std::chrono::hours(1));
    }

    ~SessionStoreFixture() {
        store.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TEST(Handlers, SessionListAndTranscriptRoundTrip) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->AppendTurn(id, {480000, 48000, "", "about three weeks now, mostly mornings"});
    fixture.store->Finalise(id);

    const json list = HandleSessionList(*fixture.store);
    ASSERT_EQ(list["sessions"].size(), 1u);
    EXPECT_EQ(list["sessions"][0]["id"], id);
    EXPECT_EQ(list["sessions"][0]["state"], "finalised");
    EXPECT_EQ(list["sessions"][0]["sampleRate"], 16000);
    EXPECT_FALSE(list["sessions"][0]["endedAt"].get<std::string>().empty());
    EXPECT_EQ(list["sessions"][0]["label"], "") << "no note yet";
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null());
    EXPECT_EQ(list["sessions"][0]["audioSeconds"], 33.0)
        << "the audio's length from the turns, which outlive the audio";
    // The fixture is the shape both languages agree on. Named first: iterating
    // a temporary's sub-object dangles (range-for extends only the top level)
    const json list_fixture = LoadFixture("session-list.json");
    for (const auto& [key, value] : list_fixture["result"]["sessions"][0].items()) {
        EXPECT_TRUE(list["sessions"][0].contains(key)) << key;
    }

    const auto outcome = HandleSessionTranscript(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    // The result payload matches the fixture's shape byte for byte
    const json built = MakeResult(std::int64_t{8}, std::get<json>(outcome));
    EXPECT_EQ(built, LoadFixture("session-transcript.json"));
}

TEST(Handlers, SessionNoteReturnsTheStoredText) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, ambient::store::DocumentKind::kNote,
                                {.text = "The patient presents with a swollen left elbow.",
                                 .style = "soap",
                                 .detail = "concise"});

    const auto outcome = HandleSessionNote(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    const json note = std::get<json>(outcome);
    EXPECT_EQ(note["text"], "The patient presents with a swollen left elbow.");
    EXPECT_EQ(note["style"], "soap");
    EXPECT_EQ(note["detail"], "concise");
    EXPECT_TRUE(note["generatedAt"].is_string());
    EXPECT_TRUE(note["editedAt"].is_null());
    const json note_fixture = LoadFixture("session-note.json");
    for (const auto& [key, value] : note_fixture["result"].items()) {
        EXPECT_TRUE(note.contains(key)) << key;
    }

    fixture.store->EditDocument(id, ambient::store::DocumentKind::kNote, "edited");
    const json edited = std::get<json>(HandleSessionNote(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(edited["text"], "edited");
    EXPECT_TRUE(edited["editedAt"].is_string());

    const auto missing = HandleSessionNote(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
}

// Seeds once, lists as samples, clears without touching the real session
TEST(Handlers, TheSampleYearSeedsOnceAndClearsCleanly) {
    using ambient::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto real = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(real);

    const auto seeded = HandleDemoSeed(*fixture.store, AMBIENT_DEMO_DIR);
    ASSERT_TRUE(std::holds_alternative<json>(seeded))
        << (std::holds_alternative<Error>(seeded) ? std::get<Error>(seeded).message : "");
    EXPECT_EQ(std::get<json>(seeded)["added"], 8);
    const auto again = HandleDemoSeed(*fixture.store, AMBIENT_DEMO_DIR);
    ASSERT_TRUE(std::holds_alternative<json>(again));
    EXPECT_EQ(std::get<json>(again)["added"], 0) << "already seeded is a no-op, not an error";

    const json sessions = HandleSessionList(*fixture.store)["sessions"];
    EXPECT_EQ(sessions.size(), 9u);
    std::size_t samples = 0;
    for (const auto& s : sessions) {
        if (s["demo"].get<bool>()) {
            samples += 1;
            EXPECT_FALSE(s["label"].get<std::string>().empty());
            EXPECT_GT(s["audioSeconds"].get<double>(), 300.0);
        } else {
            EXPECT_EQ(s["id"], real);
        }
    }
    EXPECT_EQ(samples, 8u);

    const json entries = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(entries.size(), 8u);
    for (const auto& e : entries) {
        EXPECT_TRUE(e["demo"].get<bool>());
        EXPECT_FALSE(e["learned"].get<std::string>().empty());
        EXPECT_FALSE(e["summary"].get<std::string>().empty());
    }
    const auto id = entries[0]["id"].get<std::string>();
    const json got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_FALSE(got["reflection"]["happened"].get<std::string>().empty());
    EXPECT_FALSE(fixture.store->ReadDocument(id, DocumentKind::kNote).text.empty());
    EXPECT_FALSE(fixture.store->ReadDocument(id, DocumentKind::kPatient).text.empty());
    EXPECT_EQ(fixture.store->ReadDocument(id, DocumentKind::kNote).style, "prose");

    EXPECT_EQ(HandleDemoClear(*fixture.store)["removed"], 8);
    const json left = HandleSessionList(*fixture.store)["sessions"];
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0]["id"], real);
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);

    // Everything, samples and real alike, in one erase
    ASSERT_TRUE(std::holds_alternative<json>(HandleDemoSeed(*fixture.store, AMBIENT_DEMO_DIR)));
    EXPECT_EQ(fixture.store->DeleteAll(), 9u);
    EXPECT_EQ(HandleSessionList(*fixture.store)["sessions"].size(), 0u);
}

// Stored summaries leave scrubbed whoever wrote them
TEST(Handlers, AStoredSummaryLeavesScrubbedWhateverWasStored) {
    using ambient::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A 53-year-old male presented with a swollen elbow."});

    const json got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A male in their fifties presented with a swollen elbow.");
    const json listed = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["summary"], "A male in their fifties presented with a swollen elbow.");

    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"summary", "The patient, aged 67, was seen."}})));
    EXPECT_EQ(fixture.store->ReadDocument(id, DocumentKind::kSummary).text,
              "The patient, in their sixties, was seen.");
}

TEST(Handlers, ReflectionGetUpdateListAndDelete) {
    using ambient::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    // Nothing yet: label only, both parts null, not listed
    json got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["label"], "Elbow swelling");
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);

    // A summary alone is an entry: the sheet was opened, the writing can follow
    fixture.store->SaveDocument(id, DocumentKind::kSummary,
                                {.text = "A patient in their forties."});
    json listed = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0]["learned"], "");
    EXPECT_EQ(listed[0]["summary"], "A patient in their forties.");
    EXPECT_TRUE(listed[0]["createdAt"].is_string());

    // Three empty answers keep the entry; only delete removes it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"happened", ""}, {"learned", ""}, {"next", ""}})));
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 1u);

    // A first answer creates the entry; a later one merges into it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"learned", "check the temperature"}})));
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"next", "add a red-flag check"}})));
    got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["reflection"]["happened"], "");
    EXPECT_EQ(got["reflection"]["learned"], "check the temperature");
    EXPECT_EQ(got["reflection"]["next"], "add a red-flag check");
    EXPECT_TRUE(got["reflection"]["createdAt"].is_string());
    EXPECT_TRUE(got["reflection"]["editedAt"].is_string()) << "the merge is an edit";
    const json fixture_shape = LoadFixture("reflection-get.json")["result"];
    for (const auto& [key, value] : fixture_shape.items()) {
        EXPECT_TRUE(got.contains(key)) << key;
    }
    for (const auto& [key, value] : fixture_shape["reflection"].items()) {
        EXPECT_TRUE(got["reflection"].contains(key)) << key;
    }

    // The summary rides on the same update when the clinician corrects it
    ASSERT_TRUE(std::holds_alternative<json>(HandleReflectionUpdate(
        *fixture.store, json{{"id", id}, {"summary", "A patient in their forties."}})));
    got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(got["summary"]["text"], "A patient in their forties.");
    EXPECT_TRUE(got["summary"]["editedAt"].is_string());

    // Listed with the card's line
    const json list = HandleReflectionList(*fixture.store)["reflections"];
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0]["id"], id);
    EXPECT_EQ(list[0]["label"], "Elbow swelling");
    EXPECT_EQ(list[0]["learned"], "check the temperature");
    // Named first: iterating a temporary's sub-object dangles
    const json list_fixture = LoadFixture("reflection-list.json");
    for (const auto& [key, value] : list_fixture["result"]["reflections"][0].items()) {
        EXPECT_TRUE(list[0].contains(key)) << key;
    }

    // Wrong types are parameter errors; unknown ids session errors
    EXPECT_TRUE(std::holds_alternative<Error>(
        HandleReflectionUpdate(*fixture.store, json{{"id", id}, {"learned", 3}})));
    EXPECT_TRUE(
        std::holds_alternative<Error>(HandleReflectionGet(*fixture.store, json{{"id", "nope"}})));

    ASSERT_TRUE(
        std::holds_alternative<json>(HandleReflectionDelete(*fixture.store, json{{"id", id}})));
    got = std::get<json>(HandleReflectionGet(*fixture.store, json{{"id", id}}));
    EXPECT_TRUE(got["reflection"].is_null());
    EXPECT_TRUE(got["summary"].is_null());
    EXPECT_EQ(HandleReflectionList(*fixture.store)["reflections"].size(), 0u);
}

TEST(Handlers, SessionPatientCarriesTheTranslationWhenStored) {
    using ambient::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "You have bursitis."});

    json patient = std::get<json>(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["text"], "You have bursitis.");
    EXPECT_EQ(patient["language"], "en");
    EXPECT_TRUE(patient["generatedAt"].is_string());
    EXPECT_TRUE(patient["translation"].is_null());

    fixture.store->SaveDocument(id, DocumentKind::kTranslation,
                                {.text = "Masz zapalenie kaletki.", .language = "pl"});
    patient = std::get<json>(HandleSessionPatient(*fixture.store, json{{"id", id}}));
    EXPECT_EQ(patient["translation"]["language"], "pl");
    EXPECT_EQ(patient["translation"]["text"], "Masz zapalenie kaletki.");
    const json patient_fixture = LoadFixture("session-patient.json");
    for (const auto& [key, value] : patient_fixture["result"].items()) {
        EXPECT_TRUE(patient.contains(key)) << key;
    }
}

TEST(Handlers, SessionListCarriesTheLabelAndTheLatestEdit) {
    using ambient::store::DocumentKind;
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(id, DocumentKind::kLabel, {.text = "Elbow swelling"});

    json list = HandleSessionList(*fixture.store);
    EXPECT_EQ(list["sessions"][0]["label"], "Elbow swelling");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null());

    fixture.store->EditDocument(id, DocumentKind::kLabel, "Left elbow bursitis");
    list = HandleSessionList(*fixture.store);
    EXPECT_EQ(list["sessions"][0]["label"], "Left elbow bursitis");
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_null())
        << "a retitle is housekeeping, not an edit to the record";

    fixture.store->SaveDocument(id, DocumentKind::kPatient, {.text = "sheet"});
    fixture.store->EditDocument(id, DocumentKind::kPatient, "sheet, edited");
    list = HandleSessionList(*fixture.store);
    EXPECT_TRUE(list["sessions"][0]["editedAt"].is_string()) << "any document's edit counts";
}

TEST(Handlers, SessionDeleteRemovesAndUnknownIdsError) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);

    const auto deleted = HandleSessionDelete(*fixture.store, json{{"id", id}});
    ASSERT_TRUE(std::holds_alternative<json>(deleted));
    EXPECT_TRUE(HandleSessionList(*fixture.store)["sessions"].empty());

    const auto missing = HandleSessionDelete(*fixture.store, json{{"id", "nope"}});
    ASSERT_TRUE(std::holds_alternative<Error>(missing));
    const json built = MakeError(std::int64_t{9}, std::get<Error>(missing));
    EXPECT_EQ(built, LoadFixture("session-error.json"));
}

TEST(Handlers, SessionMethodsRejectAMissingId) {
    SessionStoreFixture fixture;
    for (const json params : {json::object(), json{{"id", 42}}}) {
        const auto transcript = HandleSessionTranscript(*fixture.store, params);
        ASSERT_TRUE(std::holds_alternative<Error>(transcript)) << params.dump();
        EXPECT_EQ(std::get<Error>(transcript).code, kInvalidParams);
        const auto deleted = HandleSessionDelete(*fixture.store, params);
        ASSERT_TRUE(std::holds_alternative<Error>(deleted)) << params.dump();
    }
}

TEST(Handlers, AudioInputsCarryThePickerFields) {
    const std::vector<ambient::audio::CaptureDevice> devices{
        {"{0.0.1}.{aa}", "Microphone Array (Realtek(R) Audio)", "Microphone Array", true, false},
        {"{0.0.1}.{bb}", "Headset (H800 Hands-Free)", "Headset", false, true},
    };

    const json result = HandleAudioInputs(devices);

    ASSERT_EQ(result["devices"].size(), 2u);
    EXPECT_EQ(result["devices"][0]["id"], "{0.0.1}.{aa}");
    EXPECT_EQ(result["devices"][0]["name"], "Microphone Array (Realtek(R) Audio)");
    EXPECT_EQ(result["devices"][0]["shortName"], "Microphone Array");
    EXPECT_EQ(result["devices"][0]["isDefault"], true);
    EXPECT_EQ(result["devices"][0]["bluetooth"], false);
    EXPECT_EQ(result["devices"][1]["bluetooth"], true);
}

TEST(Handlers, NoMicrophonesIsAnEmptyListNotAnError) {
    const json result = HandleAudioInputs({});
    EXPECT_TRUE(result["devices"].is_array());
    EXPECT_TRUE(result["devices"].empty());
}

TEST(Handlers, AnchorStatusReportsOriginAndSessions) {
    const auto root = std::filesystem::temp_directory_path() / "ambient-handlers-anchor";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    ambient::diar::AnchorStore anchors(root);
    EXPECT_EQ(MakeResult(std::int64_t{3}, HandleAnchorStatus(anchors)),
              LoadFixture("anchor-status.json"));

    anchors.Accrue(std::vector<float>{1.0f, 0.0f});
    auto status = HandleAnchorStatus(anchors);
    EXPECT_EQ(status["origin"], "accrued");
    EXPECT_EQ(status["sessions"], 1);

    anchors.Replace(std::vector<float>{0.0f, 1.0f}, 1'757'000'000);
    status = HandleAnchorStatus(anchors);
    EXPECT_EQ(status["origin"], "enrolled");
    EXPECT_EQ(status["sessions"], 0);
    EXPECT_EQ(status["enrolledAt"], 1'757'000'000);
    std::filesystem::remove_all(root);
}

TEST(Handlers, AnchorClearRefusesDuringASessionAndOtherwiseForgets) {
    const auto root = std::filesystem::temp_directory_path() / "ambient-handlers-anchor-clear";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    ambient::diar::AnchorStore anchors(root);
    anchors.Accrue(std::vector<float>{1.0f, 0.0f});

    const auto refused = HandleAnchorClear(anchors, true);
    ASSERT_TRUE(std::holds_alternative<Error>(refused));
    EXPECT_EQ(std::get<Error>(refused).code, kSessionError);
    EXPECT_TRUE(anchors.Anchor().has_value()) << "nothing changed";

    const auto cleared = HandleAnchorClear(anchors, false);
    ASSERT_TRUE(std::holds_alternative<json>(cleared));
    EXPECT_FALSE(anchors.Anchor().has_value());
    EXPECT_EQ(HandleAnchorStatus(anchors)["origin"], "none");
    std::filesystem::remove_all(root);
}

TEST(Handlers, AnEmptyModelStoreListsNothing) {
    const ambient::models::ModelStore store(std::filesystem::temp_directory_path() /
                                            "ambient-no-models");
    const json result = HandleModels(store);
    EXPECT_TRUE(result["models"].is_array());
    EXPECT_TRUE(result["models"].empty());
}

TEST(Handlers, EchoRejectsAMissingOrNonStringPayload) {
    const json cases[] = {
        json::object(),
        json{{"payload", 42}},
        json{{"payload", nullptr}},
        json{{"payload", json::array({"a"})}},
    };

    for (const auto& params : cases) {
        const auto outcome = HandleEcho(params);
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, kInvalidParams) << params.dump();
    }
}

// Answers every search with one result that names the note it was given
struct EchoRetriever : ambient::guidance::IGuidanceRetriever {
    std::mutex mutex;
    std::vector<std::pair<std::string, int>> searches;
    bool fail = false;

    ambient::guidance::Results Search(const std::string& note, int limit) override {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            searches.emplace_back(note, limit);
        }
        if (fail) throw std::runtime_error("no embedding model staged");
        ambient::guidance::Results results;
        results.considered = 1;
        ambient::guidance::Result one;
        one.chunk_id = "fx100-1_1_1";
        one.trigger = note;
        results.shown.push_back(one);
        return results;
    }
    std::vector<ambient::guidance::Corpus> Corpora() override {
        return {};
    }
};

// Notifications the lane sent, waitable
struct Sent {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::string, json>> all;

    Notify Sink() {
        return [this](const std::string& method, json params) {
            const std::lock_guard<std::mutex> lock(mutex);
            all.emplace_back(method, std::move(params));
            changed.notify_all();
        };
    }
    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&] { return all.size() >= count; });
    }
};

TEST(Handlers, GuidanceReadyMatchesTheFixture) {
    ambient::guidance::Results results;
    results.considered = 40;
    ambient::guidance::Result one;
    one.corpus = "fixture";
    one.chunk_id = "fx100-1_1_1";
    one.guideline = "fx100";
    one.title = "Fictional inflammatory joint disease: assessment and management";
    one.section = "1.1 Referral";
    one.text =
        "Refer adults with persistent synovitis of undetermined cause to a specialist, and refer "
        "urgently if the small joints of the hands or feet are affected.";
    one.score = 0.8971234;
    one.trigger = "Examination shows synovitis of several MCP joints.";
    results.shown.push_back(one);

    Sent sent;
    auto request =
        GuidanceSearchRequest("a1b2c3d4e5f60718293a4b5c6d7e8f90", "note", 3, sent.Sink());
    request.on_ready(results);
    const json fixture = LoadFixture("guidance-ready.json");
    ASSERT_EQ(sent.all.size(), 1u);
    EXPECT_EQ(fixture["method"], sent.all[0].first);
    EXPECT_EQ(sent.all[0].second, fixture["params"]);
}

TEST(Handlers, GuidanceFailedMatchesTheFixture) {
    Sent sent;
    auto request =
        GuidanceSearchRequest("a1b2c3d4e5f60718293a4b5c6d7e8f90", "note", 3, sent.Sink());
    request.on_failed("guidance embedder gte-large-int8: tokenizer ignores max_length");
    const json fixture = LoadFixture("guidance-failed.json");
    ASSERT_EQ(sent.all.size(), 1u);
    EXPECT_EQ(fixture["method"], sent.all[0].first);
    EXPECT_EQ(sent.all[0].second, fixture["params"]);
}

TEST(Handlers, GuidanceCorporaMatchesTheFixture) {
    ambient::guidance::Corpus loaded;
    loaded.id = "fixture";
    loaded.name = "Fixture guidance corpus";
    loaded.licence = "invented";
    loaded.attribution = "none";
    loaded.source = "text";
    loaded.embedder = "gte-large-int8";
    loaded.sha256 = "e4f1be59be8647759ccd16d916ba9504b464f39a2799cb598ca5f0e4dc779a9f";
    loaded.chunks = 40;
    loaded.built_at = "2026-09-11T00:00:00Z";
    ambient::guidance::Corpus refused;
    refused.id = "nice-2026-08-25";
    refused.unavailable = "corpus.db sha256 does not match the manifest";

    const json fixture = LoadFixture("guidance-corpora.json");
    EXPECT_EQ(GuidanceCorporaJson({loaded, refused}), fixture["result"]);
}

TEST(Handlers, GuidanceSearchRunsTheStoredNoteThroughTheLane) {
    SessionStoreFixture fixture;
    const auto id = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(id);
    fixture.store->SaveDocument(
        id, ambient::store::DocumentKind::kNote,
        {.text = "Six weeks of synovitis in the small joints of both hands.",
         .style = "prose",
         .detail = "standard"});
    EchoRetriever retriever;
    ambient::guidance::GuidanceLane lane(retriever);
    Sent sent;

    const auto outcome = HandleGuidanceSearch(*fixture.store, lane, json{{"id", id}}, sent.Sink());
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    EXPECT_EQ(ResultOf(outcome), json::object());
    ASSERT_TRUE(sent.WaitFor(1));
    EXPECT_EQ(sent.all[0].first, "guidance/ready");
    EXPECT_EQ(sent.all[0].second["id"], id);
    EXPECT_EQ(sent.all[0].second["shown"][0]["trigger"],
              "Six weeks of synovitis in the small joints of both hands.");
    ASSERT_EQ(retriever.searches.size(), 1u);
    EXPECT_EQ(retriever.searches[0].second, kGuidanceLimit);
}

TEST(Handlers, GuidanceSearchTakesFreeTextAndALimit) {
    SessionStoreFixture fixture;
    EchoRetriever retriever;
    ambient::guidance::GuidanceLane lane(retriever);
    Sent sent;

    const auto outcome = HandleGuidanceSearch(
        *fixture.store, lane, json{{"text", "Chest pain on exertion."}, {"limit", 5}}, sent.Sink());
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    ASSERT_TRUE(sent.WaitFor(1));
    EXPECT_EQ(sent.all[0].first, "guidance/ready");
    EXPECT_TRUE(sent.all[0].second["id"].is_null());
    EXPECT_EQ(retriever.searches,
              (std::vector<std::pair<std::string, int>>{{"Chest pain on exertion.", 5}}));
}

TEST(Handlers, GuidanceSearchRefusesBadParamsAndAMissingNote) {
    SessionStoreFixture fixture;
    const auto without_note = fixture.store->Begin({16000, "", ""});
    fixture.store->Finalise(without_note);
    EchoRetriever retriever;
    ambient::guidance::GuidanceLane lane(retriever);
    Sent sent;

    struct Case {
        json params;
        int code;
    };
    const Case cases[] = {
        {json::object(), kInvalidParams},
        {json{{"text", 5}}, kInvalidParams},
        {json{{"text", "Chest pain."}, {"limit", 0}}, kInvalidParams},
        {json{{"text", "Chest pain."}, {"limit", 21}}, kInvalidParams},
        {json{{"text", ""}}, kSessionError},
        {json{{"id", "nope"}}, kSessionError},
        {json{{"id", without_note}}, kSessionError},
    };
    for (const auto& c : cases) {
        const auto outcome = HandleGuidanceSearch(*fixture.store, lane, c.params, sent.Sink());
        ASSERT_TRUE(std::holds_alternative<Error>(outcome)) << c.params.dump();
        EXPECT_EQ(std::get<Error>(outcome).code, c.code) << c.params.dump();
    }
    EXPECT_TRUE(retriever.searches.empty());
    EXPECT_TRUE(sent.all.empty());
}

TEST(Handlers, GuidanceSearchReportsAFailedSearch) {
    SessionStoreFixture fixture;
    EchoRetriever retriever;
    retriever.fail = true;
    ambient::guidance::GuidanceLane lane(retriever);
    Sent sent;

    const auto outcome =
        HandleGuidanceSearch(*fixture.store, lane, json{{"text", "Chest pain."}}, sent.Sink());
    ASSERT_TRUE(std::holds_alternative<json>(outcome));
    ASSERT_TRUE(sent.WaitFor(1));
    EXPECT_EQ(sent.all[0].first, "guidance/failed");
    EXPECT_EQ(sent.all[0].second["detail"], "no embedding model staged");
    EXPECT_TRUE(sent.all[0].second["id"].is_null());
}

}  // namespace
}  // namespace ambient::ipc
