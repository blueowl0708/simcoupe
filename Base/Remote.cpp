// Part of SimCoupe - A SAM Coupe emulator
//
// Remote.cpp: ZRCP-style remote control over a local TCP socket
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

// Notes:
//  A text protocol modelled on ZEsarUX's ZRCP (same prompt and command names
//  where they make sense), so the same scripts can drive either emulator.
//  Commands run on the emulation thread between chunks, so no locking is
//  needed. Commands that need emulated time (run-frames, cpu-step, run,
//  send-keys-*) hold back the reply and further input until they finish.
//  New input stops a waiting run or run-frames, as in ZEsarUX: the socket is
//  read between chunks (once a frame), so this costs nothing per instruction.

#include "SimCoupe.h"
#include "Remote.h"

#include "CPU.h"
#include "Debug.h"
#include "Disassem.h"
#include "Expr.h"
#include "Frame.h"
#include "GUI.h"
#include "Keyboard.h"
#include "Memory.h"
#include "Options.h"
#include "SAMIO.h"
#include "SavePNG.h"
#include "Symbol.h"

#include <cerrno>
#include <limits>
#include <set>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool would_block() { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void set_nonblocking(socket_t s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
static void close_socket(socket_t s) { close(s); }
static bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static void set_nonblocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

constexpr auto PROMPT = "command> ";
constexpr auto STEP_PROMPT = "command@cpu-step> ";
constexpr auto MAX_BREAKPOINTS = 100;
constexpr size_t MAX_LOG_LINES = 100000;
constexpr auto MAX_PORT_BREAKPOINTS = 100;
constexpr size_t MAX_IO_LOG = 100000;
constexpr size_t MAX_IO_FILTERS = 64;
constexpr int MS_PER_FRAME = 20;

namespace Remote
{
bool trace_instructions;
bool io_hooks;

namespace
{
socket_t listen_socket = INVALID_SOCKET;
socket_t client_socket = INVALID_SOCKET;
int client_id;
std::string in_buf, out_buf;
bool close_after_flush;

// Emulation held by the remote (ZRCP's cpu-step mode)
bool stopped;

// A command waiting for emulated time to pass before it replies
enum class Wait { None, Frames, Instructions, Break, Typing };
struct Pending
{
    Wait type = Wait::None;
    int count = 0;          // frames or instructions left (0 = no limit for Break)
    int total = 0;          // the count asked for
    int owner = 0;          // client connection that should get the reply
    size_t queued = 0;      // input already received when it started, which doesn't interrupt it
};
Pending pending;

// Keys
std::array<uint8_t, 9> held_keys{};     // send-keys-event
std::array<uint8_t, 9> typed_keys{};    // send-keys-string/ascii, current character
std::deque<int> type_queue;
int type_frames = 1;
int type_frames_left;
bool type_key_down;

// Breakpoints (ZRCP numbering, 1 to MAX_BREAKPOINTS)
struct RemoteBreakpoint
{
    std::string condition;
    Expr expr;
    std::string action;
    bool enabled = false;
    bool last_value = false;
};
std::array<RemoteBreakpoint, MAX_BREAKPOINTS> breakpoints;
bool breakpoints_enabled;

enum { MEMBP_NONE, MEMBP_READ, MEMBP_WRITE, MEMBP_READWRITE };
struct MemBreakpoint
{
    int from, to, type;
    int page;               // -1: from/to are CPU addresses; else a SimCoupe page, from/to offsets in it
};
std::vector<MemBreakpoint> mem_breakpoints;
uint16_t last_instr_pc;     // start of the instruction that ran last, for memory breakpoint stops

// Port breakpoints: they match when (port & mask) == (bp.port & mask) and the direction fits
enum { PORTBP_NONE, PORTBP_IN, PORTBP_OUT, PORTBP_INOUT };
struct PortBreakpoint
{
    int port, mask, type;
};
std::vector<PortBreakpoint> port_breakpoints;

// The I/O log: records matching I/O without stopping, oldest entries dropped when full
struct IoFilter
{
    int port, mask;
};
struct IoEntry
{
    uint64_t frame;
    uint32_t cycle;
    uint16_t pc, port;
    uint8_t value;
    bool is_out;
};
bool io_log_enabled;
std::vector<IoFilter> io_filters;       // empty = every port
bool io_filter_in = true, io_filter_out = true;
std::deque<IoEntry> io_log;
uint64_t io_dropped;
uint64_t frame_count;                   // frames run since start-up

// Execution history: a ring of the state before each instruction, filled by OnInstruction while
// enabled. A repeat of the same instruction (LDIR, HALT, JR $) replaces the entry and counts.
constexpr size_t HISTORY_SLOTS = 10000;
constexpr int DEFAULT_HISTORY_LINES = 20;
struct HistoryEntry
{
    uint16_t pc, sp, af, bc, de, hl, ix, iy, af2, bc2, de2, hl2;
    uint8_t i, r, im, iffs;     // iffs: bit 0 = IFF1, bit 1 = IFF2
    uint8_t lmpr, hmpr, vmpr;
    int page;                   // physical page the instruction was fetched from
    std::array<uint8_t, 4> bytes;
    uint32_t repeats;
};
bool history_enabled;
std::vector<HistoryEntry> history;      // ring of HISTORY_SLOTS once first used
size_t history_next;                    // slot the next entry goes in
size_t history_count;
uint64_t history_dropped;
HistoryEntry history_pending;           // the instruction about to run, not yet in the ring
bool history_armed;                     // history_pending is valid

std::deque<std::string> log_lines;

bool coverage_enabled;
std::vector<uint8_t> coverage;      // one bit per byte of pMemory

int memory_zone = -1;               // -1 = CPU view, otherwise a SimCoupe page number

////////////////////////////////////////////////////////////////////////////////

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s)
{
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    for (std::string token; ss >> token; )
        tokens.push_back(token);
    return tokens;
}

// ZRCP number syntax: decimal by default; 0x, $, # or & prefix, or H suffix, for hex
std::optional<int> parse_number(std::string s)
{
    s = trim(s);
    if (s.empty())
        return std::nullopt;

    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        s = s.substr(2);
        base = 16;
    }
    else if (s[0] == '$' || s[0] == '#' || s[0] == '&')
    {
        s = s.substr(1);
        base = 16;
    }
    else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H'))
    {
        s.pop_back();
        base = 16;
    }

    bool negative = !s.empty() && s[0] == '-' && base == 10;
    if (negative)
        s = s.substr(1);

    if (s.empty())
        return std::nullopt;

    // Digits only (strtoll would also accept spaces and signs here)
    if (!std::isalnum(static_cast<unsigned char>(s[0])))
        return std::nullopt;

    char* end{};
    errno = 0;
    auto value = std::strtoll(s.c_str(), &end, base);
    if (*end || errno == ERANGE)
        return std::nullopt;

    if (negative)
        value = -value;

    // Reject anything that doesn't fit an int, rather than wrapping
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return std::nullopt;

    return static_cast<int>(value);
}

// Labels from load-symbols are 16-bit CPU-view addresses, so they only describe addresses
// seen through the CPU (the page paged in there is not considered). Nothing is added to
// any output until symbols are loaded.
constexpr int MAX_LABEL_OFFSET = 0x7f;      // label+offset is shown up to this far past a label
constexpr int DISASSEMBLE_LABEL_LEN = 64;

std::string sym_name(uint16_t addr)
{
    return Symbol::NameRemote(addr, MAX_LABEL_OFFSET);
}

// " (label+0x4)" for appending after an address in a message, or nothing
std::string sym_suffix(uint16_t addr)
{
    auto name = sym_name(addr);
    return name.empty() ? "" : " (" + name + ")";
}

std::string flags_string(uint8_t f)
{
    static const char flag_names[] = "SZ5H3PNC";
    std::string s;
    for (int i = 0; i < 8; ++i)
        s += (f & (0x80 >> i)) ? flag_names[i] : ' ';
    return s;
}

// With symbols loaded: PC as label+offset, and any other 16-bit register that is exactly
// at a label
std::string registers_symbols()
{
    if (!Symbol::HasRemote())
        return "";

    std::string s;
    auto add = [&](const char* name, uint16_t value, int max_offset)
    {
        auto label = Symbol::NameRemote(value, max_offset);
        if (!label.empty())
            s += fmt::format(" {}:{}", name, label);
    };

    add("PC", cpu.get_pc(), MAX_LABEL_OFFSET);
    add("SP", cpu.get_sp(), 0);
    add("BC", cpu.get_bc(), 0);
    add("DE", cpu.get_de(), 0);
    add("HL", cpu.get_hl(), 0);
    add("IX", cpu.get_ix(), 0);
    add("IY", cpu.get_iy(), 0);
    return s.empty() ? "" : "  SYMBOLS:" + s;
}

std::string registers_string()
{
    auto& io = IO::State();
    return fmt::format(
        "PC={:04x} SP={:04x} AF={:04x} BC={:04x} HL={:04x} DE={:04x} IX={:04x} IY={:04x} "
        "AF'={:04x} BC'={:04x} HL'={:04x} DE'={:04x} I={:02x} R={:02x}  F={} F'={} MEMPTR={:04x} IM{} IFF{}{} "
        "VPS: 0 LMPR={:02x} HMPR={:02x} VMPR={:02x}",
        cpu.get_pc(), cpu.get_sp(), cpu.get_af(), cpu.get_bc(), cpu.get_hl(), cpu.get_de(),
        cpu.get_ix(), cpu.get_iy(), cpu.get_alt_af(), cpu.get_alt_bc(), cpu.get_alt_hl(), cpu.get_alt_de(),
        cpu.get_i(), cpu.get_r(), flags_string(cpu.get_f()), flags_string(cpu.get_alt_af() & 0xff),
        cpu.get_wz(), cpu.get_int_mode(), cpu.get_iff1() ? '1' : '-', cpu.get_iff2() ? '2' : '-',
        io.lmpr, io.hmpr, io.vmpr) + registers_symbols();
}

////////////////////////////////////////////////////////////////////////////////
// Memory zones: -1 is the CPU's view, others are SimCoupe page numbers, with
// addresses running on through the following pages of the same kind.

bool zone_valid(int zone)
{
    if (zone == -1 || zone == ROM0 || zone == ROM1)
        return true;
    if (zone >= INTMEM && zone < INTMEM + GetOption(mainmem) / 16)
        return true;
    return zone >= EXTMEM && zone < EXTMEM + GetOption(externalmem) * NUM_EXTERNAL_PAGES_1MB;
}

