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
#include <fstream>
#include <filesystem>
#include <queue>
#include <condition_variable>
#include <iomanip>
#include <ctime>
#include <random>

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

// global counter for unique process IDs
std::atomic<int> g_pid_counter{1};

// TO-DO: parse txt file
bool load_config(const std::string& path, Config& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string key, val;
    while (file >> key >> val) {
        // Strip surrounding quotes if present (e.g., "rr" -> rr)
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        try {
            if (key == "num-cpu" || key == "num_cpu") {
                int c = std::stoi(val);
                if (c >= 1 && c <= 128) out.num_cpu = c;
            } else if (key == "scheduler") {
                if (val == "fcfs" || val == "rr") out.scheduler = val;
            } else if (key == "quantum-cycles" || key == "quantum_cycles") {
                out.quantum_cycles = static_cast<uint32_t>(std::stoul(val));
            } else if (key == "batch-process-freq" || key == "batch_process_freq") {
                out.batch_process_freq = static_cast<uint32_t>(std::stoul(val));
            } else if (key == "min-ins" || key == "min_ins") {
                out.min_ins = static_cast<uint32_t>(std::stoul(val));
            } else if (key == "max-ins" || key == "max_ins") {
                out.max_ins = static_cast<uint32_t>(std::stoul(val));
            } else if (key == "delays-per-exec" || key == "delay-per-exec" || key == "delay_per_exec") {
                out.delay_per_exec = static_cast<uint32_t>(std::stoul(val));
            }
        } catch (...) {
            // Ignore parse errors and retain default values
        }
    }

    if (out.min_ins > out.max_ins) {
        std::swap(out.min_ins, out.max_ins);
    }
    return true;
}

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

std::string get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "(%m/%d/%Y %I:%M:%S%p)", &tm_buf);
    return std::string(buf);
}

inline int random_int(int low, int high) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(low, high);
    return dist(rng);
}

// generates a randomized operand (either an existing variable or a uint16 immediate)
Operand generate_random_operand(const std::vector<std::string>& var_pool) {
    Operand op;
    if (!var_pool.empty() && random_int(0, 1) == 0) {
        op.is_var = true;
        op.name = var_pool[random_int(0, static_cast<int>(var_pool.size()) - 1)];
    } else {
        op.is_var = false;
        op.value = static_cast<uint16_t>(random_int(0, 100));
    }
    return op;
}

Instruction generate_random_instruction(const std::string& proc_name, std::vector<std::string>& var_pool, int depth = 0) {
    Instruction ins;
    
    // Pick instruction type: 0: PRINT, 1: DECLARE, 2: ADD, 3: SUBTRACT, 4: SLEEP, 5: FOR
    // If nesting depth has reached 3, forbid nested FOR loops
    int max_type = (depth < 3) ? 5 : 4;
    int type_idx = random_int(0, max_type);

    switch (type_idx) {
        case 0: { // PRINT
            ins.type = InsType::PRINT;
            ins.msg = "Hello world from " + proc_name + "!";
            break;
        }
        case 1: { // DECLARE
            ins.type = InsType::DECLARE;
            ins.var = "var_" + std::to_string(var_pool.size() + 1);
            ins.a.is_var = false;
            ins.a.value = static_cast<uint16_t>(random_int(0, 1000));
            var_pool.push_back(ins.var);
            break;
        }
        case 2: { // ADD
            ins.type = InsType::ADD;
            if (var_pool.empty()) {
                ins.var = "var_1";
                var_pool.push_back(ins.var);
            } else {
                ins.var = var_pool[random_int(0, static_cast<int>(var_pool.size()) - 1)];
            }
            ins.a = generate_random_operand(var_pool);
            ins.b = generate_random_operand(var_pool);
            break;
        }
        case 3: { // SUBTRACT
            ins.type = InsType::SUBTRACT;
            if (var_pool.empty()) {
                ins.var = "var_1";
                var_pool.push_back(ins.var);
            } else {
                ins.var = var_pool[random_int(0, static_cast<int>(var_pool.size()) - 1)];
            }
            ins.a = generate_random_operand(var_pool);
            ins.b = generate_random_operand(var_pool);
            break;
        }
        case 4: { // SLEEP
            ins.type = InsType::SLEEP;
            ins.sleep = static_cast<uint8_t>(random_int(1, 5)); // 1 to 5 ticks
            break;
        }
        case 5: { // FOR (nested loop)
            ins.type = InsType::FOR;
            ins.repeats = static_cast<uint32_t>(random_int(2, 4));
            int body_size = random_int(1, 3);
            for (int i = 0; i < body_size; ++i) {
                ins.body.push_back(generate_random_instruction(proc_name, var_pool, depth + 1));
            }
            break;
        }
    }
    return ins;
}

// Recursively counts total code lines represented by instructions (including loop bodies)
size_t count_instruction_lines(const std::vector<Instruction>& ins_list) {
    size_t total = 0;
    for (const auto& ins : ins_list) {
        total++; // count the instruction line itself
        if (ins.type == InsType::FOR) {
            total += count_instruction_lines(ins.body);
        }
    }
    return total;
}

// main function to create fully initialized dummy processes
ProcPtr generate_dummy_process(const std::string& name) {
    auto proc = std::make_shared<Process>();
    proc->id = g_pid_counter.fetch_add(1);
    proc->name = name;
    proc->state = State::READY;
    proc->pc = 0;

    // pick target instruction count between min_ins and max_ins
    uint32_t target_ins = (g_config.max_ins > g_config.min_ins)
                          ? random_int(g_config.min_ins, g_config.max_ins)
                          : g_config.min_ins;
    //safety guard in case target ins is 0 (avoid crashes and division by zero)
    if (target_ins == 0) target_ins = 1;

    std::vector<std::string> var_pool;
    proc->instructions.reserve(target_ins);

    while (proc->instructions.size() < target_ins) {
        proc->instructions.push_back(generate_random_instruction(name, var_pool, 0));
    }

    // set line_count to match total lines of code
    proc->line_count = count_instruction_lines(proc->instructions);

    return proc;
}

// Scheduler
std::atomic<uint64_t> g_cpu_ticks{0};

struct Scheduler {
    // ADD HERE: ready queue (+ mutex/condvar), running/finished lists,
    //       core worker threads, tick thread, quantum handling for "rr"
    void start()  {}   // spin up cores
    void stop()   {}   // join cores
};

Scheduler g_scheduler;

// Commands
void cmd_initialize() {
    // ADD HERE: load config, set initial state, start scheduler, start generator
    if (g_initialized) {
        print_message("\nSystem has already been initialized.");
        return;
    }

    const std::string config_path = "config.txt";
    if (!load_config(config_path, g_config)) {
        print_message("\nError: Failed to open " + config_path + ". Using default configurations.");
    }

    g_scheduler.start();
    g_initialized = true;

    print_message("\nSystem initialized successfully.");
    print_message("Cores: " + std::to_string(g_config.num_cpu) + 
                  " | Scheduler: " + g_config.scheduler + 
                  " | Quantum: " + std::to_string(g_config.quantum_cycles));
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
        else {
            // must initialize before other commands
            if (!g_initialized) {
                print_message("System not initialized. Please run 'initialize' first.");
                continue;
            }

            if (cmd == "screen") {
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
    }
    return 0;
}