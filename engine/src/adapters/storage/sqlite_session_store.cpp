#include "adapters/storage/sqlite_session_store.hpp"

#include <cstdio>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <stdexcept>
#include <utility>

#include "adapters/storage/schema.hpp"

namespace ambient::store {

namespace {

// 1 was a catalog beside one file per session; 2 the single database;
// 3 adds retain; 4 the summary and reflection kinds; 5 the demo flag;
// 6 the document sequence and the guidance kind
constexpr std::int64_t kSchemaVersion = 6;
// "AMBC": the header mark of a clinical store
constexpr std::int64_t kApplicationId = 0x414D4243;
// Audio held in memory while the disk refuses commits; older frames are dropped and counted
constexpr std::chrono::seconds kPendingBound(30);
constexpr std::int64_t kSqlitePageLimit = 1073741823;  // the default max_page_count

struct KindSpec {
    const char* name;  // documents.kind
    Domain domain;     // sealing domain
};

KindSpec SpecFor(DocumentKind kind) {
    switch (kind) {
        case DocumentKind::kNote:
            return {"note", Domain::kNote};
        case DocumentKind::kPatient:
            return {"patient", Domain::kPatient};
        case DocumentKind::kTranslation:
            return {"translation", Domain::kTranslation};
        case DocumentKind::kLabel:
            return {"label", Domain::kLabel};
        case DocumentKind::kSummary:
            return {"summary", Domain::kSummary};
        case DocumentKind::kReflection:
            return {"reflection", Domain::kReflection};
    }
    throw std::invalid_argument("unknown document kind");
}

std::string Iso8601Now() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%T}Z", now);
}

std::string RandomId() {
    std::random_device device;
    std::string id;
    for (int i = 0; i < 4; ++i) id += std::format("{:08x}", device());
    return id;
}

std::span<const std::uint8_t> AsBytes(std::span<const float> frames) {
    return {reinterpret_cast<const std::uint8_t*>(frames.data()), frames.size_bytes()};
}

std::span<const std::uint8_t> AsBytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool TableExists(Db& db, const char* table) {
    Db::Stmt exists = db.Prepare("SELECT count(*) FROM sqlite_master WHERE name = ?");
    exists.BindText(1, table);
    return exists.Step() && exists.ColumnInt64(0) != 0;
}

bool HasColumn(Db& db, const char* table, const char* column) {
    Db::Stmt info = db.Prepare(("PRAGMA table_info(" + std::string(table) + ")").c_str());
    while (info.Step()) {
        if (info.ColumnText(1) == column) return true;
    }
    return false;
}

// A row per dangling reference, none when the rebuild kept every one
void RequireForeignKeys(Db& db) {
    Db::Stmt check = db.Prepare("PRAGMA foreign_key_check");
    if (check.Step()) throw StoreError(StoreCode::kSchema, "migration left a dangling reference");
}

std::filesystem::path DatabasePath(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    return root / "ambient.db";
}