int zone_size(int zone)
{
    if (zone == -1)
        return 0x10000;
    if (zone == ROM0 || zone == ROM1)
        return (ROM1 - zone + 1) * MEM_PAGE_SIZE;
    if (zone < EXTMEM)
        return (INTMEM + GetOption(mainmem) / 16 - zone) * MEM_PAGE_SIZE;
    return (EXTMEM + GetOption(externalmem) * NUM_EXTERNAL_PAGES_1MB - zone) * MEM_PAGE_SIZE;
}

std::string zone_name(int zone)
{
    if (zone == -1)
        return "Mapped memory";
    return Memory::PageDesc(zone);
}

uint8_t* zone_ptr(int addr)
{
    if (memory_zone == -1)
        return AddrReadPtr(static_cast<uint16_t>(addr));

    auto page = memory_zone + addr / MEM_PAGE_SIZE;
    return PageReadPtr(page) + (addr % MEM_PAGE_SIZE);
}

// The shared bounds check for every command that reads, writes or saves a block of memory:
// the block must lie inside the current zone (the 64K CPU view for zone -1) and be no longer
// than MAX_LENGTH, so a client can never make us allocate or loop without limit.
// Use this (or get_number_range for plain counts) for any number that comes from the client.
constexpr int MAX_LENGTH = 0x1000000;

bool zone_range_ok(int addr, int len, std::string& error)
{
    auto size = static_cast<int64_t>(zone_size(memory_zone));
    if (addr < 0 || len < 0 || len > MAX_LENGTH || static_cast<int64_t>(addr) + len > size)
    {
        error += "Error. Address or length out of range for this memory zone\n";
        return false;
    }
    return true;
}

uint8_t zone_read(int addr)
{
    return *zone_ptr(addr);
}

