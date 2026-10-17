/**
 * @file app.hpp
 * @brief The application: the objects it owns, and the wiring between them.
 *
 * Ownership (issue #59) — stated once, here, instead of implied by file-scope
 * pointers:
 *
 *   board_, audio_, display_, camera_   devkit::X::create(); owned by the App
 *   ui_        MainScreen, built in the LVGL task by the splash's dismissal
 *   detector_  built after the splash; owns its inference task
 *   voice_     built last; owns its feed + detect tasks and the microphone
 *
 * Every one of them outlives app_main()'s return: the LVGL port task and the two
 * pipeline tasks keep running, and these objects are their dependencies. The App
 * itself is a function-local static in app_main — firmware lifetime. Its
 * destructor is correct, but no point in the program's life reaches it: a device
 * reset is not a process exit.
 *
 * Callbacks handed to other objects capture `this`, which is safe for the same
 * reason — the App outlives every one of them. The screen's two callbacks are
 * installed once, while it is being built (issue #52); the pipeline's run on its
 * own tasks.
 */

#pragma once

#include <memory>

#include "splash.hpp"   /* Splash::Reason — the dismissal thunk's signature */

namespace devkit { class Board; class Audio; class Display; class Camera; }
class MainScreen;
class Detector;
class VoicePipeline;

class App final {
public:
    App() = default;
    ~App();

    App(const App &)            = delete;
    App &operator=(const App &) = delete;
    App(App &&)                 = delete;
    App &operator=(App &&)      = delete;

    /** Bring the devkit up, show the splash, and hand over to the tasks. */
    void start();

private:
    /* ---- staged bring-up -------------------------------------------------- */
    bool create_devkit_interfaces();
    void print_banner() const;
    void print_memory_summary(const char *label) const;

    /* ---- the splash's dismissal, then the screen it builds ---------------- */
    static void splash_dismissed(Splash::Reason reason, void *arg);
    void build_main_screen();

    /* ---- the wiring the other objects call -------------------------------- */
    void on_wakeword(const char *cmd);
    void on_command(const char *cmd);
    void on_timeout(const char *cmd);
    void handle_result_dismissed();
    void return_to_command_mode();
    void start_capture_cycle();

    /* ---- owned objects, declared in construction order (destroyed in
     *      reverse: voice_, detector_, ui_, camera_, display_, audio_, board_) */
    std::unique_ptr<devkit::Board>   board_;
    std::unique_ptr<devkit::Audio>   audio_;
    std::unique_ptr<devkit::Display> display_;
    std::unique_ptr<devkit::Camera>  camera_;
    std::unique_ptr<MainScreen>      ui_;
    std::unique_ptr<Detector>        detector_;
    std::unique_ptr<VoicePipeline>   voice_;
};

/**
 * @brief Construct the firmware-lifetime App and start it.
 *
 * Declared here but defined in app.cpp, where the owned types are complete: an
 * App instance needs their destructors, so app_main.cpp cannot instantiate one
 * from forward declarations alone — and it should not have to include LVGL and
 * the devkit headers just to reach the entry point.
 */
void run_app();
