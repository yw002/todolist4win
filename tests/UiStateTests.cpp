#include "app/AppState.h"

void ReportTest(const char*, const char*, bool, const char*);

using namespace todo;

namespace {
void Check(const char* name, bool value, const char* detail = "") { ReportTest("UiState", name, value, detail); }
}  // namespace

// The UI never derives writability on its own: it trusts AppState, which the
// controller keeps in sync with the connection state. These checks pin the
// contract the controller and pages rely on instead of re-asserting fields the
// test just assigned.
void RunUiStateTests() {
    AppState state;
    Check("starts read-only until the first successful load", state.readOnly);
    Check("starts on the all tasks route", state.route == Route::AllTasks);
    Check("starts disconnected", state.connection == ConnectionState::Initializing);
    Check("starts without an overlay", state.overlay == Overlay::None);
    Check("starts without focus target", state.focusedControl == FocusedControl::None);
    Check("starts not loading", !state.loading);
    Check("starts without data version", state.dataVersion == 0);
    Check("generation starts at one", state.generation == 1);
    Check("no task cursor initially", !state.taskCursor.has_value() && !state.hasTaskNext);
    Check("no history cursor initially", !state.historyCursor.has_value() && !state.hasHistoryNext);
    Check("empty task list initially", state.tasks.empty() && state.history.empty() && state.categories.empty());

    // Preferences mirrored from AppConfig before the window appears.
    Check("close to tray defaults on", state.closeToTray);
    Check("window is not topmost by default", !state.topmost);
    Check("theme follows the system by default", state.themeMode == ThemeMode::System);
    Check("completed tasks stay visible by default", state.completionDisplay == CompletionDisplay::KeepVisible);

    // Preference mirroring is one-way: config -> state, verified by the
    // controller tests; here only the pre-window defaults are pinned.
    Check("empty error message initially", state.errorMessage.empty() && state.toast.empty());
}