// Foreign and newer files are refused. Each step stamps its version inside its transaction;
// the column additions are skipped where an older build's half-stamped file already has them
Db OpenDatabase(const std::filesystem::path& root) {
    Db db(DatabasePath(root));
    const std::int64_t application_id = db.ApplicationId();
    std::int64_t version = db.UserVersion();
    if (application_id != 0 && application_id != kApplicationId) {
        throw StoreError(StoreCode::kSchema, "not an ambient store");
    }
    if (version == 0) {
        if (db.QueryInt64("SELECT count(*) FROM sqlite_master") != 0) {
            throw StoreError(StoreCode::kSchema, "not an ambient store");
        }
        // Incremental vacuum is creation-time; the WAL switch already wrote the
        // header, so the empty file is rebuilt to take it
        db.Exec("PRAGMA auto_vacuum=INCREMENTAL");
        db.Exec("VACUUM");
        Db::Transaction txn(db);
        db.Exec(kSchemaSql);
        db.SetApplicationId(kApplicationId);
        db.SetUserVersion(kSchemaVersion);
        txn.Commit();
    } else if (version > kSchemaVersion) {
        throw StoreError(StoreCode::kSchema, "store schema is newer than this build");
    } else {
        if (application_id == 0 && !TableExists(db, "sessions")) {
            throw StoreError(StoreCode::kSchema, "not an ambient store");
        }
        if (version == 2) {
            Db::Transaction txn(db);
            if (!HasColumn(db, "sessions", "retain")) db.Exec(kMigrate2To3Sql);
            db.SetUserVersion(3);
            txn.Commit();
            version = 3;
        }
        if (version == 3) {
            // note_options references the dropped table: keys off, or the drop cascades. The pragma
            // is a no-op inside a transaction
            db.Exec("PRAGMA foreign_keys=OFF");
            {
                Db::Transaction txn(db);
                db.Exec(kMigrate3To4Sql);
                db.SetUserVersion(4);
                txn.Commit();
            }
            db.Exec("PRAGMA foreign_keys=ON");
            version = 4;
        }
        if (version == 4) {
            Db::Transaction txn(db);
            if (!HasColumn(db, "sessions", "demo")) db.Exec(kMigrate4To5Sql);
            db.SetUserVersion(5);
            txn.Commit();
            version = 5;
        }
        if (version == 5) {
            db.Exec("PRAGMA foreign_keys=OFF");
            {
                Db::Transaction txn(db);
                if (!HasColumn(db, "documents", "seq")) db.Exec(kMigrate5To6Sql);
                RequireForeignKeys(db);
                db.SetUserVersion(6);
                txn.Commit();
            }
            db.Exec("PRAGMA foreign_keys=ON");
            // Every earlier rewrite resealed under one IV; the copies sit in freed pages
            db.Exec("VACUUM");
        }
        // Stores from before the mark take it once
        if (application_id == 0) db.SetApplicationId(kApplicationId);
    }
    return db;
}

}  // namespace

SqliteSessionStore::SqliteSessionStore(const std::filesystem::path& root,
                                       std::chrono::milliseconds commit_interval)
    : commit_interval_(commit_interval), db_(OpenDatabase(root)) {
    ImportPerSessionFiles(root);
    writer_ = std::thread([this] { WriterLoop(); });
}

SqliteSessionStore::~SqliteSessionStore() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    writer_.join();
    // Destruction is not finalisation; an open session stays recoverable with what it buffered
    if (open_.has_value()) {
        try {
            CommitPending();
        } catch (const StoreError& e) {
            std::fprintf(stderr, "ambient-engine: store commit failed at close: %s\n", e.what());
        }
    }
}

SessionId SqliteSessionStore::Begin(const SessionMeta& meta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.has_value()) throw StoreError(StoreCode::kBusy, "a session is already recording");

    Open session;
    session.id = RandomId();
    session.sample_rate = static_cast<std::uint64_t>(meta.sample_rate);
    session.cipher.emplace(ChunkCipher::Generate());
    const std::vector<std::uint8_t> wrapped = session.cipher->Wrapped();

    // Row and key land together: a session either exists with its key or
    // not at all
    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, state, sample_rate, device_id, device_name, retain)"
        " VALUES(?, ?, 'recording', ?, ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindText(2, Iso8601Now());
    insert.BindInt64(3, meta.sample_rate);
    insert.BindTextOrNull(4, meta.device_id);
    insert.BindTextOrNull(5, meta.device_name);
    insert.BindInt64(6, meta.retain ? 1 : 0);
    insert.Step();
    InsertKey(session.id, wrapped);
    txn.Commit();

    open_.emplace(std::move(session));
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex_);
        pending_ = {open_->id, {}, 0};
    }
    return open_->id;
}

