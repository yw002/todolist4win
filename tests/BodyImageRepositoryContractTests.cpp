// Shared body/image contract. The entry point only takes IUnitOfWorkFactory&, so every suite can
// run the exact same assertions against its own factory.
// Win32 UI, no direct SQL: everything goes through the repositories, TaskService and pure domain
// functions. All database files are supplied by the caller (a temporary directory in the default
// suite), so the real %LOCALAPPDATA% is never touched.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Clock.h"
#include "common/Error.h"
#include "common/Sha256.h"
#include "core/RepositoryInterfaces.h"
#include "core/TaskService.h"
#include "domain/BodyDocument.h"
#include "domain/TaskRules.h"

void ReportTest(const char*, const char*, bool, const char*);

namespace {

using namespace todo;

void Check(const char* suite, const char* name, bool value, const char* detail = "") {
    ReportTest(suite, name, value, detail);
}

std::vector<std::byte> Png(const std::string& seed) {
    std::vector<std::byte> bytes;
    for (const char value : seed) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    return bytes;
}

BodyImage MakeImage(const std::string& seed, std::uint32_t width = 16, std::uint32_t height = 8) {
    BodyImage image;
    image.bytes = Png(seed);
    image.key = Sha256::Hex(image.bytes);
    image.mime = "image/png";
    image.width = width;
    image.height = height;
    return image;
}

BodyDocument MakeDocument(const std::vector<std::string>& keys) {
    BodyDocument document = BodyDocument::Empty();
    document.blocks[0].text = "正文第一段\n第二段";
    for (const std::string& key : keys) {
        BodyBlock block;
        block.kind = BodyBlockKind::Image;
        block.image.key = key;
        block.image.mime = "image/png";
        block.image.width = 16;
        block.image.height = 8;
        document.blocks.push_back(std::move(block));
    }
    BodyBlock trailing;
    trailing.kind = BodyBlockKind::Text;
    trailing.text = "结尾";
    document.blocks.push_back(std::move(trailing));
    return document;
}

class FixedClock final : public IClock {
public:
    LocalDateTime now{2026, 6, 1, 9, 0, 0, 0};
    [[nodiscard]] LocalDateTime NowLocal() const override { return now; }
};

std::int64_t CreateTaskRow(IUnitOfWorkFactory& factory) {
    auto uow = factory.Create();
    if (!uow.ok()) return 0;
    auto begun = uow.value()->Begin();
    if (!begun.ok()) return 0;
    Task task;
    task.title = "blob owner";
    task.note = "plain text";
    task.createdAt = LocalDateTime{2026, 1, 1, 0, 0, 0, 0};
    auto created = uow.value()->Tasks().Create(task);
    if (!created.ok()) return 0;
    auto committed = uow.value()->Commit();
    if (!committed.ok()) return 0;
    return created.value().id;
}

}  // namespace

