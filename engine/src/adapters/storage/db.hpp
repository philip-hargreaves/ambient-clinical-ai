#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "ports/store_error.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace ambient::store {

class Db {
   public:
    class Stmt {
       public:
        Stmt(Stmt&& other) noexcept;
        Stmt& operator=(Stmt&& other) noexcept;
        ~Stmt();

        void BindInt64(int index, std::int64_t value);
        void BindText(int index, std::string_view value);
        void BindBlob(int index, std::span<const std::uint8_t> value);
        void BindNull(int index);
        // Empty binds NULL: the store's optional texts are empty strings in C++
        void BindTextOrNull(int index, std::string_view value);

        bool Step();  // true: a row is ready; false: done
        void Reset();

        std::int64_t ColumnInt64(int index) const;
        std::string ColumnText(int index) const;
        std::vector<std::uint8_t> ColumnBlob(int index) const;
        // Valid until the next Step or Reset; for large blobs read once
        std::span<const std::uint8_t> ColumnBlobView(int index) const;

       private:
        friend class Db;
        Stmt(sqlite3_stmt* stmt, sqlite3* db);

        sqlite3_stmt* stmt_;
        sqlite3* db_;
    };

    // Rolls back on destruction unless Commit() ran
    class Transaction {
       public:
        explicit Transaction(Db& db);
        ~Transaction();
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        void Commit();

       private:
        Db& db_;
        bool done_ = false;
    };

    // kSession: the clinical store's pragmas (WAL, synchronous FULL, foreign keys).
    // kBuild: read-write, create, no pragmas; the caller sets the file's shape.
    // kImmutableReadOnly: a finished file that nothing writes; no locking, no journal
    enum class Mode { kSession, kBuild, kImmutableReadOnly };

    explicit Db(const std::filesystem::path& path, Mode mode = Mode::kSession);
    Db(Db&& other) noexcept;
    Db& operator=(Db&& other) noexcept;
    ~Db();

    void Exec(const char* sql);
    Stmt Prepare(const char* sql);
    std::int64_t QueryInt64(const char* sql);

    // Header marks, both transactional: the writing application and the schema version
    std::int64_t ApplicationId();
    void SetApplicationId(std::int64_t id);
    std::int64_t UserVersion();
    void SetUserVersion(std::int64_t version);

    // Folds the WAL into the file and truncates it; false when a reader held the log
    bool CheckpointTruncate();

   private:
    sqlite3* db_ = nullptr;
};

}  // namespace ambient::store