void SqliteSessionStore::Append(const SessionId& id, std::span<const float> frames,
                                std::uint64_t lost_frames) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_.id != id) {
        throw StoreError(StoreCode::kNotFound, "no open session with id " + id);
    }
    pending_.lost += lost_frames;
    pending_.frames.insert(pending_.frames.end(), frames.begin(), frames.end());
}

void SqliteSessionStore::TakePending(Open& session) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    session.held.insert(session.held.end(), pending_.frames.begin(), pending_.frames.end());
    session.held_lost += pending_.lost;
    pending_.frames.clear();
    pending_.lost = 0;
}

void SqliteSessionStore::ClosePending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_ = {};
}

// Timing is queryable shape; speaker and text are content, so encrypted
void SqliteSessionStore::InsertTurn(const SessionId& id, std::int64_t seq,
                                    const ChunkCipher& cipher, const asr::Turn& turn) {
    const std::string content =
        nlohmann::json{{"speaker", turn.speaker}, {"text", turn.text}}.dump();
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(Domain::kTurns, id, static_cast<std::uint64_t>(seq), AsBytes(content));
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO turns(session_id, seq, first_frame, frame_count, payload)"
        " VALUES(?, ?, ?, ?, ?)");
    insert.BindText(1, id);
    insert.BindInt64(2, seq);
    insert.BindInt64(3, static_cast<std::int64_t>(turn.first_frame));
    insert.BindInt64(4, static_cast<std::int64_t>(turn.frame_count));
    insert.BindBlob(5, sealed);
    insert.Step();
}

void SqliteSessionStore::AppendTurn(const SessionId& id, const asr::Turn& turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    Db::Transaction txn(db_);
    InsertTurn(session.id, session.next_turn_seq, *session.cipher, turn);
    txn.Commit();
    session.next_turn_seq += 1;
}

void SqliteSessionStore::ReplaceTurns(const SessionId& id, std::span<const asr::Turn> turns) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);

    Db::Transaction txn(db_);
    Db::Stmt erase = db_.Prepare("DELETE FROM turns WHERE session_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    for (const asr::Turn& turn : turns) {
        // Sequence numbers continue rather than restart, so every sealed
        // payload's AAD stays unique for the session's lifetime
        InsertTurn(session.id, session.next_turn_seq, *session.cipher, turn);
        session.next_turn_seq += 1;
    }
    txn.Commit();
}

// The seal erases the audio: the transcript is the record; the recording
// existed only to resume a crash
void SqliteSessionStore::Finalise(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    Open& session = RequireOpen(id);
    TakePending(session);
    const std::uint64_t lost = session.lost_committed + session.held_lost;

    Db::Transaction txn(db_);
    Db::Stmt erase = db_.Prepare("DELETE FROM chunks WHERE session_id = ?");
    erase.BindText(1, session.id);
    erase.Step();
    Db::Stmt update = db_.Prepare(
        "UPDATE sessions SET ended_at = ?, state = 'finalised', lost_frames = ? WHERE id = ?");
    update.BindText(1, Iso8601Now());
    update.BindInt64(2, static_cast<std::int64_t>(lost));
    update.BindText(3, session.id);
    update.Step();
    txn.Commit();
    db_.Exec("PRAGMA incremental_vacuum");

    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Abandon(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireOpen(id);
    CommitPending();
    // No state change: recording is what marks it recoverable
    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Cancel(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireOpen(id);
    Erase(id);
    open_.reset();
    ClosePending();
}

void SqliteSessionStore::Delete(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (open_.has_value() && open_->id == id) {
        throw StoreError(StoreCode::kBusy, id + " is still recording");
    }
    Erase(id);
}

// The key row goes with the session (cascade): whatever ciphertext lingers
// in free pages afterwards is noise without it
void SqliteSessionStore::Erase(const SessionId& id) {
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE id = ?");
    erase.BindText(1, id);
    if (EraseWhere(erase) == 0) throw StoreError(StoreCode::kNotFound, "no session " + id);
}

std::size_t SqliteSessionStore::EraseWhere(Db::Stmt& erase) {
    erase.Step();
    const auto removed = static_cast<std::size_t>(db_.QueryInt64("SELECT changes()"));
    if (removed > 0) Checkpoint();
    return removed;
}

void SqliteSessionStore::EraseUnretained() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE retain = 0 AND state = 'finalised'");
    EraseWhere(erase);
}

std::vector<RecoverableSession> SqliteSessionStore::ScanRecoverable() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RecoverableSession> found;
    Db::Stmt select =
        db_.Prepare("SELECT id, started_at, sample_rate FROM sessions WHERE state = 'recording'");
    while (select.Step()) {
        RecoverableSession session{select.ColumnText(0), select.ColumnText(1),
                                   static_cast<int>(select.ColumnInt64(2))};
        if (open_.has_value() && session.id == open_->id) continue;  // live, not crashed
        found.push_back(std::move(session));
    }
    return found;
}

