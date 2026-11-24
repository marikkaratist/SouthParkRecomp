// Development aids: command-line switches and a crash report. See southpark_dev.h.

#include <chrono>
#include <cmath>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#    include <windows.h>
#    include <io.h>
#endif

#include "southpark_game.h"
#include "southpark_dev.h"

namespace {
    bool autostart = false;
    int dl_dumps_skip = 0;       // graphics tasks to let pass before --dump-dl starts
    std::filesystem::path rom;
    std::atomic<uint8_t*> guest_memory{ nullptr };

    // Defined with the display list dumper below: writes out the graphics task that
    // was being processed, if any. Used by the crash report.
    void dump_current_display_list();
}

// --mash <seconds>: for that long after the first controller read, START and A are pressed
// in turn, twice a second. Gets an unattended run through the title screens and into a level.
static int mash_seconds = 0;

uint16_t southpark::dev::mashed_buttons() {
    if (mash_seconds <= 0) {
        return 0;
    }
    static const auto first = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - first).count();
    if (elapsed > mash_seconds * 1000LL) {
        // Afterwards, for half a minute, one START every four seconds: skips a cutscene that
        // was still to come; in the level it pauses and resumes.
        const auto late = elapsed - mash_seconds * 1000LL;
        return (late < 30000 && late % 4000 < 80) ? 0x1000 : 0;
    }
    const auto phase = elapsed % 500;
    if (phase < 80) {
        return 0x1000;      // START
    }
    if (phase >= 250 && phase < 330) {
        return 0x8000;      // A
    }
    return 0;
}

// --trace-input: one line per player update. --fake-mouse <n>: n counts of sideways mouse movement
// on every controller read, as if the mouse were being moved steadily (for measuring turning).
static bool trace_input_on = false;
static float fake_mouse_counts = 0.0f;

bool southpark::dev::trace_input() {
    return trace_input_on;
}

float southpark::dev::fake_mouse() {
    return fake_mouse_counts;
}

// --update-rate 30|60: overrides the Game Update Rate option.
static int forced_rate = 0;

int southpark::dev::forced_update_rate() {
    return forced_rate;
}

// --draw-distance <scale>: overrides the Draw Distance option.
static float forced_distance = 0.0f;

float southpark::dev::forced_draw_distance() {
    return forced_distance;
}

// --find-player: print the player object's floats that look like a position, once.
// --teleport <x> <z>: put the player there once, 47 seconds after start (in the level with --mash 40). The player object
// holds its position at +0x04 (x), +0x08 (y), +0x0C (z), the previous one at +0xA8..+0xB0 and its
// bounds at +0xDC..+0xF0 (min x, y, z, max x, y, z); all are moved by the same amount.
static bool find_player = false;
static const auto program_start = std::chrono::steady_clock::now();
static bool teleport_wanted = false;
static float teleport_x = 0.0f, teleport_z = 0.0f;

void southpark::dev::player_object_seen(uint8_t* rdram, uint32_t object) {
    static int updates = 0;
    if (object < 0x80000000u || object >= 0x80C00000u) {
        return;
    }
    updates++;
    auto read = [&](uint32_t offset) {
        uint32_t bits = static_cast<uint32_t>(*reinterpret_cast<int32_t*>(rdram + ((object + offset) & 0x3FFFFFF)));
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    if (find_player && updates == 200) {
        std::printf("player object %08X:\n", object);
        for (uint32_t offset = 0; offset < 0x600; offset += 4) {
            const float value = read(offset);
            if (std::fabs(value) > 100.0f && std::fabs(value) < 100000.0f) {
                std::printf("  +0x%03X %.1f\n", offset, value);
            }
        }
    }
    // The level is under way about 44 seconds after start with --mash 40.
    static const auto first = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    static bool teleported = false;
    if (teleported && teleport_wanted && updates % 120 == 0) {
        std::printf("player at %.0f, %.0f%c", read(0x04), read(0x0C), 10);
    }
    if (teleport_wanted && !teleported && now - program_start > std::chrono::seconds(47)) {
        teleported = true;
        auto write = [&](uint32_t offset, float value) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            *reinterpret_cast<int32_t*>(rdram + ((object + offset) & 0x3FFFFFF)) = static_cast<int32_t>(bits);
        };
        const float dx = teleport_x - read(0x04), dz = teleport_z - read(0x0C);
        for (uint32_t offset : { 0x04u, 0xA8u, 0xDCu, 0xE8u }) {
            write(offset, read(offset) + dx);
        }
        for (uint32_t offset : { 0x0Cu, 0xB0u, 0xE4u, 0xF0u }) {
            write(offset, read(offset) + dz);
        }
        std::printf("teleport: player moved by %.0f, %.0f\n", dx, dz);
    }
}

