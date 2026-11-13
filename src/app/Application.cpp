#include "app/Application.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "common/Clock.h"
#include "data/MySqlUnitOfWork.h"
#include "data/SqliteUnitOfWork.h"

namespace todo {
namespace {
// The INI is read through SettingsService, never through ConfigStore directly: the app needs the
// effective password and the credential state as much as it needs the plain settings.
AppConfig LoadInitialSettings(SettingsService& settings) {
    auto loaded = settings.Load();
    return loaded.ok() ? loaded.value().config : AppConfig{};
}
}  // namespace

Application::Application(HINSTANCE instance)
    : paths_(AppPaths::Default()),
      vault_(paths_),
      settings_(configStore_, vault_),
      config_(LoadInitialSettings(settings_)),
      window_(instance, configStore_, config_) {
    config_.Sanitize();
}

Application::~Application() {
    // Belt and braces: Run() already joins the recycler before detaching the dispatcher, but a
    // destructor-only path must not leave a worker thread alive either.
    recycler_.Shutdown();
}

std::unique_ptr<DbWorker> Application::CreateWorker(DataSourceKind kind, const DatabaseConfig& database, const std::string& mysqlPassword) {
    const std::function<void()> notify = [this] { dispatcher_.PostCompletion(); };
    if (kind == DataSourceKind::MySql) {
        // The password is overwritten from the settings layer on purpose: DatabaseConfig::password
        // is a runtime-only field, and the value carried by an AppConfig is whatever the config file
        // had - which, since T13, is never the real password any more.
        DatabaseConfig effective = database;
        effective.password = mysqlPassword;
        return std::make_unique<DbWorker>(std::make_unique<MySqlUnitOfWorkFactory>(std::move(effective)), notify);
    }
    return std::make_unique<DbWorker>(std::make_unique<SqliteUnitOfWorkFactory>(paths_.LocalDatabasePath()), notify);
}

std::string Application::MigrateLegacyCredentialAtStartup() {
    const auto migrated = settings_.MigrateLegacyCredential();
    if (migrated.ok()) return {};
    logger_.Error("credential_migration", migrated.error());
    // Warn only when the plaintext really is still on disk. This asks the file, not a flag, so a
    // stale in-memory state can never produce a warning about a secret that is already gone.
    if (!settings_.LegacyPasswordRetained()) return {};
    // One message, one attempt. ConfigStore keeps the plaintext line in place, so the next start
    // tries again; a retry loop here would only repeat the same failing DPAPI call and re-toast.
    return "已保存的数据库密码无法安全迁移，请在设置中重新输入";
}

Result<std::unique_ptr<DbWorker>> Application::CreateCandidateWorker(const DataSourceDraft& draft) {
    // Resolve the effective password first: CredentialUnavailable has to stop the switch before a
    // candidate (and therefore a probe with a blank password) can even exist.
    auto password = settings_.PreviewPassword(draft);
    if (!password.ok()) return Result<std::unique_ptr<DbWorker>>::Failure(password.error());
    return Result<std::unique_ptr<DbWorker>>::Success(CreateWorker(draft.kind, draft.database, password.value()));
}

Result<DataSourceCommitResult> Application::CommitDataSource(const DataSourceDraft& draft) {
    // Credential first, INI second; SettingsService owns that order and the best-effort rollback.
    auto committed = settings_.CommitConnection(draft);
    if (!committed.ok()) return Result<DataSourceCommitResult>::Failure(committed.error());
    config_ = settings_.Config();
    config_.Sanitize();
    DataSourceCommitResult result;
    result.config = config_;
    result.hasSavedCredential = settings_.Credential() == CredentialState::Present;
    return Result<DataSourceCommitResult>::Success(result);
}

int Application::Run() {
    if (!window_.Create()) { MessageBoxW(nullptr, L"无法创建主窗口或 Direct2D 渲染设备。", L"TodoList", MB_OK | MB_ICONERROR); return 1; }
    dispatcher_.Attach(window_.Handle());
    // Must happen before the worker is built: the migration rewrites config.ini and decides which
    // password this process is going to use.
    const std::string migrationWarning = MigrateLegacyCredentialAtStartup();
    DataSourceSwitchPort port;
    port.createCandidate = [this](const DataSourceDraft& draft) { return CreateCandidateWorker(draft); };
    port.commit = [this](const DataSourceDraft& draft) { return CommitDataSource(draft); };
    port.retire = [this](std::unique_ptr<DbWorker> worker) { recycler_.Retire(std::move(worker)); };
    port.savedCredential = [this] { return settings_.Credential() == CredentialState::Present; };
    auto worker = CreateWorker(config_.dataSource, config_.database, settings_.RuntimePassword());
    controller_ = std::make_unique<AppController>(config_, std::move(worker), std::make_shared<SystemClock>(), logger_, [this] { window_.OnStateChanged(); }, std::move(port));
    window_.SetController(*controller_);
    if (!migrationWarning.empty()) controller_->ShowToast(migrationWarning);
    window_.ShowMainWindow();
    controller_->Start();
    MSG message{};
    int result = 0;
    while (true) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status == 0) { result = static_cast<int>(message.wParam); break; }
        if (status == -1) { result = 1; break; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    controller_->OnCompletions();
    // Join the background stops before the dispatcher is detached: everything that could still post
    // a completion message has to be finished while the window handle is still valid.
    recycler_.Shutdown();
    controller_.reset();
    dispatcher_.Detach();
    return result;
}

}  // namespace todo