// The label is content, so each row's is opened with its own key; the
// edit stamp is the latest over the session's documents
std::vector<SessionSummary> SqliteSessionStore::ListSessions() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionSummary> sessions;
    Db::Stmt select = db_.Prepare(
        "SELECT s.id, s.started_at, s.ended_at, s.state, s.sample_rate, k.wrapped, l.payload,"
        // Label, summary and reflection are not edits to the record
        " (SELECT max(edited_at) FROM documents d WHERE d.session_id = s.id"
        "  AND d.kind NOT IN ('label', 'summary', 'reflection')),"
        // The audio's length outlives the audio: the turns' end is plaintext
        " (SELECT max(first_frame + frame_count) FROM turns t WHERE t.session_id = s.id),"
        " EXISTS(SELECT 1 FROM documents r WHERE r.session_id = s.id"
        "  AND r.kind IN ('reflection', 'summary')),"
        " s.demo, l.seq"
        " FROM sessions s"
        " LEFT JOIN session_keys k ON k.session_id = s.id"
        " LEFT JOIN documents l ON l.session_id = s.id AND l.kind = 'label'"
        // A keep-off session exists only until it is left; history never
        // shows what is not being kept. Crashed ones stay for recovery
        " WHERE NOT (s.retain = 0 AND s.state = 'finalised')"
        " ORDER BY s.started_at DESC, s.rowid DESC");
    while (select.Step()) {
        SessionSummary summary{select.ColumnText(0), select.ColumnText(1), select.ColumnText(2),
                               select.ColumnText(3), static_cast<int>(select.ColumnInt64(4))};
        const std::vector<std::uint8_t> sealed = select.ColumnBlob(6);
        if (!sealed.empty()) {
            try {
                const ChunkCipher cipher = ChunkCipher::FromWrapped(select.ColumnBlob(5));
                const auto plain =
                    cipher.Open(Domain::kLabel, summary.id,
                                static_cast<std::uint64_t>(select.ColumnInt64(11)), sealed);
                summary.label.assign(plain.begin(), plain.end());
            } catch (const StoreError&) {  // one unreadable label never hides the list
            }
        }
        summary.edited_at = select.ColumnText(7);
        if (summary.sample_rate > 0) {
            summary.audio_seconds =
                static_cast<double>(select.ColumnInt64(8)) / summary.sample_rate;
        }
        summary.has_reflection = select.ColumnInt64(9) != 0;
        summary.demo = select.ColumnInt64(10) != 0;
        sessions.push_back(std::move(summary));
    }
    return sessions;
}

// Row, key and turns land in one transaction, already finalised
SessionId SqliteSessionStore::Seed(const SessionSeed& seed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SessionId id = RandomId();
    const ChunkCipher cipher = ChunkCipher::Generate();

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO sessions(id, started_at, ended_at, state, sample_rate, retain, demo)"
        " VALUES(?, ?, ?, 'finalised', ?, 1, 1)");
    insert.BindText(1, id);
    insert.BindText(2, seed.started_at);
    insert.BindText(3, seed.ended_at);
    insert.BindInt64(4, seed.sample_rate);
    insert.Step();
    InsertKey(id, cipher.Wrapped());
    std::int64_t seq = 0;
    for (const asr::Turn& turn : seed.turns) {
        InsertTurn(id, seq, cipher, turn);
        seq += 1;
    }
    txn.Commit();
    return id;
}

