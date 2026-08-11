// kist_data_collector — records every enabled stream (cameras, lowstate,
// dex3 hands, UWB) into one session directory (compressed payloads verbatim
// + CSV indices; see common/session.hpp for the layout). Prints per-second
// per-stream rx/write fps and the loss counters; Ctrl-C / SIGTERM stops,
// drains the queues, and appends the summary to meta.yaml. After a Ctrl-C
// stop the operator labels the episode with one keypress (S = success,
// F = fail; Ctrl-C again = leave unlabeled) -> meta.yaml `result:`.
//
//   ./kist_data_collector [config_path]      (default config/config.yaml)

#include "common/config.hpp"
#include "common/dds_config.hpp"
#include "system/data_collector.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include <termios.h>
#include <unistd.h>

static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_stop_signal{0};

// One keypress, no Enter: S = success, F = fail. Empty string = unlabeled
// (stdin not a terminal, or the operator pressed Ctrl-C again to skip).
static std::string prompt_result() {
    if (!isatty(STDIN_FILENO)) return "";

    // Re-register SIGINT without SA_RESTART so a second Ctrl-C interrupts
    // the blocking read() (glibc's signal() restarts syscalls by default).
    struct sigaction sa{};
    sa.sa_handler = [](int) {};
    sigaction(SIGINT, &sa, nullptr);

    termios saved{};
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return "";
    termios raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    std::printf("label this episode — [S]uccess / [F]ail (Ctrl-C = skip): ");
    std::fflush(stdout);
    std::string result;
    for (;;) {
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;   // EOF or Ctrl-C
        if (c == 's' || c == 'S') { result = "success"; break; }
        if (c == 'f' || c == 'F') { result = "fail";    break; }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    std::printf("%s\n", result.empty() ? "(unlabeled)" : result.c_str());
    return result;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string config_path = (argc >= 2) ? argv[1] : "config/config.yaml";

    kist::Config::instance().load(config_path);
    const auto& root = kist::Config::instance().root();

    auto settings = kist::DataCollector::Settings::from_yaml(root);
    if (!kist::apply_dds_config(root, &settings.dds_uri)) return 1;

    kist::DataCollector collector;
    if (!collector.start(settings)) return 1;

    std::signal(SIGINT,  [](int) { g_stop_signal = SIGINT;  g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop_signal = SIGTERM; g_stop = true; });

    auto window = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(1)) continue;
        window = now;
        collector.print_report();
    }
    collector.stop();

    // Label only on interactive Ctrl-C — a SIGTERM (service manager, kill)
    // must not leave the process waiting on keyboard input.
    if (g_stop_signal == SIGINT)
        collector.write_result(prompt_result());
    return 0;
}