void RunBodyImageRepositoryContract(const char* suite, IUnitOfWorkFactory& factory) {
    const BodyImage first = MakeImage("first-png-bytes");
    const BodyImage second = MakeImage("second-png-bytes", 32, 16);
    const std::int64_t taskId = CreateTaskRow(factory);
    Check(suite, "fixture task created", taskId > 0);
    if (taskId <= 0) return;

    // --- Put / Get / idempotency -------------------------------------------------
    {
        auto uow = factory.Create();
        if (!uow.ok()) { Check(suite, "uow opens for Put", false); return; }
        auto begun = uow.value()->Begin();
        Check(suite, "Begin for Put succeeds", begun.ok());
        auto put = uow.value()->Images().Put(taskId, first);
        Check(suite, "Put succeeds", put.ok(), put.ok() ? "" : put.error().message.c_str());
        auto putAgain = uow.value()->Images().Put(taskId, first);
        Check(suite, "Put is idempotent", putAgain.ok());
        auto secondPut = uow.value()->Images().Put(taskId, second);
        Check(suite, "second Put succeeds", secondPut.ok());
        auto loaded = uow.value()->Images().Get(taskId, first.key);
        Check(suite, "Get returns bytes byte-for-byte", loaded.ok() && loaded.value().bytes == first.bytes);
        Check(suite, "Get returns dimensions", loaded.ok() && loaded.value().width == 16 && loaded.value().height == 8);
        Check(suite, "Get returns mime", loaded.ok() && loaded.value().mime == "image/png");
        auto missing = uow.value()->Images().Get(taskId, std::string(64, '0'));
        Check(suite, "missing blob reports ImageUnavailable",
              !missing.ok() && missing.error().code == ErrorCode::ImageUnavailable);
        Check(suite, "missing blob is not a connection error", !missing.ok() && !missing.error().IsConnectionError());
        auto committed = uow.value()->Commit();
        Check(suite, "Commit after Put succeeds", committed.ok());
    }

    // --- DeleteUnreferenced ------------------------------------------------------
    {
        auto uow = factory.Create();
        if (!uow.ok()) { Check(suite, "uow opens for GC", false); return; }
        auto begun = uow.value()->Begin();
        Check(suite, "Begin for GC succeeds", begun.ok());
        auto gc = uow.value()->Images().DeleteUnreferenced(taskId, {first.key});
        Check(suite, "DeleteUnreferenced succeeds", gc.ok());
        auto kept = uow.value()->Images().Get(taskId, first.key);
        Check(suite, "referenced blob survives GC", kept.ok());
        auto dropped = uow.value()->Images().Get(taskId, second.key);
        Check(suite, "unreferenced blob is removed", !dropped.ok() && dropped.error().code == ErrorCode::ImageUnavailable);
        // A duplicate key in the list models two image blocks referencing the same PNG; ImageKeys()
        // de-duplicates, but the repository must not drop it either way.
        auto repeated = uow.value()->Images().DeleteUnreferenced(taskId, {first.key, first.key});
        Check(suite, "duplicate reference keeps blob", repeated.ok());
        auto stillKept = uow.value()->Images().Get(taskId, first.key);
        Check(suite, "blob still present after duplicate-key GC", stillKept.ok());
        auto committed = uow.value()->Commit();
        Check(suite, "Commit after GC succeeds", committed.ok());
    }

    // --- Rejection of malformed images (no write at all) -------------------------
    {
        auto uow = factory.Create();
        if (!uow.ok()) { Check(suite, "uow opens for invalid images", false); return; }
        auto begun = uow.value()->Begin();
        Check(suite, "Begin for validation succeeds", begun.ok());
        BodyImage wrongKey = first;
        wrongKey.key = std::string(64, '0');
        auto bad = uow.value()->Images().Put(taskId, wrongKey);
        Check(suite, "key/bytes mismatch is rejected", !bad.ok() && bad.error().code == ErrorCode::ImageInvalid);
        BodyImage empty = first;
        empty.bytes.clear();
        auto emptyResult = uow.value()->Images().Put(taskId, empty);
        Check(suite, "empty PNG is rejected", !emptyResult.ok());
        auto committed = uow.value()->Commit();
        Check(suite, "Commit after rejections succeeds", committed.ok());
    }

    // --- TaskService writes note + body_doc + blobs in ONE UoW -------------------
    auto clock = std::make_shared<FixedClock>();
    TaskService tasks(factory, clock);
    const BodyDocument document = MakeDocument({first.key, second.key});
    const std::string docJson = document.ToJson();
    CreateTaskCommand create;
    create.title = "body note projection";
    create.note = "ignored by service";
    create.body = BodyEdit{docJson, {first, second}};
    auto created = tasks.Create(create);
    Check(suite, "Create with body succeeds", created.ok(), created.ok() ? "" : created.error().message.c_str());
    if (created.ok()) {
        Check(suite, "note == PlainText(false)", created.value().note == document.PlainText(false));
        Check(suite, "body_doc is the canonical JSON", created.value().bodyDoc == docJson);
        auto uow = factory.Create();
        if (uow.ok()) {
            auto blob = uow.value()->Images().Get(created.value().id, second.key);
            Check(suite, "new blob written in same UoW", blob.ok() && blob.value().bytes == second.bytes);
            auto firstBlob = uow.value()->Images().Get(created.value().id, first.key);
            Check(suite, "both new blobs written in same UoW", firstBlob.ok());
        }
    }

    // --- Edit without body leaves body_doc / blobs untouched ---------------------
    if (created.ok()) {
        EditTaskCommand edit;
        edit.id = created.value().id;
        edit.title = "renamed only";
        edit.note = created.value().note;
        edit.expectedRevision = created.value().revision;
        auto edited = tasks.Edit(edit);
        Check(suite, "Edit without body succeeds", edited.ok());
        Check(suite, "Edit without body preserves body_doc", edited.ok() && edited.value().bodyDoc == docJson);
        auto uow = factory.Create();
        if (uow.ok()) {
            auto blob = uow.value()->Images().Get(edit.id, second.key);
            Check(suite, "Edit without body preserves blobs", blob.ok());
        }
    }

    // --- Edit removes a dropped block's blob -------------------------------------
    if (created.ok()) {
        const BodyDocument shrunk = MakeDocument({first.key});
        EditTaskCommand edit;
        edit.id = created.value().id;
        edit.title = "shrink body";
        edit.note = "ignored";
        edit.expectedRevision = created.value().revision + 1;
        edit.body = BodyEdit{shrunk.ToJson(), {}};
        auto edited = tasks.Edit(edit);
        Check(suite, "Edit with fewer images succeeds", edited.ok(), edited.ok() ? "" : edited.error().message.c_str());
        Check(suite, "note re-derived from shrunk doc", edited.ok() && edited.value().note == shrunk.PlainText(false));
        auto uow = factory.Create();
        if (uow.ok()) {
            auto dropped = uow.value()->Images().Get(edit.id, second.key);
            Check(suite, "removed block's blob is GC'd", !dropped.ok() && dropped.error().code == ErrorCode::ImageUnavailable);
        }
    }

    // --- v>1 refuses to save, never overwrites newer data ------------------------
    if (created.ok()) {
        EditTaskCommand edit;
        edit.id = created.value().id;
        edit.title = "future format";
        edit.note = "x";
        edit.expectedRevision = created.value().revision + 2;
        edit.body = BodyEdit{"{\"v\":99,\"blocks\":[]}", {}};
        auto refused = tasks.Edit(edit);
        Check(suite, "v>1 save is refused", !refused.ok() && refused.error().code == ErrorCode::BodySchemaUnsupported);
        auto uow = factory.Create();
        if (uow.ok()) {
            auto loaded = uow.value()->Tasks().GetByIdForUpdate(edit.id);
            Check(suite, "refused save leaves title untouched", loaded.ok() && loaded.value().title == "shrink body");
        }
    }

    // --- Completing a task keeps blobs; history stores only a text placeholder ---
    if (created.ok()) {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto current = uow.value()->Tasks().GetByIdForUpdate(created.value().id);
            if (current.ok()) {
                auto completed = tasks.SetCompleted(current.value().id, true, current.value().revision);
                Check(suite, "SetCompleted succeeds", completed.ok());
                auto after = factory.Create();
                if (after.ok()) {
                    auto blob = after.value()->Images().Get(current.value().id, first.key);
                    Check(suite, "completion does not GC blobs", blob.ok());
                    auto latest = after.value()->History().GetLatestForTask(current.value().id);
                    Check(suite, "history note contains image placeholder",
                          latest.ok() && latest.value().snapshot.note.find(kImagePlaceholder) != std::string::npos);
                    Check(suite, "history note is not JSON",
                          latest.ok() && latest.value().snapshot.note.find("\"blocks\"") == std::string::npos);
                }
            }
        }
    }

    // --- Deleting a task cascades history AND blobs ------------------------------
    {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto before = uow.value()->Images().Get(taskId, first.key);
            Check(suite, "fixture blob present before delete", before.ok());
        }
    }
    auto deleted = tasks.Delete(taskId);
    Check(suite, "DeleteTask succeeds", deleted.ok());
    {
        auto uow = factory.Create();
        if (uow.ok()) {
            auto after = uow.value()->Images().Get(taskId, first.key);
            Check(suite, "task delete cascades blobs", !after.ok() && after.error().code == ErrorCode::ImageUnavailable);
        }
    }
}