std::size_t SqliteSessionStore::DeleteAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE id <> ?");
    erase.BindText(1, open_.has_value() ? open_->id : std::string());
    return EraseWhere(erase);
}

std::size_t SqliteSessionStore::ClearDemo() {
    std::lock_guard<std::mutex> lock(mutex_);
    Db::Stmt erase = db_.Prepare("DELETE FROM sessions WHERE demo = 1");
    return EraseWhere(erase);
}

void SqliteSessionStore::RequireStored(const SessionId& id) {
    if (open_.has_value() && open_->id == id) {
        throw StoreError(StoreCode::kBusy, id + " is still recording");
    }
    Db::Stmt select = db_.Prepare("SELECT 1 FROM session_keys WHERE session_id = ?");
    select.BindText(1, id);
    if (!select.Step()) throw StoreError(StoreCode::kNotFound, "no session " + id);
}

ChunkCipher SqliteSessionStore::CipherFor(const SessionId& id) {
    RequireStored(id);
    Db::Stmt select = db_.Prepare("SELECT wrapped FROM session_keys WHERE session_id = ?");
    select.BindText(1, id);
    select.Step();
    return ChunkCipher::FromWrapped(select.ColumnBlob(0));
}

void SqliteSessionStore::InsertKey(const SessionId& id, std::span<const std::uint8_t> wrapped) {
    Db::Stmt key = db_.Prepare("INSERT INTO session_keys(session_id, wrapped) VALUES(?, ?)");
    key.BindText(1, id);
    key.BindBlob(2, wrapped);
    key.Step();
}

// Text is content, so sealed; the rest is shape. The options row follows
// the note row (cascade), so a kind is all or nothing
void SqliteSessionStore::WriteDocument(const SessionId& id, DocumentKind kind,
                                       const Document& document) {
    const ChunkCipher cipher = CipherFor(id);
    const KindSpec spec = SpecFor(kind);

    Db::Transaction txn(db_);
    // The slot's next sequence: a rewrite never reseals under a used IV
    std::int64_t seq = 1;
    {
        Db::Stmt previous =
            db_.Prepare("SELECT seq FROM documents WHERE session_id = ? AND kind = ?");
        previous.BindText(1, id);
        previous.BindText(2, spec.name);
        if (previous.Step()) seq = previous.ColumnInt64(0) + 1;
    }
    const std::vector<std::uint8_t> sealed =
        cipher.Seal(spec.domain, id, static_cast<std::uint64_t>(seq), AsBytes(document.text));
    Db::Stmt replace = db_.Prepare(
        "INSERT OR REPLACE INTO documents"
        "(session_id, kind, seq, language, payload, generated_at, edited_at)"
        " VALUES(?, ?, ?, ?, ?, ?, ?)");
    replace.BindText(1, id);
    replace.BindText(2, spec.name);
    replace.BindInt64(3, seq);
    replace.BindText(4, document.language);
    replace.BindBlob(5, sealed);
    replace.BindTextOrNull(6, document.generated_at);
    replace.BindTextOrNull(7, document.edited_at);
    replace.Step();
    if (kind == DocumentKind::kNote) {
        Db::Stmt options = db_.Prepare(
            "INSERT OR REPLACE INTO note_options(session_id, style, detail) VALUES(?, ?, ?)");
        options.BindText(1, id);
        options.BindText(2, document.style);
        options.BindText(3, document.detail);
        options.Step();
    }
    txn.Commit();
}

void SqliteSessionStore::SaveDocument(const SessionId& id, DocumentKind kind,
                                      const Document& document) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document generated = document;
    generated.generated_at = Iso8601Now();
    generated.edited_at.clear();
    WriteDocument(id, kind, generated);
}