void zone_write(int addr, uint8_t value)
{
    if (memory_zone == -1)
    {
        auto addr16 = static_cast<uint16_t>(addr);
        check_video_write(addr16);
        *AddrWritePtr(addr16) = value;
    }
    else
    {
        *zone_ptr(addr) = value;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Keys

constexpr std::array<const char*, SK_MAX> sam_key_names
{
    "shift", "z", "x", "c", "v", "f1", "f2", "f3",
    "a", "s", "d", "f", "g", "f4", "f5", "f6",
    "q", "w", "e", "r", "t", "f7", "f8", "f9",
    "1", "2", "3", "4", "5", "escape", "tab", "caps",
    "0", "9", "8", "7", "6", "minus", "plus", "delete",
    "p", "o", "i", "u", "y", "equals", "quotes", "f0",
    "return", "l", "k", "j", "h", "semicolon", "colon", "edit",
    "space", "symbol", "m", "n", "b", "comma", "period", "inv",
    "cntrl", "up", "down", "left", "right",
};

void set_key(std::array<uint8_t, 9>& keys, int sam_key, bool pressed)
{
    if (sam_key < SK_MIN || sam_key >= SK_MAX)
        return;

    auto mask = static_cast<uint8_t>(1 << (sam_key & 7));
    if (pressed)
        keys[sam_key >> 3] |= mask;
    else
        keys[sam_key >> 3] &= ~mask;
}

// Key name or decimal ASCII code -> SAM key(s)
bool lookup_key(const std::string& str, eSamKey& mod, eSamKey& key)
{
    auto name = to_lower(str);
    static const std::map<std::string, std::string> aliases
    {
        {"esc", "escape"}, {"enter", "return"}, {"del", "delete"}, {"sym", "symbol"},
        {"symbolshift", "symbol"}, {"ctrl", "cntrl"}, {"control", "cntrl"}, {"capslock", "caps"},
    };
    if (auto it = aliases.find(name); it != aliases.end())
        name = it->second;

    // Single digits are ASCII codes, as in ZRCP, so only match letters and words by name
    if (!std::isdigit(static_cast<unsigned char>(name[0])))
    {
        for (int i = SK_MIN; i < SK_MAX; ++i)
        {
            if (name == sam_key_names[i])
            {
                mod = SK_NONE;
                key = static_cast<eSamKey>(i);
                return true;
            }
        }
    }

    if (auto code = parse_number(str); code && *code >= 0 && *code <= 255)
        return Keyboard::CharToSamKeys(*code, mod, key);

    return false;
}

void set_typed_char(int ch)
{
    typed_keys.fill(0);
    eSamKey mod, key;
    if (Keyboard::CharToSamKeys(ch, mod, key))
    {
        set_key(typed_keys, mod, true);
        set_key(typed_keys, key, true);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Output and pending replies

void update_trace()
{
    bool any_breakpoints = breakpoints_enabled &&
        std::any_of(breakpoints.begin(), breakpoints.end(), [](auto& bp) { return bp.enabled; });

    trace_instructions = coverage_enabled || history_enabled || any_breakpoints ||
        (breakpoints_enabled && !mem_breakpoints.empty()) ||
        pending.type == Wait::Instructions || (pending.type == Wait::Break && pending.count);

    io_hooks = io_log_enabled || (breakpoints_enabled && !port_breakpoints.empty());
}

void send(const std::string& str)
{
    if (client_socket != INVALID_SOCKET)
        out_buf += str;
}

std::string prompt()
{
    return stopped ? STEP_PROMPT : PROMPT;
}

void complete(const std::string& reply)
{
    auto owner = pending.owner;
    pending = {};
    update_trace();

    if (owner == client_id)
        send(reply + prompt());
}

void stop_typing()
{
    type_queue.clear();
    typed_keys.fill(0);
}

// Stop the emulation and finish any waiting command with the given message
void stop_with(const std::string& message)
{
    Log(message);
    stopped = true;

    if (pending.type == Wait::Typing)
        stop_typing();

    if (pending.type != Wait::None)
        complete(message + "\n");
}

////////////////////////////////////////////////////////////////////////////////
// Commands

struct Request
{
    std::vector<std::string> args;
    std::string rest;           // everything after the command name
    std::string raw_rest;       // the same, keeping trailing spaces (for typed text)
    std::string out;
    bool in_action = false;     // run as a breakpoint action
    bool deferred = false;      // reply comes later
};

using Handler = void (*)(Request&);

struct Command
{
    const char* name;
    const char* alias;
    const char* syntax;
    const char* help;
    Handler handler;
};

extern const std::vector<Command> commands;

void execute(const std::string& line, Request& req);

bool need_args(Request& req, size_t n)
{
    if (req.args.size() < n)
    {
        req.out += "Error. Missing parameters\n";
        return false;
    }
    return true;
}

bool get_number(Request& req, size_t index, int& value)
{
    auto n = parse_number(req.args[index]);
    if (!n)
    {
        req.out += fmt::format("Error. Invalid number: {}\n", req.args[index]);
        return false;
    }
    value = *n;
    return true;
}

// Reads a number argument and checks it lies in lo..hi
bool get_number_range(Request& req, size_t index, int& value, int lo, int hi)
{
    int n;
    if (!get_number(req, index, n))
        return false;
    if (n < lo || n > hi)
    {
        req.out += fmt::format("Error. Value out of range ({} to {}): {}\n", lo, hi, req.args[index]);
        return false;
    }
    value = n;
    return true;
}

// An address argument: a number, or a loaded label with an optional +N or -N. The label's value
// is a 16-bit CPU-view address whatever zone is selected.
std::optional<int> parse_address(const std::string& arg, std::string& error)
{
    if (auto label = Symbol::FindRemote(arg))
        return label;

    auto sign = arg.find_first_of("+-", 1);
    if (sign != std::string::npos)
    {
        if (auto base = Symbol::FindRemote(arg.substr(0, sign)))
        {
            auto offset = parse_number(arg.substr(sign + 1));
            if (!offset || *offset < 0)
            {
                error = fmt::format("Error. Invalid offset after the label: {}\n", arg);
                return std::nullopt;
            }

            int64_t addr = *base + (arg[sign] == '+' ? int64_t(*offset) : -int64_t(*offset));
            if (addr < 0 || addr > 0xffff)
            {
                error = fmt::format("Error. Label plus offset is outside 0 to FFFFH: {}\n", arg);
                return std::nullopt;
            }
            return static_cast<int>(addr);
        }
    }

    if (auto n = parse_number(arg))
        return n;

    auto first = arg.empty() ? '\0' : arg[0];
    if (std::isalpha(static_cast<unsigned char>(first)) || first == '_' || first == '.')
        error = fmt::format("Error. Unknown label: {}{}\n", arg, Symbol::HasRemote() ? "" : " (no symbols loaded)");
    else
        error = fmt::format("Error. Invalid number: {}\n", arg);
    return std::nullopt;
}

// get_number for an address: also accepts labels
bool get_address(Request& req, size_t index, int& value)
{
    std::string error;
    auto n = parse_address(req.args[index], error);
    if (!n)
    {
        req.out += error;
        return false;
    }
    value = *n;
    return true;
}

bool get_address_range(Request& req, size_t index, int& value, int lo, int hi)
{
    int n;
    if (!get_address(req, index, n))
        return false;
    if (n < lo || n > hi)
    {
        req.out += fmt::format("Error. Value out of range ({} to {}): {}\n", lo, hi, req.args[index]);
        return false;
    }
    value = n;
    return true;
}

// Names accepted wherever a port number is (the low byte of the port: most SAM ports ignore
// the high byte). Shared by set-register (the ones marked settable) and the port commands.
struct PortName
{
    const char* name;
    uint8_t port;
    bool settable;
};
const std::vector<PortName> port_names
{
    { "kempston", KEMPSTON_PORT, false }, { "lepr", LEPR_PORT, false }, { "hepr", HEPR_PORT, false },
    { "clock", CLOCK_PORT, false }, { "lpen", LPEN_PORT, false }, { "clut", CLUT_BASE_PORT, false },
    { "status", STATUS_PORT, false }, { "line", LINE_PORT, false },
    { "lmpr", LMPR_PORT, true }, { "hmpr", HMPR_PORT, true }, { "vmpr", VMPR_PORT, true },
    { "midi", MIDI_PORT, false }, { "border", BORDER_PORT, true }, { "keyboard", KEYBOARD_PORT, false },
    { "attr", ATTR_PORT, false }, { "saa", SAA_PORT, false },
};

const PortName* find_port_name(const std::string& name)
{
    auto lname = to_lower(name);
    for (auto& pn : port_names)
    {
        if (lname == pn.name)
            return &pn;
    }
    return nullptr;
}

// Reads a port argument: a name from the table above, or a number 0-FFFFH
bool get_port(Request& req, size_t index, int& port)
{
    if (auto pn = find_port_name(req.args[index]))
    {
        port = pn->port;
        return true;
    }
    return get_number_range(req, index, port, 0, 0xffff);
}

// The window's own GUI (the debugger or a dialog) holds the emulation while it's open, and
// upstream's pause holds it until a key is pressed in the window. A command that waits for
// the emulation to run would never finish, and stopping it would leave the GUI undrawn.
bool held_by_window(Request& req)
{
    if (GUI::IsActive())
        req.out += "Error. The emulator's debugger or a dialog is open in its window\n";
    else if (g_fPaused)
        req.out += "Error. The emulator is paused (press a key in its window)\n";
    else
        return false;

    return true;
}

bool can_defer(Request& req)
{
    if (req.in_action)
    {
        req.out += "Error. Not allowed in a breakpoint action\n";
        return false;
    }
    return !held_by_window(req);
}

// The state before the instruction about to run (the CPU's current PC and registers)
void history_capture(HistoryEntry& e)
{
    auto pc = cpu.get_pc();
    e.pc = pc;
    e.sp = cpu.get_sp();
    e.af = cpu.get_af();
    e.bc = cpu.get_bc();
    e.de = cpu.get_de();
    e.hl = cpu.get_hl();
    e.ix = cpu.get_ix();
    e.iy = cpu.get_iy();
    e.af2 = cpu.get_alt_af();
    e.bc2 = cpu.get_alt_bc();
    e.de2 = cpu.get_alt_de();
    e.hl2 = cpu.get_alt_hl();
    e.i = cpu.get_i();
    e.r = cpu.get_r();
    e.im = static_cast<uint8_t>(cpu.get_int_mode());
    e.iffs = (cpu.get_iff1() ? 1 : 0) | (cpu.get_iff2() ? 2 : 0);
    e.lmpr = IO::State().lmpr;
    e.hmpr = IO::State().hmpr;
    e.vmpr = IO::State().vmpr;
    e.page = PtrPage(AddrReadPtr(pc));
    for (int j = 0; j < 4; ++j)
        e.bytes[j] = read_byte(static_cast<uint16_t>(pc + j));
    e.repeats = 1;
}

// Called when execution resumes (and when recording starts): captures the instruction that is
// about to run. It is only added to the history when the next hook shows it has run.
void history_arm()
{
    history_armed = history_enabled;
    if (history_armed)
        history_capture(history_pending);
}

// Called after each instruction has run: adds it (as captured before it ran, a repeat of the
// previous entry's address being counted instead), then captures the next one
void history_record()
{
    if (history_armed)
    {
        auto& last = history[(history_next + HISTORY_SLOTS - 1) % HISTORY_SLOTS];
        if (history_count && last.pc == history_pending.pc && last.page == history_pending.page)
        {
            auto repeats = last.repeats + 1;
            last = history_pending;
            last.repeats = repeats;
        }
        else
        {
            history[history_next] = history_pending;
            history_next = (history_next + 1) % HISTORY_SLOTS;
            if (history_count < HISTORY_SLOTS)
                ++history_count;
            else
                ++history_dropped;
        }
    }

    history_armed = true;
    history_capture(history_pending);
}

void defer(Request& req, Wait type, int count)
{
    pending.type = type;
    pending.count = pending.total = count;
    pending.owner = client_id;
    pending.queued = in_buf.size();
    req.deferred = true;
    last_instr_pc = cpu.get_pc();
    history_arm();
    update_trace();
}

void cmd_help(Request& req)
{
    if (!req.args.empty())
    {
        auto name = to_lower(req.args[0]);
        for (auto& cmd : commands)
        {
            if (name == cmd.name || (cmd.alias && name == cmd.alias))
            {
                req.out += fmt::format("Syntax: {}{}{} {}\n{}\n", cmd.name,
                    cmd.alias ? "|" : "", cmd.alias ? cmd.alias : "", cmd.syntax, cmd.help);
                return;
            }
        }
        req.out += "Error. Unknown command\n";
        return;
    }

    req.out += "Available commands (help <command> for details):\n";
    for (auto& cmd : commands)
        req.out += fmt::format("{:<24} {}\n", cmd.name, cmd.syntax);
}

void cmd_version(Request& req)
{
    req.out += fmt::format("SimCoupe {}.{}.{}\n",
        SIMCOUPE_MAJOR_VERSION, SIMCOUPE_MINOR_VERSION, SIMCOUPE_PATCH_VERSION);
}

void cmd_noop(Request&)
{
}

void cmd_quit(Request& req)
{
    req.out += "Sayonara baby\n";
    close_after_flush = true;
}

void cmd_exit_emulator(Request& req)
{
    req.out += "Exiting emulator\n";
    close_after_flush = true;
    g_fQuit = true;
}

void cmd_get_registers(Request& req)
{
    req.out += registers_string() + "\n";
}

void cmd_set_register(Request& req)
{
    if (!need_args(req, 1))
        return;

    static const std::map<std::string, Expr::Token> regs
    {
        {"a", Expr::Token::A}, {"f", Expr::Token::F}, {"b", Expr::Token::B}, {"c", Expr::Token::C},
        {"d", Expr::Token::D}, {"e", Expr::Token::E}, {"h", Expr::Token::H}, {"l", Expr::Token::L},
        {"af", Expr::Token::AF}, {"bc", Expr::Token::BC}, {"de", Expr::Token::DE}, {"hl", Expr::Token::HL},
        {"af'", Expr::Token::Alt_AF}, {"bc'", Expr::Token::Alt_BC}, {"de'", Expr::Token::Alt_DE}, {"hl'", Expr::Token::Alt_HL},
        {"ix", Expr::Token::IX}, {"iy", Expr::Token::IY}, {"ixh", Expr::Token::IXH}, {"ixl", Expr::Token::IXL},
        {"iyh", Expr::Token::IYH}, {"iyl", Expr::Token::IYL}, {"sp", Expr::Token::SP}, {"pc", Expr::Token::PC},
        {"i", Expr::Token::I}, {"r", Expr::Token::R}, {"im", Expr::Token::IM},
        {"iff1", Expr::Token::IFF1}, {"iff2", Expr::Token::IFF2},
    };
    auto settable_port = [](const std::string& name) -> const PortName*
    {
        auto pn = find_port_name(name);
        return (pn && pn->settable) ? pn : nullptr;
    };

    auto max_value = [](const std::string& name)
    {
        static const std::set<std::string> regs16
        {
            "af", "bc", "de", "hl", "af'", "bc'", "de'", "hl'", "ix", "iy", "sp", "pc",
        };
        if (regs16.count(name))
            return 0xffff;
        if (name == "im")
            return 2;
        if (name == "iff1" || name == "iff2")
            return 1;
        return 0xff;
    };

    // Check every assignment before applying any, so a bad one changes nothing
    for (auto& arg : req.args)
    {
        auto eq = arg.find('=');
        auto name = to_lower(arg.substr(0, eq));
        std::string error;
        auto value = (eq == std::string::npos) ? std::nullopt : parse_address(arg.substr(eq + 1), error);
        if (!value)
        {
            req.out += error.empty() ? fmt::format("Error. Expected register=value: {}\n", arg) : error;
            return;
        }

        if (!regs.count(name) && !settable_port(name))
        {
            req.out += fmt::format("Error. Unknown register: {}\n", name);
            return;
        }

        if (*value < 0 || *value > max_value(name))
        {
            req.out += fmt::format("Error. Value out of range (0 to {}): {}\n", max_value(name), arg.substr(eq + 1));
            return;
        }
    }

    for (auto& arg : req.args)
    {
        auto eq = arg.find('=');
        auto name = to_lower(arg.substr(0, eq));
        std::string error;
        auto value = (eq == std::string::npos) ? std::nullopt : parse_address(arg.substr(eq + 1), error);
        if (!value)
        {
            req.out += error.empty() ? fmt::format("Error. Expected register=value: {}\n", arg) : error;
            return;
        }

        if (auto it = regs.find(name); it != regs.end())
            Expr::SetReg(it->second, *value);
        else if (auto port = settable_port(name))
            IO::Out(port->port, static_cast<uint8_t>(*value));
    }

    req.out += registers_string() + "\n";
}

void cmd_enter_step(Request& req)
{
    if (GUI::IsActive())
    {
        held_by_window(req);
        return;
    }

    stopped = true;
    if (!req.in_action)
        Log("Entered cpu-step mode");
}

void cmd_exit_step(Request&)
{
    stopped = false;
    last_instr_pc = cpu.get_pc();
    history_arm();
}

void cmd_cpu_step(Request& req)
{
    if (!can_defer(req))
        return;

    stopped = true;
    defer(req, Wait::Instructions, 1);
}

void cmd_run(Request& req)
{
    if (!can_defer(req))
        return;

    int limit = 0;
    if (!req.args.empty() && !get_number_range(req, 0, limit, 0, std::numeric_limits<int>::max()))
        return;

    stopped = false;
    defer(req, Wait::Break, limit);
}

void cmd_run_frames(Request& req)
{
    if (!can_defer(req))
        return;

    int frames = 1;
    if (!req.args.empty() && !get_number_range(req, 0, frames, 0, std::numeric_limits<int>::max()))
        return;

    if (frames <= 0)
    {
        stopped = true;
        return;
    }

    stopped = false;
    defer(req, Wait::Frames, frames);
}

void cmd_reset(Request&)
{
    CPU::Reset(true);
    CPU::Reset(false);
}

void cmd_hard_reset(Request&)
{
    for (int page = INTMEM; page < ROM0; ++page)
        memset(PageWritePtr(page), 0, MEM_PAGE_SIZE);

    CPU::Reset(true);
    CPU::Reset(false);
}

void cmd_nmi(Request&)
{
    CPU::NMI();
}

void cmd_set_speed(Request& req)
{
    if (req.args.empty())
    {
        req.out += fmt::format("{}\n", (g_nTurbo & TURBO_REMOTE) ? "turbo" : std::to_string(GetOption(speed)));
        return;
    }

    auto mode = to_lower(req.args[0]);
    if (mode == "turbo")
        g_nTurbo |= TURBO_REMOTE;
    else if (mode == "normal")
    {
        g_nTurbo &= ~TURBO_REMOTE;
        SetOption(speed, 100);
    }
    else if (auto percent = parse_number(mode); percent && *percent >= 50 && *percent <= 1000)
    {
        g_nTurbo &= ~TURBO_REMOTE;
        SetOption(speed, *percent);
    }
    else
        req.out += "Error. Use normal, turbo or a percentage (50-1000)\n";
}

void cmd_read_memory(Request& req)
{
    int addr = 0, len = 1;
    if (!req.args.empty() && !get_address(req, 0, addr))
        return;
    if (req.args.size() > 1 && !get_number(req, 1, len))
        return;
    if (!zone_range_ok(addr, len, req.out))
        return;

    std::string hex;
    hex.reserve(len * 2);
    for (int i = 0; i < len; ++i)
        hex += fmt::format("{:02X}", zone_read(addr + i));
    req.out += hex + "\n";
}

void cmd_write_memory(Request& req)
{
    int addr = 0;
    if (!need_args(req, 2) || !get_address(req, 0, addr))
        return;

    std::vector<uint8_t> data;
    for (size_t i = 1; i < req.args.size(); ++i)
    {
        int value;
        if (!get_number_range(req, i, value, -128, 255))
            return;
        data.push_back(static_cast<uint8_t>(value));
    }

    if (!zone_range_ok(addr, static_cast<int>(data.size()), req.out))
        return;

    for (size_t i = 0; i < data.size(); ++i)
        zone_write(addr + static_cast<int>(i), data[i]);
}

void cmd_write_memory_raw(Request& req)
{
    int addr = 0;
    if (!need_args(req, 2) || !get_address(req, 0, addr))
        return;

    auto& hex = req.args[1];
    if (hex.size() % 2 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    {
        req.out += "Error. Expected an even number of hex digits\n";
        return;
    }

    auto len = static_cast<int>(hex.size() / 2);
    if (!zone_range_ok(addr, len, req.out))
        return;

    for (int i = 0; i < len; ++i)
        zone_write(addr + i, static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16)));
}

void cmd_load_binary(Request& req)
{
    int addr = 0, len = 0;
    if (!need_args(req, 2) || !get_address(req, 1, addr))
        return;
    if (req.args.size() > 2 && !get_number(req, 2, len))
        return;

    std::ifstream file(req.args[0], std::ios::binary);
    if (!file)
    {
        req.out += fmt::format("Error. Can't open {}\n", req.args[0]);
        return;
    }

    if (len < 0 || len > MAX_LENGTH)
    {
        req.out += "Error. Address or length out of range for this memory zone\n";
        return;
    }

    // Read at most MAX_LENGTH + 1 bytes (or the requested length), so a huge file can't exhaust memory
    // (sized from the file, so a small file doesn't cost a 16M buffer)
    file.seekg(0, std::ios::end);
    auto file_size = static_cast<int64_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    if (file_size < 0)
    {
        req.out += fmt::format("Error. Can't read {}\n", req.args[0]);
        return;
    }

    auto limit = static_cast<int>(std::min<int64_t>(file_size, (len > 0) ? len : MAX_LENGTH + 1));
    std::vector<uint8_t> data(limit);
    file.read(reinterpret_cast<char*>(data.data()), limit);
    data.resize(static_cast<size_t>(file.gcount()));

    if (!zone_range_ok(addr, static_cast<int>(data.size()), req.out))
        return;

    for (size_t i = 0; i < data.size(); ++i)
        zone_write(addr + static_cast<int>(i), data[i]);

    req.out += fmt::format("Loaded {} bytes\n", data.size());
}

void cmd_save_binary(Request& req)
{
    int addr = 0, len = 0;
    if (!need_args(req, 3) || !get_address(req, 1, addr) || !get_number(req, 2, len))
        return;
    if (!zone_range_ok(addr, len, req.out))
        return;

    std::ofstream file(req.args[0], std::ios::binary);
    if (!file)
    {
        req.out += fmt::format("Error. Can't create {}\n", req.args[0]);
        return;
    }

    for (int i = 0; i < len; ++i)
        file.put(static_cast<char>(zone_read(addr + i)));
}

void cmd_get_memory_zones(Request& req)
{
    req.out += "Zone: -1 Name: Mapped memory\n";
    for (int zone = 0; zone < TOTAL_PAGES; ++zone)
    {
        if (zone_valid(zone))
        {
            req.out += fmt::format("Zone: {} Name: {} Size: {} R/W: 3\n",
                zone, zone_name(zone), MEM_PAGE_SIZE);
        }
    }
}

void cmd_set_memory_zone(Request& req)
{
    int zone = -1;
    if (!need_args(req, 1) || !get_number(req, 0, zone))
        return;

    if (!zone_valid(zone))
    {
        req.out += "Error. Invalid memory zone\n";
        return;
    }

    memory_zone = zone;
}

void cmd_get_paging_state(Request& req)
{
    auto& io = IO::State();
    req.out += fmt::format(
        "LMPR={:02X} HMPR={:02X} VMPR={:02X} LEPR={:02X} HEPR={:02X} BORDER={:02X} MODE={} SCREEN={} "
        "ROM0={} ROM1={} WPROT={} A={} B={} C={} D={}\n",
        io.lmpr, io.hmpr, io.vmpr, io.lepr, io.hepr, io.border,
        ((io.vmpr & VMPR_MODE_MASK) >> VMPR_MODE_SHIFT) + 1,
        Memory::PageDesc(io.vmpr & VMPR_PAGE_MASK, true),
        (io.lmpr & LMPR_ROM0_OFF) ? "off" : "on", (io.lmpr & LMPR_ROM1) ? "on" : "off",
        (io.lmpr & LMPR_WPROT) ? "on" : "off",
        Memory::PageDesc(GetSectionPage(Section::A), true), Memory::PageDesc(GetSectionPage(Section::B), true),
        Memory::PageDesc(GetSectionPage(Section::C), true), Memory::PageDesc(GetSectionPage(Section::D), true));
}

void cmd_get_screen_mode(Request& req)
{
    auto& io = IO::State();
    req.out += fmt::format("MODE={} PAGE={:02X} BORDER={}\n",
        ((io.vmpr & VMPR_MODE_MASK) >> VMPR_MODE_SHIFT) + 1, io.vmpr & VMPR_PAGE_MASK,
        BORDER_COLOUR(io.border));
}

void cmd_get_tstates(Request& req)
{
    req.out += fmt::format("{}\n", CPU::frame_cycles);
}

// Disassembles one instruction. With symbols loaded, jump targets and operands are named (names
// come with colour codes: \a and a letter, which are removed)
unsigned disassemble_text(uint8_t* bytes, uint16_t pc, char* text, size_t size)
{
    bool symbols = Symbol::HasRemote();
    auto len = Disassemble(bytes, pc, text, size, symbols ? DISASSEMBLE_LABEL_LEN : 0);
    if (symbols)
    {
        std::string plain;
        for (auto* p = text; *p; ++p)
        {
            if (*p == '\a' && p[1])
                ++p;
            else
                plain += *p;
        }
        std::strncpy(text, plain.c_str(), size - 1);
        text[size - 1] = '\0';
    }
    return len;
}

constexpr int MAX_DISASSEMBLE_LINES = 0x10000;

void cmd_disassemble(Request& req)
{
    int addr = cpu.get_pc(), lines = 1;
    if (!req.args.empty() && !get_address_range(req, 0, addr, 0, 0xffff))
        return;
    if (req.args.size() > 1 && !get_number_range(req, 1, lines, 0, MAX_DISASSEMBLE_LINES))
        return;

    for (int i = 0; i < lines; ++i)
    {
        auto pc = static_cast<uint16_t>(addr);
        std::array<uint8_t, 4> bytes{};
        for (int j = 0; j < 4; ++j)
            bytes[j] = read_byte(static_cast<uint16_t>(pc + j));

        char text[256]{};
        auto len = disassemble_text(bytes.data(), pc, text, sizeof(text));
        if (Symbol::HasRemote())
        {
            if (auto label = Symbol::NameRemote(pc, 0); !label.empty())
                req.out += label + ":\n";
        }

        std::string hex;
        for (unsigned j = 0; j < len; ++j)
            hex += fmt::format("{:02X}", bytes[j]);

        req.out += fmt::format("{:04X} {:<8} {}\n", pc, hex, text);
        addr += len;
    }
}

void cmd_send_keys_event(Request& req)
{
    if (!need_args(req, 2))
        return;

    eSamKey mod, key;
    if (!lookup_key(req.args[0], mod, key))
    {
        req.out += fmt::format("Error. Unknown key: {}\n", req.args[0]);
        return;
    }

    bool pressed = req.args[1] != "0";
    set_key(held_keys, mod, pressed);
    set_key(held_keys, key, pressed);
}

constexpr int MAX_KEY_MS = 3600000;   // longest key hold for the typing commands

void start_typing(Request& req, int ms, const std::vector<int>& chars)
{
    if (!can_defer(req))
        return;

    if (chars.empty())
        return;

    type_frames = std::max(1, (ms + MS_PER_FRAME / 2) / MS_PER_FRAME);
    type_queue.assign(chars.begin() + 1, chars.end());
    set_typed_char(chars.front());
    type_key_down = true;
    type_frames_left = type_frames;
    defer(req, Wait::Typing, 0);
}

void cmd_send_keys_string(Request& req)
{
    // Only the delay is a separate argument: the text may be nothing but spaces
    int ms = 0;
    if (!need_args(req, 1) || !get_number_range(req, 0, ms, 0, MAX_KEY_MS))
        return;

    // The text is the rest of the line after the delay and one separator, spaces included
    auto text_start = req.raw_rest.find(req.args[0]) + req.args[0].size() + 1;
    if (text_start >= req.raw_rest.size())
    {
        req.out += "Error. Missing parameters\n";
        return;
    }

    auto text = req.raw_rest.substr(text_start);
    start_typing(req, ms, std::vector<int>(text.begin(), text.end()));
}

void cmd_send_keys_ascii(Request& req)
{
    int ms = 0;
    if (!need_args(req, 2) || !get_number_range(req, 0, ms, 0, MAX_KEY_MS))
        return;

    std::vector<int> chars;
    for (size_t i = 1; i < req.args.size(); ++i)
    {
        int code;
        if (!get_number_range(req, i, code, 0, 255))
            return;
        chars.push_back(code);
    }

    start_typing(req, ms, chars);
}

void cmd_release_keys(Request&)
{
    held_keys.fill(0);
    if (pending.type != Wait::Typing)
        stop_typing();
}

void cmd_save_screen(Request& req)
{
    if (!need_args(req, 1))
        return;

    if (!PNG::Save(Frame::GetFrameBuffer(), req.rest))
        req.out += fmt::format("Error. Can't save {}\n", req.rest);
}

void cmd_insert_disk(Request& req)
{
    if (!need_args(req, 2))
        return;

    int drive = 0;
    if (!get_number(req, 0, drive) || (drive != 1 && drive != 2))
    {
        req.out += "Error. Drive must be 1 or 2\n";
        return;
    }

    if ((drive == 1 ? GetOption(drive1) : GetOption(drive2)) != drvFloppy)
    {
        req.out += fmt::format("Error. Floppy drive {} is not present\n", drive);
        return;
    }

    // An optional final "boot" resets and boots the disk
    auto path = trim(req.rest.substr(req.rest.find(req.args[0]) + req.args[0].size()));
    bool boot = false;
    if (req.args.size() > 2 && to_lower(req.args.back()) == "boot")
    {
        boot = true;
        path = trim(path.substr(0, path.size() - 4));
    }

    auto& floppy = (drive == 1) ? pFloppy1 : pFloppy2;
    if (!floppy->Insert(path))
    {
        req.out += fmt::format("Error. Can't insert {}\n", path);
        return;
    }

    if (boot)
    {
        CPU::Reset(true);
        CPU::Reset(false);
        IO::QueueAutoBoot(AutoLoadType::Disk);
    }
}

void cmd_eject_disk(Request& req)
{
    int drive = 1;
    if (!req.args.empty() && (!get_number(req, 0, drive) || (drive != 1 && drive != 2)))
    {
        req.out += "Error. Drive must be 1 or 2\n";
        return;
    }

    ((drive == 1) ? pFloppy1 : pFloppy2)->Eject();
}

void cmd_enable_breakpoints(Request&)
{
    breakpoints_enabled = true;
    update_trace();
}

void cmd_disable_breakpoints(Request&)
{
    breakpoints_enabled = false;
    update_trace();
}

bool get_breakpoint_index(Request& req, int& index)
{
    if (!need_args(req, 1) || !get_number(req, 0, index))
        return false;

    if (index < 1 || index > MAX_BREAKPOINTS)
    {
        req.out += fmt::format("Error. Index out of range (1-{})\n", MAX_BREAKPOINTS);
        return false;
    }
    return true;
}

std::string text_after_first_arg(Request& req)
{
    auto pos = req.rest.find(req.args[0]) + req.args[0].size();
    return trim(req.rest.substr(pos));
}

void cmd_set_breakpoint(Request& req)
{
    int index;
    if (!get_breakpoint_index(req, index))
        return;

    if (!breakpoints_enabled)
    {
        req.out += "Error. You must enable breakpoints first\n";
        return;
    }

    auto& bp = breakpoints[index - 1];
    auto condition = text_after_first_arg(req);
    if (condition.empty())
    {
        bp = {};
    }
    else
    {
        std::string remain;
        auto expr = Expr::Compile(condition, remain);
        if (!expr || !trim(remain).empty())
        {
            req.out += fmt::format("Error. Invalid condition (a syntax error, or a label that is not loaded): {}\n", condition);
            return;
        }

        bp.condition = condition;
        bp.expr = expr;
        bp.enabled = true;
        bp.last_value = false;
    }

    update_trace();
}

void cmd_set_breakpoint_action(Request& req)
{
    int index;
    if (!get_breakpoint_index(req, index))
        return;

    breakpoints[index - 1].action = text_after_first_arg(req);
}

void enable_breakpoint(Request& req, bool enable)
{
    int index;
    if (!get_breakpoint_index(req, index))
        return;

    auto& bp = breakpoints[index - 1];
    if (bp.condition.empty())
    {
        req.out += "Error. Breakpoint is not set\n";
        return;
    }

    bp.enabled = enable;
    bp.last_value = false;
    update_trace();
}

void cmd_enable_breakpoint(Request& req)
{
    enable_breakpoint(req, true);
}

void cmd_disable_breakpoint(Request& req)
{
    enable_breakpoint(req, false);
}

void cmd_get_breakpoints(Request& req)
{
    req.out += fmt::format("Breakpoints: {}\n", breakpoints_enabled ? "On" : "Off");

    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
    {
        auto& bp = breakpoints[i];
        if (bp.condition.empty())
            continue;

        req.out += fmt::format("{} {}: {}{}{}\n", bp.enabled ? "Enabled" : "Disabled", i + 1,
            bp.condition, bp.action.empty() ? "" : " -> ", bp.action);
    }

    for (auto& mbp : mem_breakpoints)
    {
        static const char* types[] = { "", "read", "write", "read/write" };
        if (mbp.page >= 0)
            req.out += fmt::format("Memory {}:{:04X}H-{:04X}H: {}\n", Memory::PageDesc(mbp.page, true), mbp.from, mbp.to, types[mbp.type]);
        else
            req.out += fmt::format("Memory {:04X}H-{:04X}H: {}\n", mbp.from, mbp.to, types[mbp.type]);
    }

    for (auto& pbp : port_breakpoints)
    {
        static const char* types[] = { "", "in", "out", "in/out" };
        req.out += fmt::format("Port {:04X}H mask {:04X}H: {}\n", pbp.port, pbp.mask, types[pbp.type]);
    }
}

// Parses a physical location, "page:offset" as printed by cpu-code-coverage get-physical: the
// page is RAMnn, EXTnn or ROMn (the numbers in hex), the offset hex (0 to 3FFF)
bool parse_physical(Request& req, const std::string& arg, int& page, int& offset)
{
    auto colon = arg.find(':');
    auto name = to_lower(arg.substr(0, colon));
    auto off = to_lower(arg.substr(colon + 1));

    auto parse_hex = [](std::string str, int max) -> std::optional<int>
    {
        if (str.size() > 2 && str[0] == '0' && str[1] == 'x')
            str = str.substr(2);
        else if (!str.empty() && str[0] == '$')
            str = str.substr(1);
        else if (str.size() > 1 && str.back() == 'h')
            str.pop_back();

        if (str.empty() || str.size() > 8 || !std::all_of(str.begin(), str.end(), [](unsigned char c) { return std::isxdigit(c); }))
            return std::nullopt;

        auto value = std::stoll(str, nullptr, 16);
        if (value > max)
            return std::nullopt;
        return static_cast<int>(value);
    };

    int base = -1;
    if (name.compare(0, 3, "ram") == 0)
        base = INTMEM;
    else if (name.compare(0, 3, "ext") == 0)
        base = EXTMEM;
    else if (name.compare(0, 3, "rom") == 0)
        base = ROM0;

    auto number = (base < 0) ? std::optional<int>{} : parse_hex(name.substr(3), 0xffff);
    if (!number)
    {
        req.out += fmt::format("Error. Invalid page (use RAMnn, EXTnn, ROM0 or ROM1, number in hex): {}\n", arg.substr(0, colon));
        return false;
    }

    page = base + *number;
    if (base == ROM0 ? (*number > 1) : (!zone_valid(page) || *number >= (base == INTMEM ? NUM_INTERNAL_PAGES : (ROM0 - EXTMEM))))
    {
        req.out += fmt::format("Error. No such page in this configuration: {}\n", arg.substr(0, colon));
        return false;
    }

    auto off_value = parse_hex(off, 0xffffff);
    if (!off_value || *off_value >= MEM_PAGE_SIZE)
    {
        req.out += fmt::format("Error. Offset out of range (0 to {:X}): {}\n", MEM_PAGE_SIZE - 1, arg.substr(colon + 1));
        return false;
    }

    offset = *off_value;
    return true;
}

void cmd_set_membreakpoint(Request& req)
{
    static const char* usage = "Error. Usage: set-membreakpoint address|page:offset type(0-3) [length]\n";

    int addr = 0, type = 0, len = 1, page = -1;
    if (!need_args(req, 2) || !get_number(req, 1, type))
        return;

    if (req.args[0].find(':') != std::string::npos)
    {
        if (!parse_physical(req, req.args[0], page, addr))
            return;
    }
    else if (!get_address(req, 0, addr))
        return;

    if (req.args.size() > 2 && !get_number(req, 2, len))
        return;

    auto limit = (page >= 0) ? MEM_PAGE_SIZE : 0x10000;
    if (type < MEMBP_NONE || type > MEMBP_READWRITE || len < 1 || len > 0x10000 || addr < 0 || addr >= limit)
    {
        req.out += usage;
        return;
    }

    // Writes to ROM are discarded by the hardware, so can never be seen (unless ROM writes are enabled)
    if (page >= ROM0 && (type & MEMBP_WRITE) && !GetOption(romwrite))
    {
        req.out += "Error. ROM can't be written, so only read breakpoints (type 1) are possible\n";
        return;
    }

    if (addr + len > limit)
    {
        req.out += (page >= 0) ? "Error. Length runs past the end of the page\n" : usage;
        return;
    }

    auto to = addr + len - 1;

    // Replace any overlapping ranges in the same address space
    mem_breakpoints.erase(std::remove_if(mem_breakpoints.begin(), mem_breakpoints.end(),
        [&](auto& mbp) { return mbp.page == page && mbp.from <= to && mbp.to >= addr; }), mem_breakpoints.end());

    if (type != MEMBP_NONE)
        mem_breakpoints.push_back({ addr, to, type, page });

    // Accesses from before the breakpoint was set must not count
    Memory::last_phys_read1 = Memory::last_phys_read2 = nullptr;
    Memory::last_phys_write1 = Memory::last_phys_write2 = nullptr;

    update_trace();
}

void cmd_clear_membreakpoints(Request&)
{
    mem_breakpoints.clear();
    update_trace();
}

void cmd_set_portbreakpoint(Request& req)
{
    int port = 0, type = 0, mask = 0;
    if (!need_args(req, 2) || !get_port(req, 0, port) || !get_number(req, 1, type))
        return;

    if (type < PORTBP_NONE || type > PORTBP_INOUT)
    {
        req.out += "Error. Usage: set-portbreakpoint port type(0-3) [mask]\n";
        return;
    }

    // Most SAM ports are decoded from the low byte only, so a port below 100H matches on that
    mask = (port <= 0xff) ? 0xff : 0xffff;
    if (req.args.size() > 2 && !get_number_range(req, 2, mask, 0, 0xffff))
        return;

    port &= mask;

    // Replace any breakpoint with the same port and mask
    port_breakpoints.erase(std::remove_if(port_breakpoints.begin(), port_breakpoints.end(),
        [&](auto& pbp) { return pbp.port == port && pbp.mask == mask; }), port_breakpoints.end());

    if (type != PORTBP_NONE)
    {
        if (port_breakpoints.size() >= MAX_PORT_BREAKPOINTS)
        {
            req.out += fmt::format("Error. At most {} port breakpoints\n", MAX_PORT_BREAKPOINTS);
            return;
        }
        port_breakpoints.push_back({ port, mask, type });
    }

    update_trace();
}

void cmd_clear_portbreakpoints(Request&)
{
    port_breakpoints.clear();
    update_trace();
}

void cmd_get_breakpoint_log(Request& req)
{
    for (auto& line : log_lines)
        req.out += line + "\n";
}

void cmd_clear_breakpoint_log(Request&)
{
    log_lines.clear();
}

// Breakpoint action commands
void cmd_prints(Request& req)
{
    Log(req.rest);
}

void cmd_printe(Request& req)
{
    int value{};
    std::string remain;
    if (!Expr::Eval(req.rest, value, remain) || !trim(remain).empty())
    {
        req.out += fmt::format("Error. Invalid expression: {}\n", req.rest);
        return;
    }

    Log(fmt::format("{} = {} ({:X}H)", req.rest, value, value));
}

void cmd_putv(Request& req)
{
    int value{};
    std::string remain;
    if (!Expr::Eval(req.rest, value, remain) || !trim(remain).empty())
    {
        req.out += fmt::format("Error. Invalid expression: {}\n", req.rest);
        return;
    }

    Log(std::to_string(value));
}

void cmd_load_symbols(Request& req)
{
    if (!need_args(req, 1))
        return;

    bool append = false;
    if (req.args.size() > 1)
    {
        if (req.args.size() > 2 || to_lower(req.args[1]) != "append")
        {
            req.out += "Error. Usage: load-symbols file [append]\n";
            return;
        }
        append = true;
    }

    std::string error;
    size_t loaded, skipped;
    if (!Symbol::LoadFile(req.args[0], append, error, loaded, skipped))
    {
        req.out += fmt::format("Error. Symbols not loaded, {}\n", error);
        return;
    }

    req.out += fmt::format("Loaded {} symbols{}, {} in total\n", loaded,
        skipped ? fmt::format(" ({} skipped: value outside 0-FFFFH)", skipped) : "", Symbol::RemoteCount());
}

void cmd_clear_symbols(Request&)
{
    Symbol::ClearRemote();
}

void cmd_get_symbols(Request& req)
{
    auto list = Symbol::ListRemote(req.args.empty() ? "" : req.args[0]);
    req.out += fmt::format("Symbols: {} of {}\n", list.size(), Symbol::RemoteCount());
    for (auto& [addr, name] : list)
        req.out += fmt::format("{:04X} {}\n", addr, name);
}

void cmd_printregs(Request&)
{
    Log(registers_string());
}

void cmd_coverage(Request& req)
{
    if (!need_args(req, 1))
        return;

    auto action = to_lower(req.args[0]);
    if (action == "enabled")
    {
        if (!need_args(req, 2))
            return;

        coverage_enabled = (to_lower(req.args[1]) == "yes" || req.args[1] == "1" || to_lower(req.args[1]) == "true");
        if (coverage_enabled && coverage.empty())
            coverage.resize(TOTAL_PAGES * MEM_PAGE_SIZE / 8);
        update_trace();
    }
    else if (action == "clear")
    {
        std::fill(coverage.begin(), coverage.end(), 0);
    }
    else if (action == "get")
    {
        // "get symbols" adds the label (or label+offset) in brackets after each address
        bool names = req.args.size() > 1 && to_lower(req.args[1]) == "symbols";
        if (req.args.size() > 1 && !names)
        {
            req.out += "Error. Use get or get symbols\n";
            return;
        }

        if (coverage.empty())
        {
            req.out += "\n";
            return;
        }

        std::string s;
        for (int addr = 0; addr < 0x10000; ++addr)
        {
            auto offset = AddrReadPtr(static_cast<uint16_t>(addr)) - pMemory;
            if (coverage[offset >> 3] & (1 << (offset & 7)))
            {
                auto label = names ? sym_name(static_cast<uint16_t>(addr)) : "";
                s += fmt::format("{:04X}{} ", addr, label.empty() ? "" : "(" + label + ")");
            }
        }
        req.out += s + "\n";
    }
    else if (action == "get-physical")
    {
        std::string s;
        for (size_t i = 0; i < coverage.size(); ++i)
        {
            if (!coverage[i])
                continue;

            for (int bit = 0; bit < 8; ++bit)
            {
                if (coverage[i] & (1 << bit))
                {
                    auto ptr = pMemory + i * 8 + bit;
                    s += fmt::format("{}:{:04X} ", Memory::PageDesc(PtrPage(ptr), true), PtrOffset(ptr));
                }
            }
        }
        req.out += s + "\n";
    }
    else
    {
        req.out += "Error. Use enabled yes|no, clear, get or get-physical\n";
    }
}

bool is_yes(const std::string& arg, bool& yes)
{
    auto a = to_lower(arg);
    if (a == "yes" || a == "1" || a == "true")
        yes = true;
    else if (a == "no" || a == "0" || a == "false")
        yes = false;
    else
        return false;
    return true;
}

// Parses a filter token: port[:mask], the port a name or number (see get_port)
bool get_io_filter(Request& req, const std::string& token, IoFilter& filter)
{
    auto colon = token.find(':');

    Request sub;
    sub.args = { token.substr(0, colon) };
    int port = 0, mask = 0xffff;
    bool ok = get_port(sub, 0, port);

    if (ok && colon != std::string::npos)
    {
        sub.args = { token.substr(colon + 1) };
        ok = get_number_range(sub, 0, mask, 0, 0xffff);
    }
    else if (ok)
    {
        mask = (port <= 0xff) ? 0xff : 0xffff;
    }

    req.out += sub.out;
    filter = { port & mask, mask };
    return ok;
}

std::string io_filter_string()
{
    std::string s = io_filter_in ? (io_filter_out ? "in/out" : "in") : "out";
    if (io_filters.empty())
        return s + " all ports";

    for (auto& f : io_filters)
        s += fmt::format(" {:04X}H:{:04X}H", f.port, f.mask);
    return s;
}

void cmd_io_log(Request& req)
{
    if (!need_args(req, 1))
        return;

    auto action = to_lower(req.args[0]);
    if (action == "enabled")
    {
        bool yes;
        if (!need_args(req, 2))
            return;
        if (!is_yes(req.args[1], yes))
        {
            req.out += "Error. Use enabled yes|no\n";
            return;
        }

        io_log_enabled = yes;
        update_trace();
    }
    else if (action == "filter")
    {
        // Check everything before changing anything
        std::vector<IoFilter> filters;
        bool in = true, out = true;
        for (size_t i = 1; i < req.args.size(); ++i)
        {
            auto arg = to_lower(req.args[i]);
            if (arg == "in" || arg == "out")
            {
                in = (arg == "in");
                out = !in;
                continue;
            }

            IoFilter filter;
            if (!get_io_filter(req, req.args[i], filter))
                return;
            if (filters.size() >= MAX_IO_FILTERS)
            {
                req.out += fmt::format("Error. At most {} ports in a filter\n", MAX_IO_FILTERS);
                return;
            }
            filters.push_back(filter);
        }

        io_filters = filters;
        io_filter_in = in;
        io_filter_out = out;
    }
    else if (action == "get")
    {
        int count = static_cast<int>(MAX_IO_LOG);
        if (req.args.size() > 1 && !get_number_range(req, 1, count, 1, static_cast<int>(MAX_IO_LOG)))
            return;

        size_t first = io_log.size() - std::min(io_log.size(), static_cast<size_t>(count));
        req.out += fmt::format("I/O log {}: {} entries, {} dropped, filter: {}\n", io_log_enabled ? "on" : "off",
            io_log.size(), io_dropped, io_filter_string());

        for (size_t i = first; i < io_log.size(); ++i)
        {
            auto& e = io_log[i];
            auto label = sym_name(e.pc);
            req.out += fmt::format("frame={} cycle={} pc={:04X}H {} port={:04X}H value={:02X}H{}\n",
                e.frame, e.cycle, e.pc, e.is_out ? "out" : "in", e.port, e.value,
                label.empty() ? "" : " sym=" + label);
        }
    }
    else if (action == "clear")
    {
        io_log.clear();
        io_dropped = 0;
    }
    else
    {
        req.out += "Error. Use enabled yes|no, filter [in|out] [port[:mask] ...], get [count] or clear\n";
    }
}

void cmd_cpu_history(Request& req)
{
    if (!need_args(req, 1))
        return;

    auto action = to_lower(req.args[0]);
    if (action == "enabled")
    {
        bool yes;
        if (!need_args(req, 2))
            return;
        if (!is_yes(req.args[1], yes))
        {
            req.out += "Error. Use enabled yes|no\n";
            return;
        }

        if (yes && history.empty())
            history.resize(HISTORY_SLOTS);

        history_enabled = yes;
        history_arm();
        update_trace();
    }
    else if (action == "clear")
    {
        history_next = history_count = 0;
        history_dropped = 0;
    }
    else
    {
        req.out += "Error. Use enabled yes|no or clear\n";
    }
}

void cmd_get_cpu_history(Request& req)
{
    int lines = DEFAULT_HISTORY_LINES;
    if (!req.args.empty() && !get_number_range(req, 0, lines, 1, static_cast<int>(HISTORY_SLOTS)))
        return;

    auto count = std::min(history_count, static_cast<size_t>(lines));
    req.out += fmt::format("CPU history {}: {} entries, {} dropped, showing {}\n", history_enabled ? "on" : "off",
        history_count, history_dropped, count);

    for (size_t n = history_count - count; n < history_count; ++n)
    {
        auto& e = history[(history_next + HISTORY_SLOTS - history_count + n) % HISTORY_SLOTS];

        char text[256]{};
        auto len = disassemble_text(e.bytes.data(), e.pc, text, sizeof(text));
        std::string hex;
        for (unsigned j = 0; j < len; ++j)
            hex += fmt::format("{:02X}", e.bytes[j]);

        auto label = sym_name(e.pc);
        req.out += fmt::format(
            "{:04X}H {}:{:04X} {:<8} {:<22} AF={:04X} BC={:04X} DE={:04X} HL={:04X} SP={:04X} IX={:04X} IY={:04X} "
            "AF'={:04X} BC'={:04X} DE'={:04X} HL'={:04X} I={:02X} R={:02X} IM{} IFF{}{} LMPR={:02X} HMPR={:02X} VMPR={:02X}{}{}\n",
            e.pc, Memory::PageDesc(e.page, true), e.pc & (MEM_PAGE_SIZE - 1), hex, text, e.af, e.bc, e.de, e.hl, e.sp, e.ix, e.iy,
            e.af2, e.bc2, e.de2, e.hl2, e.i, e.r, e.im, (e.iffs & 1) ? '1' : '-', (e.iffs & 2) ? '2' : '-',
            e.lmpr, e.hmpr, e.vmpr,
            e.repeats > 1 ? fmt::format(" x{}", e.repeats) : "",
            label.empty() ? "" : " sym=" + label);
    }
}

const std::vector<Command> commands
{
    { "help", "?", "[command]", "Lists commands, or shows help for one", cmd_help },
    { "get-version", nullptr, "", "Shows the emulator version", cmd_version },
    { "noop", nullptr, "", "Does nothing", cmd_noop },
    { "quit", "exit", "", "Closes the connection", cmd_quit },
    { "exit-emulator", nullptr, "", "Exits SimCoupe", cmd_exit_emulator },

    { "get-registers", "gr", "", "Shows the CPU registers and SAM paging ports", cmd_get_registers },
    { "set-register", "sr", "reg=value ...", "Sets registers (PC, SP, AF ... IX, I, R, IM, IFF1, IFF2) or the LMPR, HMPR, VMPR and BORDER ports", cmd_set_register },
    { "enter-cpu-step", "pause", "", "Stops the emulation (cpu-step mode); an error while the window's debugger or a dialog is open", cmd_enter_step },
    { "exit-cpu-step", "resume", "", "Resumes the emulation", cmd_exit_step },
    { "cpu-step", "cs", "", "Runs one instruction, then shows the registers; an error while the window's debugger, a dialog or its pause holds the emulation", cmd_cpu_step },
    { "run", nullptr, "[instructions]", "Runs until a breakpoint fires, the client sends anything (or a new client does), or after the given number of instructions (0 = no limit); then stops. An error while the window's debugger, a dialog or its pause holds the emulation, and one opening ends it", cmd_run },
    { "run-frames", "rf", "[frames]", "Runs the given number of frames (default 1), then stops; a breakpoint, any input from a client, or the window's debugger, a dialog or its pause stops it early (an error if one is already open)", cmd_run_frames },
    { "reset-cpu", nullptr, "", "Resets the SAM", cmd_reset },
    { "hard-reset-cpu", nullptr, "", "Clears RAM and resets the SAM", cmd_hard_reset },
    { "generate-nmi", nullptr, "", "Generates an NMI", cmd_nmi },
    { "set-speed", nullptr, "[normal|turbo|percent]", "Sets or shows the emulation speed (turbo = unthrottled)", cmd_set_speed },
    { "get-tstates", nullptr, "", "Shows the T-state count within the current frame", cmd_get_tstates },
    { "disassemble", "d", "[address] [lines]", "Disassembles from the CPU's view of memory (default PC); address 0-FFFFH, up to 65536 lines", cmd_disassemble },

    { "read-memory", nullptr, "address [length]", "Reads memory as hex, from the current memory zone (the block must lie inside the zone; at most 16M)", cmd_read_memory },
    { "write-memory", nullptr, "address value [value ...]", "Writes bytes to the current memory zone", cmd_write_memory },
    { "write-memory-raw", nullptr, "address hexbytes", "Writes a hex string to the current memory zone", cmd_write_memory_raw },
    { "load-binary", nullptr, "file address [length]", "Loads a file into the current memory zone (length 0 = whole file)", cmd_load_binary },
    { "save-binary", nullptr, "file address length", "Saves memory from the current memory zone to a file", cmd_save_binary },
    { "get-memory-zones", "gmz", "", "Lists memory zones (-1 = CPU view; others are pages: RAM, EXT, ROM)", cmd_get_memory_zones },
    { "set-memory-zone", "smz", "zone", "Selects the memory zone used by the memory commands", cmd_set_memory_zone },
    { "get-paging-state", nullptr, "", "Shows the paging ports and the page in each 16K section", cmd_get_paging_state },

    { "send-keys-event", nullptr, "key 1|0", "Presses (1) or releases (0) a key: a SAM key name (shift, symbol, cntrl, return, up, f1, ...) or a decimal ASCII code", cmd_send_keys_event },
    { "send-keys-string", nullptr, "ms text", "Types text (spaces included, even if it is only spaces), holding each key for ms milliseconds (0-3600000, rounded to 20ms frames); not while the window's debugger, a dialog or its pause holds the emulation", cmd_send_keys_string },
    { "send-keys-ascii", nullptr, "ms code [code ...]", "Types decimal ASCII codes 0-255 (13 = Return); not while the window's debugger, a dialog or its pause holds the emulation", cmd_send_keys_ascii },
    { "release-keys", nullptr, "", "Releases all keys pressed with send-keys-event", cmd_release_keys },

    { "save-screen", nullptr, "file.png", "Saves the current frame as a PNG (works headless)", cmd_save_screen },
    { "get-screen-mode", nullptr, "", "Shows the screen mode, screen page and border colour", cmd_get_screen_mode },

    { "insert-disk", nullptr, "drive file [boot]", "Inserts a disk image into drive 1 or 2; boot also resets and boots it", cmd_insert_disk },
    { "eject-disk", nullptr, "[drive]", "Ejects the disk in drive 1 (default) or 2", cmd_eject_disk },

    { "enable-breakpoints", nullptr, "", "Turns breakpoint checking on", cmd_enable_breakpoints },
    { "disable-breakpoints", nullptr, "", "Turns breakpoint checking off", cmd_disable_breakpoints },
    { "set-breakpoint", "sb", "index [condition]", "Sets breakpoint 1-100 to a SimCoupe debugger expression (e.g. PC=8000H, A=3 and peek(5C00H)=1); no condition clears it. Fires when the condition becomes true", cmd_set_breakpoint },
    { "set-breakpointaction", "sba", "index [action]", "Sets the action: prints text, printe expr, printregs, putv expr, any other command, or empty/break/menu to stop (the default)", cmd_set_breakpoint_action },
    { "enable-breakpoint", "eb", "index", "Enables one breakpoint", cmd_enable_breakpoint },
    { "disable-breakpoint", "db", "index", "Disables one breakpoint", cmd_disable_breakpoint },
    { "get-breakpoints", "gb", "", "Lists breakpoints", cmd_get_breakpoints },
    { "set-membreakpoint", nullptr, "address|page:offset type [length]", "Stops on memory access: type 0 = clear, 1 = read, 2 = write, 3 = read/write. An address is in the CPU's view; page:offset (RAM05:0100, EXT00:3FFF, ROM0:0000, offset in hex) is physical, wherever the page is mapped", cmd_set_membreakpoint },
    { "clear-membreakpoints", nullptr, "", "Clears all memory breakpoints", cmd_clear_membreakpoints },
    { "set-portbreakpoint", nullptr, "port type [mask]", "Stops on port I/O: type 0 = clear, 1 = in, 2 = out, 3 = both; port is a number or a name (lmpr, hmpr, vmpr, border ...); a port matches when (port & mask) equals the given one (default mask FFH, or FFFFH for a port above FFH). Checked after enable-breakpoints", cmd_set_portbreakpoint },
    { "clear-portbreakpoints", nullptr, "", "Clears all port breakpoints", cmd_clear_portbreakpoints },
    { "cpu-history", nullptr, "enabled yes|no | clear", "Records the CPU state before each instruction in a ring of 10000 (off by default; it slows emulation while on); clear empties it. Read with get-cpu-history", cmd_cpu_history },
    { "get-cpu-history", nullptr, "[count]", "Shows the newest count (default 20, at most 10000) instructions that have run since recording was on, oldest first: address, physical page and offset, bytes, disassembly and the registers before it ran; each instruction that executes is recorded once, an instruction stopped before is not until it runs; a repeat at the same address (LDIR, JR $) is one line marked xN with the registers before its last run", cmd_get_cpu_history },
    { "io-log", nullptr, "enabled yes|no | filter [in|out] [port[:mask] ...] | get [count] | clear", "Logs I/O without stopping (frame, cycle, instruction address, direction, port, value); filter limits it to the ports and direction given (none = all); get shows the newest entries and the number dropped (at most 100000 are kept)", cmd_io_log },
    { "get-breakpoint-log", nullptr, "", "Shows the log written by breakpoint actions and stops", cmd_get_breakpoint_log },
    { "clear-breakpoint-log", nullptr, "", "Clears the breakpoint log", cmd_clear_breakpoint_log },
    { "prints", nullptr, "text", "Adds text to the log", cmd_prints },
    { "printe", nullptr, "expression", "Adds an expression and its value to the log", cmd_printe },
    { "putv", nullptr, "expression", "Adds the value of an expression to the log", cmd_putv },
    { "printregs", nullptr, "", "Adds the registers to the log", cmd_printregs },

    { "load-symbols", nullptr, "file [append]", "Loads labels from a pyz80 .sym, an sjasmplus --sym= file or --lstlab listing, or a hexaddress=name file, replacing the current ones (or adding with append). A bad file is an error naming the line and loads nothing. Labels are 16-bit CPU-view addresses (no page); then accepted by name (name, name+N) in breakpoint conditions, printe/putv, set-register and wherever a command takes an address, and shown in disassemble, registers, stops, io-log and coverage get symbols", cmd_load_symbols },
    { "clear-symbols", nullptr, "", "Removes all loaded labels", cmd_clear_symbols },
    { "get-symbols", nullptr, "[prefix]", "Lists loaded labels (address name), sorted by address, optionally only those starting with prefix (not case sensitive)", cmd_get_symbols },

    { "cpu-code-coverage", nullptr, "enabled yes|no | clear | get [symbols] | get-physical", "Records executed addresses; get lists them in the CPU's view (symbols adds the label of each, with loaded symbols), get-physical as page:offset", cmd_coverage },
};

void execute(const std::string& line, Request& req)
{
    auto text = line;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        text.pop_back();
    text.erase(0, std::min(text.find_first_not_of(" \t"), text.size()));
    if (trim(text).empty())
        return;

    auto space = text.find_first_of(" \t");
    auto name = to_lower(text.substr(0, space));
    auto rest_start = (space == std::string::npos) ? space : text.find_first_not_of(" \t", space);
    req.raw_rest = (rest_start == std::string::npos) ? "" : text.substr(rest_start);
    req.rest = trim(req.raw_rest);
    req.args = split(req.rest);

    for (auto& cmd : commands)
    {
        if (name == cmd.name || (cmd.alias && name == cmd.alias))
        {
            cmd.handler(req);
            return;
        }
    }

    req.out += fmt::format("Error. Unknown command: {}\n", name);
}

// Runs a breakpoint action; returns true if it should stop the emulation
bool run_action(const std::string& action)
{
    auto name = to_lower(trim(action));
    if (name.empty() || name == "break" || name == "menu")
        return true;

    Request req;
    req.in_action = true;
    execute(action, req);

    std::istringstream ss(req.out);
    for (std::string line; std::getline(ss, line); )
        Log(line);

    return stopped;
}

bool mem_breakpoint_hit(const uint8_t* phys, int type)
{
    if (!phys)
        return false;

    auto page = PtrPage(phys);
    auto offset = PtrOffset(phys);

    for (auto& mbp : mem_breakpoints)
    {
        if (!(mbp.type & type))
            continue;

        // The CPU address the page is mapped at, if any (the first one if it is mapped twice)
        int addr = -1;
        for (int section = 0; section < 4; ++section)
        {
            auto a = section * MEM_PAGE_SIZE + offset;
            if (AddrPage(static_cast<uint16_t>(a)) != page)
                continue;
            if (mbp.page < 0 && (a < mbp.from || a > mbp.to))
                continue;

            addr = a;
            break;
        }

        if (mbp.page < 0 ? (addr < 0) : (page != mbp.page || offset < mbp.from || offset > mbp.to))
            continue;

        stop_with(fmt::format("Memory breakpoint fired: {} {}:{:04X}H via {}, instruction at {:04X}H{}",
            (type == MEMBP_READ) ? "read" : "write", Memory::PageDesc(page, true), offset,
            (addr < 0) ? std::string("(unmapped)") : fmt::format("{:04X}H", addr), last_instr_pc, sym_suffix(last_instr_pc)));
        return true;
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////
// Socket handling

void close_client()
{
    if (client_socket != INVALID_SOCKET)
        close_socket(client_socket);

    client_socket = INVALID_SOCKET;
    in_buf.clear();
    pending.queued = 0;
    out_buf.clear();
    close_after_flush = false;
}

void flush()
{
    while (client_socket != INVALID_SOCKET && !out_buf.empty())
    {
        auto sent = ::send(client_socket, out_buf.data(), static_cast<int>(out_buf.size()), SEND_FLAGS);
        if (sent > 0)
            out_buf.erase(0, sent);
        else if (sent < 0 && would_block())
            return;
        else
        {
            close_client();
            return;
        }
    }

    if (close_after_flush)
        close_client();
}

void accept_client()
{
    auto s = accept(listen_socket, nullptr, nullptr);
    if (s == INVALID_SOCKET)
        return;

    // One client at a time: a new connection replaces the old one
    close_client();
    client_socket = s;
    ++client_id;

    set_nonblocking(client_socket);
    int on = 1;
    setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
    setsockopt(client_socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    send("Welcome to SimCoupe remote command protocol (ZRCP)\nWrite help for available commands\n\n");
    send(prompt());
}

void read_client()
{
    char buf[4096];
    auto len = recv(client_socket, buf, sizeof(buf), 0);
    if (len > 0)
        in_buf.append(buf, len);
    else if (len == 0 || !would_block())
        close_client();
}

// Input that arrives while run or run-frames is waiting stops the emulation, as ZEsarUX does,
// with a reply like a breakpoint stop; the input is then handled as usual. Commands sent
// together with the run (already queued) don't count. A disconnect doesn't stop the run, but
// the next client's input does (the reply then only goes to the log, as that client didn't
// send the run).
void check_interrupt()
{
    if ((pending.type != Wait::Frames && pending.type != Wait::Break) || in_buf.size() <= pending.queued)
        return;

    auto message = fmt::format("Interrupted by client input at PC={:04X}H{}", cpu.get_pc(), sym_suffix(cpu.get_pc()));
    if (pending.type == Wait::Frames)
        message += fmt::format(" after {} of {} frames", pending.total - pending.count, pending.total);

    stop_with(message);
}

// The window's GUI or pause took the emulation while a command waited for it (F10, the
// debugger key, one of the debugger's own breakpoints): end the wait with a reply saying so.
// The window owns the emulation from then on, and it runs again once the GUI closes or the
// pause ends.
void check_window()
{
    if (pending.type == Wait::None || (!GUI::IsActive() && !g_fPaused))
        return;

    auto by = Debug::IsActive() ? "debugger" : GUI::IsActive() ? "dialog" : "pause";
    auto message = fmt::format("Interrupted by the emulator's {} at PC={:04X}H{}", by, cpu.get_pc(), sym_suffix(cpu.get_pc()));
    if (pending.type == Wait::Frames)
        message += fmt::format(" after {} of {} frames", pending.total - pending.count, pending.total);

    Log(message);
    stopped = false;

    if (pending.type == Wait::Typing)
        stop_typing();

    complete(message + "\n");
}

void process_input()
{
    check_window();
    check_interrupt();

    while (pending.type == Wait::None && client_socket != INVALID_SOCKET && !close_after_flush)
    {
        auto eol = in_buf.find('\n');
        if (eol == std::string::npos)
            break;

        auto line = in_buf.substr(0, eol);
        in_buf.erase(0, eol + 1);

        Request req;
        execute(line, req);
        send(req.out);

        if (!req.deferred)
            send(prompt());
    }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

bool Init()
{
    Exit();

    if (!GetOption(remote))
        return true;

#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET)
    {
        Message(MsgType::Error, "Remote control: can't create socket");
        return false;
    }

    int on = 1;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(GetOption(remoteport)));

    if (bind(listen_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_socket, 1) != 0)
    {
        Message(MsgType::Error, "Remote control: can't listen on 127.0.0.1 port {}", GetOption(remoteport));
        close_socket(listen_socket);
        listen_socket = INVALID_SOCKET;
        return false;
    }

    set_nonblocking(listen_socket);
    fprintf(stderr, "Remote control listening on 127.0.0.1:%d\n", GetOption(remoteport));
    return true;
}

void Exit()
{
    flush();
    close_client();

    if (listen_socket != INVALID_SOCKET)
    {
        close_socket(listen_socket);
        listen_socket = INVALID_SOCKET;
#ifdef _WIN32
        WSACleanup();
#endif
    }
}

bool IsActive()
{
    return listen_socket != INVALID_SOCKET;
}

bool IsStopped()
{
    return stopped && pending.type == Wait::None;
}

void ShowStopped()
{
    Frame::SetStatus("Stopped by remote control");
    Frame::End();
}

void Poll()
{
    if (listen_socket == INVALID_SOCKET)
        return;

    // Sleep briefly in select() when the emulation isn't running, rather than spin
    bool idle = g_fPaused || IsStopped();
    timeval tv{ 0, idle ? 10000 : 0 };

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_socket, &read_fds);
    auto max_fd = listen_socket;
    if (client_socket != INVALID_SOCKET)
    {
        FD_SET(client_socket, &read_fds);
        max_fd = std::max(max_fd, client_socket);
    }

    if (select(static_cast<int>(max_fd + 1), &read_fds, nullptr, nullptr, &tv) > 0)
    {
        if (client_socket != INVALID_SOCKET && FD_ISSET(client_socket, &read_fds))
            read_client();

        if (FD_ISSET(listen_socket, &read_fds))
            accept_client();
    }

    process_input();
    flush();
}

bool OnInstruction()
{
    if (history_enabled)
        history_record();

    if (coverage_enabled)
    {
        auto offset = AddrReadPtr(cpu.get_pc()) - pMemory;
        coverage[offset >> 3] |= 1 << (offset & 7);
    }

    if (pending.type == Wait::Instructions || (pending.type == Wait::Break && pending.count))
    {
        if (--pending.count == 0)
        {
            stopped = true;
            complete((pending.type == Wait::Instructions) ? registers_string() + "\n" : "Stopped after the instruction limit\n");
            return true;
        }
    }

    if (!breakpoints_enabled)
        return false;

    bool stop = false;
    for (int i = 0; i < MAX_BREAKPOINTS && !stop; ++i)
    {
        auto& bp = breakpoints[i];
        if (!bp.enabled)
            continue;

        // Fire when the condition becomes true, as ZEsarUX does
        bool value = bp.expr.Eval() != 0;
        bool fired = value && !bp.last_value;
        bp.last_value = value;

        if (fired && run_action(bp.action))
        {
            stop_with(fmt::format("Breakpoint fired: {}{}", bp.condition, sym_suffix(cpu.get_pc())));
            stop = true;
        }
    }

    if (!mem_breakpoints.empty() && !stop)
    {
        stop = mem_breakpoint_hit(Memory::last_phys_write1, MEMBP_WRITE) ||
            mem_breakpoint_hit(Memory::last_phys_write2, MEMBP_WRITE) ||
            mem_breakpoint_hit(Memory::last_phys_read1, MEMBP_READ) ||
            mem_breakpoint_hit(Memory::last_phys_read2, MEMBP_READ);

        Memory::last_phys_read1 = Memory::last_phys_read2 = nullptr;
        Memory::last_phys_write1 = Memory::last_phys_write2 = nullptr;
    }

    last_instr_pc = cpu.get_pc();
    return stop;
}

void OnIO(uint16_t port, uint8_t value, bool is_out)
{
    // The CPU has fetched the whole instruction (and operand), so its start is two bytes back:
    // D3 n, DB n and the ED-prefixed forms, including a repeating block instruction
    auto pc = static_cast<uint16_t>(cpu.get_pc() - 2);

    if (io_log_enabled && (is_out ? io_filter_out : io_filter_in))
    {
        bool match = io_filters.empty();
        for (auto& f : io_filters)
            match |= (port & f.mask) == f.port;

        if (match)
        {
            if (io_log.size() >= MAX_IO_LOG)
            {
                io_log.pop_front();
                io_dropped++;
            }
            io_log.push_back({ frame_count, CPU::frame_cycles, pc, port, value, is_out });
        }
    }

    if (breakpoints_enabled)
    {
        for (auto& pbp : port_breakpoints)
        {
            if ((pbp.type & (is_out ? PORTBP_OUT : PORTBP_IN)) && (port & pbp.mask) == pbp.port)
            {
                stop_with(fmt::format("Port breakpoint fired: {} {:04X}H value {:02X}H, instruction at {:04X}H{}",
                    is_out ? "OUT" : "IN", port, value, pc, sym_suffix(pc)));
                g_fBreak = true;
                return;
            }
        }
    }
}

void OnFrameEnd()
{
    frame_count++;

    if (pending.type == Wait::Typing && --type_frames_left <= 0)
    {
        if (type_key_down)
        {
            typed_keys.fill(0);
            type_key_down = false;
            type_frames_left = type_frames;
        }
        else if (!type_queue.empty())
        {
            set_typed_char(type_queue.front());
            type_queue.pop_front();
            type_key_down = true;
            type_frames_left = type_frames;
        }
        else
        {
            complete("");
        }
    }
    else if (pending.type == Wait::Frames && --pending.count <= 0)
    {
        stopped = true;
        complete("");
    }
}

void ApplyKeys(std::array<uint8_t, 9>& key_matrix)
{
    for (size_t i = 0; i < key_matrix.size(); ++i)
        key_matrix[i] &= ~(held_keys[i] | typed_keys[i]);
}

void Log(const std::string& str)
{
    log_lines.push_back(str);
    if (log_lines.size() > MAX_LOG_LINES)
        log_lines.pop_front();
}

} // namespace Remote
