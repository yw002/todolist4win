#include "app/Application.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "common/Clock.h"
#include "data/SqliteUnitOfWork.h"

namespace todo {
namespace {
AppConfig LoadInitialSettings(ConfigStore& store) {
    auto loaded = store.Load();
    return loaded.ok() ? loaded.value() : AppConfig{};
}
}  // namespace

Application::Application(HINSTANCE instance)
    : paths_(AppPaths::Default()),
      config_(LoadInitialSettings(configStore_)),
      window_(instance, configStore_, config_) {
    config_.Sanitize();
}

Application::~Application() = default;

std::unique_ptr<DbWorker> Application::CreateWorker() {
    const std::function<void()> notify = [this] { dispatcher_.PostCompletion(); };
    return std::make_unique<DbWorker>(std::make_unique<SqliteUnitOfWorkFactory>(paths_.LocalDatabasePath()), notify);
}

int Application::Run() {
    if (!window_.Create()) { MessageBoxW(nullptr, L"无法创建主窗口或 Direct2D 渲染设备。", L"TodoList", MB_OK | MB_ICONERROR); return 1; }
    dispatcher_.Attach(window_.Handle());
    auto worker = CreateWorker();
    controller_ = std::make_unique<AppController>(config_, std::move(worker), std::make_shared<SystemClock>(), logger_, [this] { window_.OnStateChanged(); });
    window_.SetController(*controller_);
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
    controller_.reset();
    dispatcher_.Detach();
    return result;
}

}  // namespace todo