void SqliteSessionStore::EditDocument(const SessionId& id, DocumentKind kind,
                                      const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    Document edited = ReadDocumentLocked(id, kind);
    edited.text = text;
    edited.edited_at = Iso8601Now();
    WriteDocument(id, kind, edited);
}

Document SqliteSessionStore::ReadDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ReadDocumentLocked(id, kind);
}

void SqliteSessionStore::DeleteDocument(const SessionId& id, DocumentKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    RequireStored(id);
    Db::Stmt remove = db_.Prepare("DELETE FROM documents WHERE session_id = ? AND kind = ?");
    remove.BindText(1, id);
    remove.BindText(2, SpecFor(kind).name);
    remove.Step();
}

Document SqliteSessionStore::ReadDocumentLocked(const SessionId& id, DocumentKind kind) {
    const ChunkCipher cipher = CipherFor(id);
    const KindSpec spec = SpecFor(kind);
    Document document;
    Db::Stmt select = db_.Prepare(
        "SELECT d.language, d.payload, d.generated_at, d.edited_at, o.style, o.detail, d.seq"
        " FROM documents d LEFT JOIN note_options o ON o.session_id = d.session_id"
        " WHERE d.session_id = ? AND d.kind = ?");
    select.BindText(1, id);
    select.BindText(2, spec.name);
    if (select.Step()) {
        const auto plain =
            cipher.Open(spec.domain, id, static_cast<std::uint64_t>(select.ColumnInt64(6)),
                        select.ColumnBlob(1));
        document.text.assign(plain.begin(), plain.end());
        document.language = select.ColumnText(0);
        document.generated_at = select.ColumnText(2);
        document.edited_at = select.ColumnText(3);
        if (kind == DocumentKind::kNote) {
            document.style = select.ColumnText(4);
            document.detail = select.ColumnText(5);
        }
    }
    return document;
}

std::vector<asr::Turn> SqliteSessionStore::ReadTurns(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    Db::Stmt select = db_.Prepare(
        "SELECT seq, first_frame, frame_count, payload FROM turns WHERE session_id = ?"
        " ORDER BY seq");
    select.BindText(1, id);
    std::vector<asr::Turn> turns;
    while (select.Step()) {
        const auto plain =
            cipher.Open(Domain::kTurns, id, static_cast<std::uint64_t>(select.ColumnInt64(0)),
                        select.ColumnBlob(3));
        const auto content = nlohmann::json::parse(plain.begin(), plain.end());
        asr::Turn turn;
        turn.first_frame = static_cast<std::uint64_t>(select.ColumnInt64(1));
        turn.frame_count = static_cast<std::uint64_t>(select.ColumnInt64(2));
        turn.speaker = content.at("speaker").get<std::string>();
        turn.text = content.at("text").get<std::string>();
        turns.push_back(std::move(turn));
    }
    return turns;
}

std::vector<float> SqliteSessionStore::ReadAudio(const SessionId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const ChunkCipher cipher = CipherFor(id);
    Db::Stmt select =
        db_.Prepare("SELECT seq, payload FROM chunks WHERE session_id = ? ORDER BY seq");
    select.BindText(1, id);
    std::vector<float> audio;
    while (select.Step()) {
        const auto plain =
            cipher.Open(Domain::kAudio, id, static_cast<std::uint64_t>(select.ColumnInt64(0)),
                        select.ColumnBlob(1));
        const auto* frames = reinterpret_cast<const float*>(plain.data());
        audio.insert(audio.end(), frames, frames + plain.size() / sizeof(float));
    }
    return audio;
}

SqliteSessionStore::Open& SqliteSessionStore::RequireOpen(const SessionId& id) {
    if (!open_.has_value() || open_->id != id) {
        throw StoreError(StoreCode::kNotFound, "no open session with id " + id);
    }
    return *open_;
}

