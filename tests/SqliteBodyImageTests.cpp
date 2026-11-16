// SQLite BLOB tests: a real temporary database, byte-level round trips, FK cascade and the shared
// body-image contract. The temporary directory is removed afterwards; %LOCALAPPDATA% is untouched.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "common/Sha256.h"
#include "data/SqliteConnection.h"
#include "data/SqliteSchema.h"
#include "data/SqliteUnitOfWork.h"

void ReportTest(const char*, const char*, bool, const char*);
void RunBodyImageRepositoryContract(const char*, todo::IUnitOfWorkFactory&);

using namespace todo;

namespace {

void Check(const char* name, bool value, const char* detail = "") { ReportTest("SqliteBodyImage", name, value, detail); }

std::filesystem::path MakeCaseRoot(const char* tag) {
    static int counter = 0;
    ++counter;
    const std::string name = std::string("todolist4win_body_") + tag + "_" +
                             std::to_string(static_cast<unsigned long long>(GetTickCount64())) + "_" +
                             std::to_string(counter);
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

void Cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

std::vector<std::byte> BinaryPayload() {
    // Includes NUL and 0xFF so a text-oriented BLOB binding would visibly corrupt the round trip.
    std::vector<std::byte> bytes;
    for (int i = 0; i < 512; ++i) {
        const unsigned char value = static_cast<unsigned char>((i * 37 + 11) & 0xFF);
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

}  // namespace

void RunSqliteBodyImageTests() {
    const auto root = MakeCaseRoot("blob");
    const auto databasePath = root / "data" / "todolist.db";
    SqliteUnitOfWorkFactory factory(databasePath);
    auto ensured = factory.EnsureDatabaseAndSchema();
    Check("schema ensures", ensured.ok(), ensured.ok() ? "" : ensured.error().message.c_str());
    if (!ensured.ok()) { Cleanup(root); return; }

    // --- Binary round trip with a deliberately non-text byte sequence ------------
    const std::vector<std::byte> payload = BinaryPayload();
    const std::string key = Sha256::Hex(payload);
    BodyImage image;
    image.key = key;
    image.mime = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = payload;

    std::int64_t taskId = 0;
    {
        auto uow = factory.Create();
        Check("uow opens", uow.ok());
        if (!uow.ok()) { Cleanup(root); return; }
        auto begun = uow.value()->Begin();
        Check("Begin succeeds", begun.ok());
        Task task;
        task.title = "binary owner";
        task.note = "note";
        task.createdAt = LocalDateTime{2026, 1, 1, 0, 0, 0, 0};
        auto created = uow.value()->Tasks().Create(task);
        Check("owner task created", created.ok());
        if (!created.ok()) { Cleanup(root); return; }
        taskId = created.value().id;
        auto put = uow.value()->Images().Put(taskId, image);
        Check("binary Put succeeds", put.ok(), put.ok() ? "" : put.error().message.c_str());
        auto committed = uow.value()->Commit();
        Check("Commit succeeds", committed.ok());
    }
    {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto loaded = uow.value()->Images().Get(taskId, key);
            Check("binary Get returns every byte", loaded.ok() && loaded.value().bytes == payload);
            Check("binary Get preserves key", loaded.ok() && loaded.value().key == key);
            Check("binary Get preserves dimensions", loaded.ok() && loaded.value().width == 640 && loaded.value().height == 480);
            Check("binary Get preserves mime", loaded.ok() && loaded.value().mime == "image/png");
        } else {
            Check("uow reopens for binary Get", false);
        }
    }

    // --- Foreign key cascade: deleting the task removes both blobs ---------------
    {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto deleted = uow.value()->Tasks().DeleteTask(taskId);
            Check("owner task deleted", deleted.ok());
            auto orphan = uow.value()->Images().Get(taskId, key);
            Check("ON DELETE CASCADE removes blob", !orphan.ok() && orphan.error().code == ErrorCode::ImageUnavailable);
        } else {
            Check("uow reopens for cascade check", false);
        }
    }

    // --- Shared contract on a fresh database in the same temporary root ----------
    const auto contractPath = root / "contract" / "todolist.db";
    SqliteUnitOfWorkFactory contractFactory(contractPath);
    auto contractEnsured = contractFactory.EnsureDatabaseAndSchema();
    Check("contract schema ensures", contractEnsured.ok(), contractEnsured.ok() ? "" : contractEnsured.error().message.c_str());
    if (contractEnsured.ok()) RunBodyImageRepositoryContract("SqliteBodyContract", contractFactory);

    Cleanup(root);
}