// --capture-at <s>: as pressing F9 (src/game/debug_capture.cpp) s seconds after start.
static int capture_at = 0;

int southpark::dev::capture_at_seconds() {
    return capture_at;
}

// --memory 8: give the game the console's 8 MiB instead of 14 (src/game/draw_distance.cpp).
static int forced_memory = 0;

int southpark::dev::forced_memory_mib() {
    return forced_memory;
}

void southpark::dev::parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--autostart") == 0) {
            autostart = true;
        }
        else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom = std::filesystem::path(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--dump-dl") == 0 && i + 1 < argc) {
            southpark::dev::set_display_list_dumps(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--dump-dl-skip") == 0 && i + 1 < argc) {
            dl_dumps_skip = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--mash") == 0 && i + 1 < argc) {
            mash_seconds = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--trace-camera") == 0) {
            southpark::trace_camera(true);
        }
        else if (std::strcmp(argv[i], "--trace-input") == 0) {
            trace_input_on = true;
        }
        else if (std::strcmp(argv[i], "--fake-mouse") == 0 && i + 1 < argc) {
            fake_mouse_counts = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--update-rate") == 0 && i + 1 < argc) {
            forced_rate = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--capture-at") == 0 && i + 1 < argc) {
            capture_at = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--find-player") == 0) {
            find_player = true;
        }
        else if (std::strcmp(argv[i], "--teleport") == 0 && i + 2 < argc) {
            teleport_wanted = true;
            teleport_x = static_cast<float>(std::atof(argv[++i]));
            teleport_z = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
            forced_memory = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--draw-distance") == 0 && i + 1 < argc) {
            forced_distance = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--no-hints") == 0) {
            southpark::set_interpolation_hints(false);
        }
        else if (std::strcmp(argv[i], "--watchdog") == 0 && i + 1 < argc) {
            southpark::dev::start_watchdog(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            const char* path = argv[++i];
            // Empty the file, then attach both streams in append mode so their
            // output interleaves instead of overwriting each other. (A windowed
            // program has no usable stderr handle to duplicate onto.)
            if (std::FILE* truncated = std::fopen(path, "w")) {
                std::fclose(truncated);
            }
            if (std::freopen(path, "a", stdout) != nullptr && std::freopen(path, "a", stderr) != nullptr) {
                std::setvbuf(stdout, nullptr, _IONBF, 0);
                std::setvbuf(stderr, nullptr, _IONBF, 0);
            }
        }
    }
}

bool southpark::dev::autostart_requested() {
    return autostart;
}

const std::filesystem::path& southpark::dev::rom_path() {
    return rom;
}

void southpark::dev::set_rdram(uint8_t* rdram) {
    guest_memory.store(rdram);
}

