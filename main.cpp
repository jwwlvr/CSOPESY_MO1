// to compile: g++ -std=c++17 -O2 -o main main.cpp -lpthread

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <charconv>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>

// CLI text colors
#define COLOR_GREEN  "\033[0;32m"
#define COLOR_YELLOW "\033[0;33m"
#define COLOR_CYAN   "\033[0;36m"
#define RESET_COLOR  "\033[0m"

#ifdef _WIN32
#include <windows.h>
void enable_ansi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#endif

std::mutex console_mutex; // sync console output

// will use config.txt to set these values
struct Config {
    int      num_cpu            = 4;       // [1, 128]
    std::string scheduler       = "rr";    // "fcfs" or "rr"
    uint32_t quantum_cycles     = 5;       // [1, 2^32]
    uint32_t batch_process_freq = 1;       // [1, 2^32]
    uint32_t min_ins            = 1000;    // [1, 2^32]
    uint32_t max_ins            = 2000;    // [1, 2^32]
    uint32_t delay_per_exec     = 0;       // [0, 2^32]
};

Config g_config;
bool   g_initialized = false;

// TO-DO: parse txt file
bool load_config(const std::string& path, Config& out);

// Process
enum class InsType { PRINT, DECLARE, ADD, SUBTRACT, SLEEP, FOR };

struct Operand {
    bool is_var = false;
    std::string name;
    uint16_t value = 0;
};

struct Instruction {
    InsType type;
    std::string var;
    Operand a, b;
    std::string msg;
    std::string print_var;
    uint8_t sleep = 0;
    std::vector<Instruction> body;
    uint32_t repeats = 0;
};

enum class State { READY, RUNNING, SLEEPING, FINISHED };

struct LogEntry { std::string timestamp; int core; std::string msg; };

struct Process {
    int id = 0;
    std::string name;
    std::vector<Instruction> instructions;
    size_t pc = 0; 
    size_t line_count = 0;
    std::unordered_map<std::string, uint16_t> vars;
    std::vector<LogEntry> logs;
    State state = State::READY;
    int core = -1;
    std::string start_time, end_time;
    uint64_t wake_tick = 0; 
    uint32_t quantum_used = 0;
    std::mutex  m;
};
using ProcPtr = std::shared_ptr<Process>;

// Scheduler
std::atomic<uint64_t> g_cpu_ticks{0};

struct Scheduler {
    // ADD HERE: ready queue (+ mutex/condvar), running/finished lists,
    //       core worker threads, tick thread, quantum handling for "rr"
    void start()  {}   // spin up cores
    void stop()   {}   // join cores
};

// process generator used by scheduler-start / scheduler-stop
std::atomic<bool> g_generating{false};
std::thread       g_generator_thread;

// Helpers
void print_message(const std::string& msg) {
    std::lock_guard<std::mutex> lock(console_mutex);
    std::cout << msg << "\n" << std::flush;
}

void print_banner() {
    std::cout << COLOR_CYAN <<
R"(+------------------------------------------------------------------+
| __        _______ _     ____ ___  __  __ _____   _____ ___     |
| \ \      / / ____| |   / ___/ _ \|  \/  | ____| |_   _/ _ \    |
|  \ \ /\ / /|  _| | |  | |  | | | | |\/| |  _|     | || | | |   |
|   \ V  V / | |___| |__| |__| |_| | |  | | |___    | || |_| |   |
|    \_/\_/  |_____|_____\____\___/|_|  |_|_____|   |_| \___/    |
|                                                                |
|   ____ ____   ___  ____  _____ ______   __  __  __  ___   _    |
|  / ___/ ___| / _ \|  _ \| ____/ ___\ \ / / |  \/  |/ _ \ / |   |
| | |   \___ \| | | | |_) |  _| \___ \\ V /  | |\/| | | | || |   |
| | |___ ___) | |_| |  __/| |___ ___) || |   | |  | | |_| || |   |
|  \____|____/ \___/|_|   |_____|____/ |_|   |_|  |_|\___/ |_|   |
+------------------------------------------------------------------+
)" << RESET_COLOR << std::flush;
}