bool SqliteSessionStore::CommitPending() {
    Open& session = *open_;
    TakePending(session);
    if (session.held.empty() && session.held_lost == 0) return false;
    const std::vector<std::uint8_t> sealed =
        session.cipher->Seal(Domain::kAudio, session.id,
                             static_cast<std::uint64_t>(session.next_seq), AsBytes(session.held));

    Db::Transaction txn(db_);
    Db::Stmt insert = db_.Prepare(
        "INSERT INTO chunks(session_id, seq, first_frame, frame_count, lost_before, payload)"
        " VALUES(?, ?, ?, ?, ?, ?)");
    insert.BindText(1, session.id);
    insert.BindInt64(2, session.next_seq);
    insert.BindInt64(3, static_cast<std::int64_t>(session.frames_committed));
    insert.BindInt64(4, static_cast<std::int64_t>(session.held.size()));
    insert.BindInt64(5, static_cast<std::int64_t>(session.held_lost));
    insert.BindBlob(6, sealed);
    insert.Step();
    txn.Commit();

    session.next_seq += 1;
    session.frames_committed += session.held.size();
    session.lost_committed += session.held_lost;
    session.held.clear();
    session.held_lost = 0;
    return true;
}

// A failed commit keeps its audio for the next tick; past kPendingBound the oldest frames are
// dropped as lost and the stored timeline moves with them. The fault is announced once per episode
void SqliteSessionStore::WriterLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        cv_.wait_for(lock, commit_interval_, [this] { return stopping_; });
        if (stopping_) break;
        if (!open_.has_value()) continue;
        try {
            if (CommitPending() && open_->faulted) {
                open_->faulted = false;
                std::fprintf(stderr, "ambient-engine: store commits again\n");
            }
        } catch (const StoreError& e) {
            Open& session = *open_;
            const std::uint64_t bound = kPendingBound.count() * session.sample_rate;
            if (session.held.size() > bound) {
                const auto dropped = session.held.size() - bound;
                session.held.erase(session.held.begin(),
                                   session.held.begin() + static_cast<std::ptrdiff_t>(dropped));
                session.frames_committed += dropped;
                session.held_lost += dropped;
            }
            if (!session.faulted) {
                session.faulted = true;
                std::fprintf(stderr, "ambient-engine: store commit failed: %s\n", e.what());
                if (on_fault_) {
                    const auto listener = on_fault_;
                    lock.unlock();
                    listener(e);
                    lock.lock();
                }
            }
        }
    }
}

void SqliteSessionStore::SetFaultListener(std::function<void(const StoreError&)> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_fault_ = std::move(listener);
}

void SqliteSessionStore::SetMaxPageCount(std::int64_t pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    db_.Exec(
        ("PRAGMA max_page_count=" + std::to_string(pages > 0 ? pages : kSqlitePageLimit)).c_str());
}

void SqliteSessionStore::Checkpoint() {
    if (!db_.CheckpointTruncate()) {
        std::fprintf(stderr, "ambient-engine: store log kept, a reader holds it\n");
    }
}