#ifdef _WIN32
namespace {
    // Prints an address as an offset into this executable where possible, so it
    // can be looked up in the linker map (tools/crash_symbols.py).
    void print_code_address(const char* label, void* address) {
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module) && module != nullptr) {
            char name[MAX_PATH] = "?";
            GetModuleFileNameA(module, name, sizeof(name));
            const char* base_name = std::strrchr(name, '\\');
            std::fprintf(stderr, "%s %s+0x%llX\n", label, base_name ? base_name + 1 : name,
                         static_cast<unsigned long long>(static_cast<uint8_t*>(address) - reinterpret_cast<uint8_t*>(module)));
        }
        else {
            std::fprintf(stderr, "%s %p\n", label, address);
        }
    }

    std::atomic<bool> reported{ false };

    bool is_fatal(DWORD code) {
        return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
               code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
               code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_IN_PAGE_ERROR;
    }

    // Registered as a vectored handler: an unhandled-exception filter is not
    // reached here (something later in start-up replaces it), and the report
    // has to be written before anything else touches the faulting thread.
    LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        if (!is_fatal(record->ExceptionCode) || reported.exchange(true)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        std::fprintf(stderr, "\n=== CRASH: exception 0x%08lX ===\n", record->ExceptionCode);
        print_code_address("at", record->ExceptionAddress);

        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
            const char* kind = record->ExceptionInformation[0] == 0 ? "read" : record->ExceptionInformation[0] == 1 ? "write" : "execute";
            uint8_t* target = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
            std::fprintf(stderr, "%s of host address %p\n", kind, static_cast<void*>(target));
            uint8_t* rdram = guest_memory.load();
            if (rdram != nullptr && target >= rdram && target < rdram + 0x100000000ULL) {
                // Inverse of recomp.h's MEM_*: guest = host - rdram + 0x80000000 (mod 2^32).
                uint32_t guest = static_cast<uint32_t>(static_cast<uint64_t>(target - rdram) + 0x80000000ULL);
                std::fprintf(stderr, "guest address 0x%08X (byte order within the word may be swapped)\n", guest);
            }
            else {
                std::fprintf(stderr, "not inside guest memory (rdram = %p)\n", static_cast<void*>(rdram));
            }
        }

        // Walk the stack from the faulting context. Recompiled functions keep
        // their guest names, so this is also the game's own call stack.
        CONTEXT context = *info->ContextRecord;
        for (int frame = 0; frame < 48 && context.Rip != 0; frame++) {
            char label[16];
            std::snprintf(label, sizeof(label), "  #%02d", frame);
            print_code_address(label, reinterpret_cast<void*>(context.Rip));
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
            if (function == nullptr) {
                // Leaf function: the return address is at the top of the stack.
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
            else {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
            }
        }
        std::fprintf(stderr, "=== end of crash report ===\n");
        std::fflush(stderr);
        dump_current_display_list();
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace {
    // abort(), std::terminate() and exit() end the process without raising an
    // exception, so they get their own report: the stack of whoever called them.
    void print_current_stack(const char* reason) {
        if (reported.exchange(true)) {
            return;
        }
        std::fprintf(stderr, "\n=== %s ===\n", reason);
        void* frames[48];
        USHORT count = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
        for (USHORT i = 0; i < count; i++) {
            char label[16];
            std::snprintf(label, sizeof(label), "  #%02d", static_cast<int>(i));
            print_code_address(label, frames[i]);
        }
        std::fprintf(stderr, "=== end of report ===\n");
        std::fflush(stderr);
    }

    void on_abort(int) {
        print_current_stack("ABORT");
    }

    void on_terminate() {
        print_current_stack("TERMINATE (uncaught C++ exception)");
        std::abort();
    }

    std::atomic<bool> exiting_normally{ false };

    void on_exit() {
        if (!exiting_normally.load()) {
            print_current_stack("EXIT called");
        }
    }
}

void southpark::dev::install_crash_handler() {
    AddVectoredExceptionHandler(1, crash_filter);
    std::signal(SIGABRT, on_abort);
    std::set_terminate(on_terminate);
    std::atexit(on_exit);
}
#else
void southpark::dev::install_crash_handler() {}
#endif

// --- Display list dump ------------------------------------------------------
// With --dump-dl <count>, the first <count> graphics tasks are written to the
// log before the renderer sees them: enough of F3DEX2 is decoded to follow
// sub-lists and to show where textures and palettes are loaded from.

#include <memory>
#include <vector>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultra64.h"

namespace {
    int dl_dumps_left = 0;
    bool log_video = false;

    uint32_t read_word(uint8_t* rdram, uint32_t address) {
        // Same address arithmetic as recomp.h's MEM_W.
        uint64_t extended = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(address)));
        return *reinterpret_cast<uint32_t*>(rdram + (extended - 0xFFFFFFFF80000000ULL));
    }

    const char* command_name(uint32_t opcode) {
        switch (opcode) {
            // F3DEX2 numbering, which is what this game's microcode uses.
            case 0x01: return "G_VTX";
            case 0x05: return "G_TRI1";
            case 0x06: return "G_TRI2";
            case 0xD7: return "G_TEXTURE";
            case 0xD9: return "G_GEOMETRYMODE";
            case 0xDA: return "G_MTX";
            case 0xDB: return "G_MOVEWORD";
            case 0xDC: return "G_MOVEMEM";
            case 0xDE: return "G_DL";
            case 0xDF: return "G_ENDDL";
            case 0xE7: return "G_RDPPIPESYNC";
            case 0xE9: return "G_RDPFULLSYNC";
            case 0xED: return "G_SETSCISSOR";
            case 0xE4: return "G_TEXRECT";
            case 0xF0: return "G_LOADTLUT";
            case 0xF2: return "G_SETTILESIZE";
            case 0xF3: return "G_LOADBLOCK";
            case 0xF4: return "G_LOADTILE";
            case 0xF5: return "G_SETTILE";
            case 0xF6: return "G_FILLRECT";
            case 0xFD: return "G_SETTIMG";
            case 0xFE: return "G_SETZIMG";
            case 0xFF: return "G_SETCIMG";
            default: return "";
        }
    }

    void dump_display_list(uint8_t* rdram, const OSTask* task) {
        uint32_t segments[16] = {};
        auto resolve = [&](uint32_t address) {
            if (address >= 0x80000000) {
                return address;
            }
            return 0x80000000 | ((segments[(address >> 24) & 0xF] + (address & 0x00FFFFFF)) & 0x1FFFFFFF);
        };

        uint32_t start = static_cast<uint32_t>(task->t.data_ptr);
        std::fprintf(stderr, "=== display list: task type %u, data_ptr 0x%08X, size 0x%X, ucode 0x%08X ===\n",
                     task->t.type, start, task->t.data_size, static_cast<uint32_t>(task->t.ucode));
        std::vector<uint32_t> stack;
        uint32_t pc = resolve(start);
        for (int count = 0; count < 40000; count++) {
            if (pc < 0x80000000 || pc >= 0x81000000) {
                std::fprintf(stderr, "  [list leaves memory at 0x%08X]\n", pc);
                break;
            }
            uint32_t w0 = read_word(rdram, pc);
            uint32_t w1 = read_word(rdram, pc + 4);
            uint32_t opcode = w0 >> 24;
            std::fprintf(stderr, "  %*s%08X: %08X %08X %s", static_cast<int>(stack.size() * 2), "", pc, w0, w1, command_name(opcode));
            if (opcode == 0xFD || opcode == 0xFF || opcode == 0xFE || opcode == 0x01 || opcode == 0xDA || opcode == 0xDC || opcode == 0xDE) {
                std::fprintf(stderr, " -> 0x%08X", resolve(w1));
            }
            std::fprintf(stderr, "\n");
            if (opcode == 0xDA) {
                // The matrix itself: 16 integer halves, then 16 fraction halves.
                uint32_t matrix = resolve(w1);
                for (int row = 0; row < 4; row++) {
                    std::fprintf(stderr, "  %*s    [", static_cast<int>(stack.size() * 2), "");
                    for (int column = 0; column < 4; column++) {
                        int index = row * 4 + column;
                        uint32_t high = read_word(rdram, matrix + (index / 2) * 4);
                        uint32_t low = read_word(rdram, matrix + 0x20 + (index / 2) * 4);
                        int shift = (index & 1) ? 0 : 16;
                        int32_t fixed = static_cast<int32_t>((((high >> shift) & 0xFFFF) << 16) | ((low >> shift) & 0xFFFF));
                        std::fprintf(stderr, " %11.4f", fixed / 65536.0);
                    }
                    std::fprintf(stderr, " ]\n");
                }
            }
            pc += 8;
            if (opcode == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) {
                segments[(w0 & 0xFFFF) / 4 & 0xF] = w1;
            }
            else if (opcode == 0xDE) {
                if (((w0 >> 16) & 0xFF) == 0) {
                    stack.push_back(pc);
                }
                pc = resolve(w1);
            }
            else if (opcode == 0xDF) {
                if (stack.empty()) {
                    break;
                }
                pc = stack.back();
                stack.pop_back();
            }
        }
        std::fprintf(stderr, "=== end of display list ===\n");
        std::fflush(stderr);
    }

    // The task the renderer is working on right now, for the crash report.
    OSTask current_task{};
    uint8_t* current_task_rdram = nullptr;
    std::atomic<bool> in_send_dl{ false };

    void dump_current_display_list() {
        if (in_send_dl.load() && current_task_rdram != nullptr) {
            dump_display_list(current_task_rdram, &current_task);
        }
    }

    // Forwards everything to the real renderer; only looks at display lists.
    class DumpingRenderer final : public ultramodern::renderer::RendererContext {
    public:
        DumpingRenderer(uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> inner)
            : rdram(rdram), inner(std::move(inner)) {}

        bool valid() override { return inner->valid(); }
        ultramodern::renderer::SetupResult get_setup_result() const override { return inner->get_setup_result(); }
        ultramodern::renderer::GraphicsApi get_chosen_api() const override { return inner->get_chosen_api(); }
        bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                           const ultramodern::renderer::GraphicsConfig& new_config) override {
            return inner->update_config(old_config, new_config);
        }
        void enable_instant_present() override { inner->enable_instant_present(); }
        void send_dl(const OSTask* original_task) override {
            // The hints change where the task starts, so work on a copy.
            OSTask task_copy = *original_task;
            OSTask* task = &task_copy;
            southpark::add_interpolation_hints(rdram, task);
            southpark::dev::note_frame();
            if (dl_dumps_skip > 0) {
                dl_dumps_skip--;
            }
            else if (dl_dumps_left > 0) {
                dl_dumps_left--;
                dump_display_list(rdram, task);
            }
            current_task = *task;
            current_task_rdram = rdram;
            in_send_dl.store(true);
            inner->send_dl(task);
            in_send_dl.store(false);
        }
        void send_dummy_workload(uint32_t fb_address) override { inner->send_dummy_workload(fb_address); }
        void update_screen() override {
            // With --dump-dl, log the video registers whenever they change (at most 40 times).
            static ultramodern::renderer::ViRegs last{};
            static int reports = 0;
            const ultramodern::renderer::ViRegs* regs = ultramodern::renderer::get_vi_regs();
            if (log_video && reports < 40 && std::memcmp(&last, regs, sizeof(last)) != 0) {
                last = *regs;
                reports++;
                std::fprintf(stderr, "VI: status %08X origin %08X width %u h_start %08X v_start %08X x_scale %08X y_scale %08X\n",
                             regs->VI_STATUS_REG, regs->VI_ORIGIN_REG, regs->VI_WIDTH_REG, regs->VI_H_START_REG,
                             regs->VI_V_START_REG, regs->VI_X_SCALE_REG, regs->VI_Y_SCALE_REG);
            }
            inner->update_screen();
        }
        void shutdown() override { inner->shutdown(); }
        uint32_t get_display_framerate() const override { return inner->get_display_framerate(); }
        float get_resolution_scale() const override { return inner->get_resolution_scale(); }

    private:
        uint8_t* rdram;
        std::unique_ptr<ultramodern::renderer::RendererContext> inner;
    };
}