void print_welcome() {
    std::cout << COLOR_YELLOW << "Welcome to CSOPESY Emulator!\n\n"
              << "Developers:\n" << COLOR_GREEN
              << "Austria, Ma. Alexandria\n"
              << "De Leon, Sofia Ysabela\n"
              << "Guererro, Laura Mae\n"
              << "Patricio, Anne Beatriz\n\n"
              << COLOR_YELLOW << "Last updated: 2026-10-29\n"
              << RESET_COLOR << "----------------------------------------\n";
}

void clear_screen() { std::cout << "\033[2J\033[H" << std::flush; }

// Commands
void cmd_initialize() {
    // ADD HERE: load config, set initial state, start scheduler, start generator
    print_message("[stub] initialize");
}

void cmd_scheduler_start() { print_message("scheduler-start"); } 
void cmd_scheduler_stop()  { print_message("scheduler-stop");  }
void cmd_report_util()     { print_message("report-util");     } 
void cmd_screen_ls()       { print_message("screen -ls");      } 


void process_screen(const std::string& name) {
    clear_screen();
    // ADD HERE: print process info + logs once on entry
    std::string line;
    while (true) {
        { std::lock_guard<std::mutex> lock(console_mutex); std::cout << "root:\\> " << std::flush; }
        if (!std::getline(std::cin, line)) return;
        if (line == "process-smi")      print_message("[stub] process-smi for " + name); // ADD HERE: name, ID, logs, current/total lines, "Finished!"
        else if (line == "exit")        { clear_screen(); print_welcome(); return; }
        else if (!line.empty())         print_message("Invalid command. Use 'process-smi' or 'exit'.");
    }
}

void cmd_screen(std::string_view args) {
    // screen -s <name> | screen -r <name> | screen -ls
    std::istringstream iss{std::string(args)};
    std::string flag, name;
    iss >> flag >> name;

    if (flag == "-ls") {
        cmd_screen_ls();
    } else if (flag == "-s" && !name.empty()) {
        // ADD HERE: create process ( should reject duplicate name), then enter its screen
        process_screen(name);
    } else if (flag == "-r" && !name.empty()) {
        // ADD HERE: look up running process; if missing/finished
        print_message("Process " + name + " not found.");
        // else process_screen(name);
    } else {
        print_message("Usage: screen -s <name> | screen -r <name> | screen -ls");
    }
}


int main() {
#ifdef _WIN32
    enable_ansi();
#endif
    clear_screen();
    print_banner();
    print_welcome();

    bool running = true;
    std::string line;
    line.reserve(128);

    while (running) {
        {
            std::lock_guard<std::mutex> lock(console_mutex);
            std::cout << "\nroot:\\> " << std::flush;
        }
        if (!std::getline(std::cin, line)) break;

        std::string_view sv(line);
        size_t start = sv.find_first_not_of(" \t");
        if (start == std::string_view::npos) continue;
        sv.remove_prefix(start);

        size_t cmd_end = sv.find_first_of(" \t");
        std::string_view cmd = sv.substr(0, cmd_end);

        std::string_view args;
        if (cmd_end != std::string_view::npos) {
            size_t a = sv.find_first_not_of(" \t", cmd_end);
            if (a != std::string_view::npos) args = sv.substr(a);
        }

        if (cmd == "exit") {
            // ADD HERE: stop scjeduler & generator
            print_message("Terminating console...");
            running = false;
        }
        else if (cmd == "initialize") {
            cmd_initialize();
        }
        else if (cmd == "screen") {
            cmd_screen(args);
        }
        else if (cmd == "scheduler-start") {
            cmd_scheduler_start();
        }
        else if (cmd == "scheduler-stop") {
            cmd_scheduler_stop();
        }
        else if (cmd == "report-util") {
            cmd_report_util();
        }
        else {
            print_message("Unknown command.");
        }
    }
    return 0;
}