// Layout 1 import (pre ambient #35): rows move unchanged; a session that
// fails to import keeps its files
void SqliteSessionStore::ImportPerSessionFiles(const std::filesystem::path& root) {
    const std::filesystem::path catalog_path = root / "main.db";
    if (!std::filesystem::exists(catalog_path)) return;

    struct Legacy {
        std::string id, started_at, ended_at, state, device_id, device_name;
        std::int64_t sample_rate = 0, lost_frames = 0;
    };
    std::vector<Legacy> rows;
    {
        Db catalog(catalog_path);
        Db::Stmt select = catalog.Prepare(
            "SELECT id, started_at, ended_at, state, sample_rate, device_id, device_name,"
            " lost_frames FROM sessions");
        while (select.Step()) {
            rows.push_back({select.ColumnText(0), select.ColumnText(1), select.ColumnText(2),
                            select.ColumnText(3), select.ColumnText(5), select.ColumnText(6),
                            select.ColumnInt64(4), select.ColumnInt64(7)});
        }
    }

    bool all_imported = true;
    for (const Legacy& row : rows) {
        const std::filesystem::path base = root / "sessions" / row.id;
        const std::filesystem::path db_path = base.string() + ".db";
        const std::filesystem::path key_path = base.string() + ".key";
        try {
            if (!std::filesystem::exists(db_path) || !std::filesystem::exists(key_path)) {
                throw StoreError(StoreCode::kIo, "session files missing");
            }
            Db::Transaction txn(db_);
            Db::Stmt insert = db_.Prepare(
                "INSERT INTO sessions(id, started_at, ended_at, state, sample_rate, device_id,"
                " device_name, lost_frames) VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
            insert.BindText(1, row.id);
            insert.BindText(2, row.started_at);
            insert.BindTextOrNull(3, row.ended_at);
            insert.BindText(4, row.state);
            insert.BindInt64(5, row.sample_rate);
            insert.BindTextOrNull(6, row.device_id);
            insert.BindTextOrNull(7, row.device_name);
            insert.BindInt64(8, row.lost_frames);
            insert.Step();
            InsertKey(row.id, ReadFileBytes(key_path));
            {
                Db session(db_path);
                // Audio crosses only for a session still to be recovered; a
                // finalised one is held to the seal-erases-audio rule
                Db::Stmt chunks = session.Prepare(
                    "SELECT seq, first_frame, frame_count, lost_before, payload FROM chunks");
                while (row.state == "recording" && chunks.Step()) {
                    Db::Stmt copy = db_.Prepare(
                        "INSERT INTO chunks(session_id, seq, first_frame, frame_count,"
                        " lost_before, payload) VALUES(?, ?, ?, ?, ?, ?)");
                    copy.BindText(1, row.id);
                    for (int i = 0; i < 4; ++i) copy.BindInt64(i + 2, chunks.ColumnInt64(i));
                    copy.BindBlob(6, chunks.ColumnBlob(4));
                    copy.Step();
                }
                Db::Stmt turns =
                    session.Prepare("SELECT seq, first_frame, frame_count, payload FROM turns");
                while (turns.Step()) {
                    Db::Stmt copy = db_.Prepare(
                        "INSERT INTO turns(session_id, seq, first_frame, frame_count, payload)"
                        " VALUES(?, ?, ?, ?, ?)");
                    copy.BindText(1, row.id);
                    for (int i = 0; i < 3; ++i) copy.BindInt64(i + 2, turns.ColumnInt64(i));
                    copy.BindBlob(5, turns.ColumnBlob(3));
                    copy.Step();
                }
                // The first layout kept the note and sheet in tables of their own
                for (const char* kind : {"note", "patient"}) {
                    if (!TableExists(session, kind)) continue;
                    Db::Stmt text = session.Prepare(
                        (std::string("SELECT payload FROM ") + kind + " WHERE seq = 0").c_str());
                    if (!text.Step()) continue;
                    Db::Stmt copy = db_.Prepare(
                        "INSERT INTO documents(session_id, kind, language, payload)"
                        " VALUES(?, ?, 'en', ?)");
                    copy.BindText(1, row.id);
                    copy.BindText(2, kind);
                    copy.BindBlob(3, text.ColumnBlob(0));
                    copy.Step();
                }
            }
            txn.Commit();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ambient-engine: session %s not imported (%s)\n", row.id.c_str(),
                         e.what());
            all_imported = false;
            continue;
        }
        std::error_code ignored;
        for (const char* suffix : {".key", ".db", ".db-wal", ".db-shm"}) {
            std::filesystem::remove(base.string() + suffix, ignored);
        }
    }

    if (all_imported) {
        std::error_code ignored;
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(catalog_path.string() + suffix, ignored);
        }
        std::filesystem::remove(root / "sessions", ignored);  // only when empty
    }
}

}  // namespace ambient::store