void southpark::dev::set_display_list_dumps(int count) {
    dl_dumps_left = count;
    log_video = count > 0;
}

std::unique_ptr<ultramodern::renderer::RendererContext> southpark::dev::wrap_renderer(
    uint8_t* rdram, std::unique_ptr<ultramodern::renderer::RendererContext> renderer) {
    return std::make_unique<DumpingRenderer>(rdram, std::move(renderer));
}

// --- Hang watchdog -----------------------------------------------------------
// With --watchdog <seconds>, a background thread watches the stream of graphics
// tasks. If none arrives for that long after the game has started drawing, the
// call stacks of all threads are written to stderr (once).

#ifdef _WIN32
#include <tlhelp32.h>

#include <chrono>
#include <cmath>
#include <thread>

namespace {
    std::atomic<uint64_t> frames_sent{ 0 };
    int watchdog_seconds = 0;

    void print_thread_stack(DWORD thread_id) {
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
        if (thread == nullptr) {
            return;
        }
        PWSTR description = nullptr;
        char name[128] = "";
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, name, sizeof(name), nullptr, nullptr);
            LocalFree(description);
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                std::fprintf(stderr, "--- thread %lu %s\n", thread_id, name);
                for (int frame = 0; frame < 24 && context.Rip != 0; frame++) {
                    char label[16];
                    std::snprintf(label, sizeof(label), "  #%02d", frame);
                    print_code_address(label, reinterpret_cast<void*>(context.Rip));
                    DWORD64 image_base = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
                    if (function == nullptr) {
                        if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), 8)) {
                            break;
                        }
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                        context.Rsp += 8;
                    }
                    else {
                        void* handler_data = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher, nullptr);
                    }
                }
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }

    void print_all_stacks() {
        std::fprintf(stderr, "\n=== HANG: no graphics task for %d s; stacks of all threads ===\n", watchdog_seconds);
        DWORD self = GetCurrentThreadId();
        DWORD process = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
                        print_thread_stack(entry.th32ThreadID);
                    }
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        std::fprintf(stderr, "=== end of hang report ===\n");
        std::fflush(stderr);
        // If the renderer is stuck inside a display list, show which one.
        dump_current_display_list();
    }

    void watchdog_loop() {
        uint64_t last = 0;
        auto last_change = std::chrono::steady_clock::now();
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            uint64_t now_count = frames_sent.load();
            auto now = std::chrono::steady_clock::now();
            if (now_count != last) {
                last = now_count;
                last_change = now;
            }
            else if (now_count > 0 && now - last_change > std::chrono::seconds(watchdog_seconds)) {
                print_all_stacks();
                return;
            }
        }
    }
}

void southpark::dev::start_watchdog(int seconds) {
    watchdog_seconds = seconds;
    std::thread(watchdog_loop).detach();
}

void southpark::dev::note_frame() {
    frames_sent.fetch_add(1);
}
#else
void southpark::dev::start_watchdog(int) {}
void southpark::dev::note_frame() {}
#endif
