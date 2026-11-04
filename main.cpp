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
#include <unordered_set>
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

// strict unsigned parse: digits only (rejects "-1", "5x", ""), must fall in [lo, hi]
bool parse_uint(const std::string& s, uint64_t lo, uint64_t hi, uint64_t& out) {
    if (s.empty() || s.size() > 10) return false;
    for (char ch : s) if (ch < '0' || ch > '9') return false;
    uint64_t v = std::stoull(s);
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

bool load_config(const std::string& path, Config& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    constexpr uint64_t U32_MAX = 0xFFFFFFFFull; // 2^32 - 1 (largest value a uint32_t holds)

    std::string key, val;
    while (file >> key >> val) {
        // Strip surrounding quotes if present (e.g., "rr" -> rr)
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        uint64_t n = 0;
        bool known = true, ok = true;

        if (key == "num-cpu" || key == "num_cpu") {
            if ((ok = parse_uint(val, 1, 128, n))) out.num_cpu = static_cast<int>(n);
        } else if (key == "scheduler") {
            if ((ok = (val == "fcfs" || val == "rr"))) out.scheduler = val;
        } else if (key == "quantum-cycles" || key == "quantum_cycles") {
            if ((ok = parse_uint(val, 1, U32_MAX, n))) out.quantum_cycles = static_cast<uint32_t>(n);
        } else if (key == "batch-process-freq" || key == "batch_process_freq") {
            if ((ok = parse_uint(val, 1, U32_MAX, n))) out.batch_process_freq = static_cast<uint32_t>(n);
        } else if (key == "min-ins" || key == "min_ins") {
            if ((ok = parse_uint(val, 1, U32_MAX, n))) out.min_ins = static_cast<uint32_t>(n);
        } else if (key == "max-ins" || key == "max_ins") {
            if ((ok = parse_uint(val, 1, U32_MAX, n))) out.max_ins = static_cast<uint32_t>(n);
        } else if (key == "delays-per-exec" || key == "delay-per-exec" || key == "delay_per_exec") {
            if ((ok = parse_uint(val, 0, U32_MAX, n))) out.delay_per_exec = static_cast<uint32_t>(n);
        } else {
            known = false;
        }

        // invalid values keep the default; tell the user instead of failing silently
        if (known && !ok) {
            std::cout << "Warning: invalid value '" << val << "' for '" << key
                      << "' in " << path << ", keeping default.\n";
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
    size_t line = 0;
};

enum class State { READY, RUNNING, SLEEPING, FINISHED };

struct LogEntry { std::string timestamp; int core; std::string msg; };

struct Frame {
    const std::vector<Instruction>* body = nullptr;
    size_t id = 0;
    uint32_t remaining = 1;
};

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
    std::vector<Frame> stack;
};
using ProcPtr = std::shared_ptr<Process>;

// process generator used by scheduler-start / scheduler-stop
std::atomic<bool> g_generating{false};

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

// assign line nums to instructions
void num_lines (std::vector<Instruction>& ins_list, size_t& next) {
    for (auto& ins : ins_list) {
        ins.line = next++;
        if (ins.type == InsType::FOR) {
            num_lines(ins.body, next);
        }
    }
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

    size_t next_line = 1;
    num_lines(proc->instructions, next_line);
    proc->stack.push_back(Frame{&proc->instructions, 0, 1}); // initialize stack with main instruction list

    return proc;
}

uint16_t operand_value(Process& p, const Operand& op) {
    if (!op.is_var) return op.value;
    return p.vars[op.name]; // undeclared variables auto-declare as 0
}

uint16_t clamp_u16(int64_t v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return static_cast<uint16_t>(v);
}

// MOves the process stack forward to the next instruction, returning true if there is a next instruction, false if the process has finished.
bool advance(Process& proc) {
    while (!proc.stack.empty()) {
        Frame& f = proc.stack.back();
        if (f.id >= f.body->size()) {
            if (--f.remaining > 0) f.id = 0; // next loop iteration
            else proc.stack.pop_back();
            continue;
        }
        const Instruction& ins = (*f.body)[f.id];
        if (ins.type == InsType::FOR) {
            f.id++;
            if (ins.repeats > 0 && !ins.body.empty())
                proc.stack.push_back(Frame{&ins.body, 0, ins.repeats}); // may invalidate f
            continue;
        }
        return true;
    }
    return false;
}

// Executes the next instruction of the process, updating its state and logs. Returns true if an instruction was executed, false if the process has finished.
bool step_process(Process& proc, int core, uint64_t now_tick) {
    if (advance(proc)) {
        Frame& f = proc.stack.back();
        const Instruction& ins = (*f.body)[f.id++];
        switch (ins.type) {
            case InsType::PRINT: {
                std::string msg = ins.msg;
                if (!ins.print_var.empty()) msg += std::to_string(proc.vars[ins.print_var]);
                proc.logs.push_back({get_current_timestamp(), core, msg});
                break;
            }
            case InsType::DECLARE:
                proc.vars[ins.var] = ins.a.value;
                break;
            case InsType::ADD:
                proc.vars[ins.var] = clamp_u16(int64_t(operand_value(proc, ins.a)) + operand_value(proc, ins.b));
                break;
            case InsType::SUBTRACT:
                proc.vars[ins.var] = clamp_u16(int64_t(operand_value(proc, ins.a)) - operand_value(proc, ins.b));
                break;
            case InsType::SLEEP:
                proc.state = State::SLEEPING;
                proc.wake_tick = now_tick + ins.sleep;
                break;
            default: break;
        }
        proc.pc = ins.line;
        return true;
    }
    return false;
}

constexpr int TICK_MS = 10;

struct Core {
    ProcPtr proc;              // process currently holding the core (null between a preempt/sleep/finish and the next tick)
    ProcPtr last;              // process that ran on this core during the most recent tick (for screen -ls / report-util)
    uint32_t delay_left = 0;   // busy-wait ticks before the next instruction
    bool busy = false;         // did this core execute a process during the most recent tick?
};

// Scheduler
std::atomic<uint64_t> g_cpu_ticks{0};

struct Scheduler {
    std::mutex m;// guards everything below
    std::vector<Core> cores;
    std::queue<ProcPtr> ready;
    std::vector<ProcPtr> sleeping;
    std::vector<ProcPtr> finished;
    std::unordered_map<std::string, ProcPtr>  by_name; // newest process per name

    std::atomic<bool> running{false};
    std::thread tick_thread;
    std::vector<std::thread> workers;
    std::condition_variable  cv;
    int next_auto_name = 1;

    std::string next_name() {   // caller holds m
        while (true) {
            std::ostringstream os;
            os << "p" << std::setw(2) << std::setfill('0') << next_auto_name++;
            auto it = by_name.find(os.str());
            if (it == by_name.end() || it->second->state == State::FINISHED) return os.str();
        }
    }

        // Creates a user-named process and queues it. Returns nullptr if an unfinished process already has that name.
    ProcPtr create_process(const std::string& name) {
        std::lock_guard<std::mutex> lock(m);
        auto it = by_name.find(name);
        if (it != by_name.end() && it->second->state != State::FINISHED) return nullptr;
        auto p = generate_dummy_process(name);
        by_name[name] = p;
        ready.push(p);
        return p;
    }

    void wake_sleepers(uint64_t now) {   // caller holds m
        for (size_t i = 0; i < sleeping.size();) {
            if (now >= sleeping[i]->wake_tick) {
                sleeping[i]->state = State::READY;
                ready.push(sleeping[i]);
                sleeping.erase(sleeping.begin() + i);
            } else ++i;
        }
    }

    // One CPU cycle on core ci (FCFS / RR, delay-per-exec, state transitions). Caller holds m.
    void core_cycle(size_t ci, uint64_t now) {
        const bool rr = (g_config.scheduler == "rr");
        Core& c = cores[ci];

        if (!c.proc) {
            if (ready.empty()) {            // nothing to run: core is genuinely idle this tick
                c.busy = false;
                c.last.reset();
                return;
            }
            c.proc = ready.front(); ready.pop();
            c.proc->quantum_used = 0;
            c.delay_left   = 0;
            c.proc->state  = State::RUNNING;
            c.proc->core   = static_cast<int>(ci);
            if (c.proc->start_time.empty()) c.proc->start_time = get_current_timestamp();
        }

        Process& p = *c.proc;
        c.busy = true;                      // this core did work during this tick
        c.last = c.proc;
        p.quantum_used++;

        if (c.delay_left > 0) {
            c.delay_left--;
        } else {
            step_process(p, static_cast<int>(ci), now);
            c.delay_left = g_config.delay_per_exec;
        }

        if (!advance(p)) { // nothing left to run: done
            p.pc       = p.line_count;
            p.state    = State::FINISHED;
            p.end_time = get_current_timestamp();
            p.core     = -1;
            finished.push_back(c.proc);
            c.proc.reset();
        } else if (p.state == State::SLEEPING) {    // SLEEP relinquishes the CPU
            p.core = -1;
            sleeping.push_back(c.proc);
            c.proc.reset();
        } else if (rr && p.quantum_used >= g_config.quantum_cycles) { // preempt
            p.state = State::READY;
            p.core  = -1;
            ready.push(c.proc);
            c.proc.reset();
        }
    }

   //runs exactly one cycle per CPU tick.
    void core_worker(size_t ci) {
        uint64_t last = 0;
        std::unique_lock<std::mutex> lock(m);
        while (true) {
            cv.wait(lock, [&] { return !running || g_cpu_ticks.load() != last; });
            if (!running) break;
            last = g_cpu_ticks.load();
            core_cycle(ci, last);
        }
    }

    void start() {
        if (running.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lock(m);
            cores.assign(g_config.num_cpu, Core{});
        }
        for (size_t ci = 0; ci < static_cast<size_t>(g_config.num_cpu); ++ci)
            workers.emplace_back([this, ci] { core_worker(ci); });

        // tick thread: advances time, wakes sleepers, generates processes
        tick_thread = std::thread([this] {
            uint32_t since_batch = 0;
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
                {
                    std::lock_guard<std::mutex> lock(m);
                    uint64_t now = ++g_cpu_ticks;
                    wake_sleepers(now);
                    if (g_generating && ++since_batch >= g_config.batch_process_freq) {
                        since_batch = 0;
                        auto p = generate_dummy_process(next_name());
                        by_name[p->name] = p;
                        ready.push(p);
                    }
                }
                cv.notify_all(); // let every core run this tick
            }
        });
    }

    void stop() {
        if (!running.exchange(false)) return;
        { std::lock_guard<std::mutex> lock(m); } // make sure no worker misses the wake-up
        cv.notify_all();
        if (tick_thread.joinable()) tick_thread.join();
        for (auto& w : workers) if (w.joinable()) w.join();
        workers.clear();
    }
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

void cmd_scheduler_start() {
    if (g_generating.exchange(true)) {
        print_message("Scheduler is already generating processes.");
        return;
    }
    print_message("Scheduler started: generating a process every " +
                  std::to_string(g_config.batch_process_freq) + " CPU tick(s).");
}

void cmd_scheduler_stop() {
    if (!g_generating.exchange(false)) {
        print_message("Scheduler is not generating processes.");
        return;
    }
    print_message("Scheduler stopped.");
}

std::string build_status_report(); // defined below

void cmd_report_util() {
    std::ofstream out("csopesy-log.txt");
    if (!out) { print_message("Error: could not write csopesy-log.txt"); return; }
    out << build_status_report() << "\n";
    out.close();
    print_message("Report generated at " +
        std::filesystem::absolute("csopesy-log.txt").string() + "!");
}

// screen -ls, shows running and finished processes, their core, PC, and line count
std::string build_status_report() {
    std::lock_guard<std::mutex> lock(g_scheduler.m);

    // A core counts as "used" if it executed a process during the most recent tick.
    // (Looking at Core::proc instead would miss cores whose process was just preempted or put to sleep.)
    struct Row { ProcPtr p; int core; };
    std::vector<Row> listed;
    std::unordered_set<const Process*> seen;
    size_t used = 0;

    for (size_t ci = 0; ci < g_scheduler.cores.size(); ++ci) {
        const Core& c = g_scheduler.cores[ci];
        if (!c.busy) continue;
        ++used;
        if (c.last && c.last->state != State::FINISHED && seen.insert(c.last.get()).second)
            listed.push_back({c.last, static_cast<int>(ci)});
    }
    // sleeping processes are mid-run but off the CPU, so list them too (core shown as "-")
    for (const auto& p : g_scheduler.sleeping)
        if (seen.insert(p.get()).second) listed.push_back({p, -1});

    const size_t total_cores = g_scheduler.cores.size();
    const size_t utilization = total_cores ? (used * 100) / total_cores : 0;

    size_t name_w = 10;
    for (const auto& r : listed)               name_w = std::max(name_w, r.p->name.size());
    for (const auto& p : g_scheduler.finished) name_w = std::max(name_w, p->name.size());
    name_w += 3;

    std::ostringstream os;
    os << "CPU utilization: " << utilization << "%\n"
       << "Cores used: " << used << "\n"
       << "Cores available: " << (total_cores - used) << "\n\n"
       << "--------------------------------------\n"
       << "Running processes:\n";
    for (const auto& r : listed) {
        os << std::left << std::setw(static_cast<int>(name_w)) << r.p->name
           << r.p->start_time << "    Core: " << std::setw(6)
           << (r.core >= 0 ? std::to_string(r.core) : std::string("-"))
           << r.p->pc << " / " << r.p->line_count << "\n";
    }
    os << "\nFinished processes:\n";
    for (const auto& p : g_scheduler.finished) {
        os << std::left << std::setw(static_cast<int>(name_w)) << p->name
           << p->start_time << "    " << std::setw(12) << "Finished"
           << p->line_count << " / " << p->line_count << "\n";
    }
    os << "--------------------------------------";
    return os.str();
}

void cmd_screen_ls() {
    print_message(build_status_report());
}


// process screen attached to a real process (used by screen -r)
// TODO: process-smi tas on-entry process info/logs dito
void render_process_smi(const ProcPtr& proc) {
    std::lock_guard<std::mutex> lock(proc->m);
    std::ostringstream os;
    os << "\nProcess name: " << proc->name << "\n"
       << "ID: " << proc->id << "\n"
       << "Logs:\n";
    for (const auto& log : proc->logs) {
        os << log.timestamp << " Core: " << log.core << "    \"" << log.msg << "\"\n";
    }
    if (proc->state == State::FINISHED) {
        os << "Finished!\n";
    }
    os << "Current instruction line: " << proc->pc << "\n"
       << "Lines of code: " << proc->line_count << "\n";
    std::cout << os.str() << std::flush;
}

void process_screen(const ProcPtr& proc) {
    clear_screen();
    render_process_smi(proc);

    std::string line;
    while (true) {
        { std::lock_guard<std::mutex> lock(console_mutex); std::cout << "\nroot:\\> " << std::flush; }
        if (!std::getline(std::cin, line)) return;

        if (line == "process-smi") {
            render_process_smi(proc);
        }
        else if (line == "exit") {
            clear_screen();
            print_banner();
            print_welcome();
            return;
        }
        else if (!line.empty()) {
            print_message("Invalid command. Use 'process-smi' or 'exit'.");
        }
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
        ProcPtr proc = g_scheduler.create_process(name);
        if (!proc) {
            print_message("Process " + name + " already exists.");
        } else {
            process_screen(proc);
        }
    } else if (flag == "-r" && !name.empty()) {
        ProcPtr proc;
        {
            std::lock_guard<std::mutex> lock(g_scheduler.m);
            auto it = g_scheduler.by_name.find(name);
            if (it != g_scheduler.by_name.end() && it->second->state != State::FINISHED)
                proc = it->second;
        }
        if (!proc) {
            print_message("Process " + name + " not found.");
        } else {
            process_screen(proc);
        }
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
            g_generating = false;
            g_scheduler.stop();
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
            else if (cmd == "clear" || cmd == "cls") {
                clear_screen(); 
                print_banner(); 
                print_welcome();
            }
            else {
                print_message("Unknown command.");
            }
        }
    }
    g_generating = false;
    g_scheduler.stop();
    return 0;
}
