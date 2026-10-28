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

#include "SimCoupe.h"
#include "Remote.h"

#include "CPU.h"
#include "Disassem.h"
#include "Expr.h"
#include "Frame.h"
#include "Keyboard.h"
#include "Memory.h"
#include "Options.h"
#include "SAMIO.h"
#include "SavePNG.h"

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
constexpr int MS_PER_FRAME = 20;

namespace Remote
{
bool trace_instructions;

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
    int owner = 0;          // client connection that should get the reply
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
};
std::vector<MemBreakpoint> mem_breakpoints;

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

    char* end{};
    auto value = std::strtol(s.c_str(), &end, base);
    if (*end)
        return std::nullopt;

    return static_cast<int>(negative ? -value : value);
}

std::string flags_string(uint8_t f)
{
    static const char flag_names[] = "SZ5H3PNC";
    std::string s;
    for (int i = 0; i < 8; ++i)
        s += (f & (0x80 >> i)) ? flag_names[i] : ' ';
    return s;
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
        io.lmpr, io.hmpr, io.vmpr);
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

bool zone_range_ok(int addr, int len, std::string& error)
{
    if (addr < 0 || len < 0 || (memory_zone != -1 && addr + len > zone_size(memory_zone)))
    {
        error = "Error. Address out of range for this memory zone\n";
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

    if (auto code = parse_number(str))
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

    trace_instructions = coverage_enabled || any_breakpoints ||
        (breakpoints_enabled && !mem_breakpoints.empty()) ||
        pending.type == Wait::Instructions || (pending.type == Wait::Break && pending.count);
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

bool can_defer(Request& req)
{
    if (req.in_action)
    {
        req.out += "Error. Not allowed in a breakpoint action\n";
        return false;
    }
    return true;
}

void defer(Request& req, Wait type, int count)
{
    pending.type = type;
    pending.count = count;
    pending.owner = client_id;
    req.deferred = true;
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
    static const std::map<std::string, uint8_t> ports
    {
        {"lmpr", LMPR_PORT}, {"hmpr", HMPR_PORT}, {"vmpr", VMPR_PORT}, {"border", BORDER_PORT},
    };

    for (auto& arg : req.args)
    {
        auto eq = arg.find('=');
        auto name = to_lower(arg.substr(0, eq));
        auto value = (eq == std::string::npos) ? std::nullopt : parse_number(arg.substr(eq + 1));
        if (!value)
        {
            req.out += fmt::format("Error. Expected register=value: {}\n", arg);
            return;
        }

        if (auto it = regs.find(name); it != regs.end())
            Expr::SetReg(it->second, *value);
        else if (auto port = ports.find(name); port != ports.end())
            IO::Out(port->second, static_cast<uint8_t>(*value));
        else
        {
            req.out += fmt::format("Error. Unknown register: {}\n", name);
            return;
        }
    }

    req.out += registers_string() + "\n";
}

void cmd_enter_step(Request& req)
{
    stopped = true;
    if (!req.in_action)
        Log("Entered cpu-step mode");
}

void cmd_exit_step(Request&)
{
    stopped = false;
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
    if (!req.args.empty() && !get_number(req, 0, limit))
        return;

    stopped = false;
    defer(req, Wait::Break, std::max(limit, 0));
}

void cmd_run_frames(Request& req)
{
    if (!can_defer(req))
        return;

    int frames = 1;
    if (!req.args.empty() && !get_number(req, 0, frames))
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
    if (!req.args.empty() && !get_number(req, 0, addr))
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
    if (!need_args(req, 2) || !get_number(req, 0, addr))
        return;

    std::vector<uint8_t> data;
    for (size_t i = 1; i < req.args.size(); ++i)
    {
        int value;
        if (!get_number(req, i, value))
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
    if (!need_args(req, 2) || !get_number(req, 0, addr))
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
    if (!need_args(req, 2) || !get_number(req, 1, addr))
        return;
    if (req.args.size() > 2 && !get_number(req, 2, len))
        return;

    std::ifstream file(req.args[0], std::ios::binary);
    if (!file)
    {
        req.out += fmt::format("Error. Can't open {}\n", req.args[0]);
        return;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (len > 0 && len < static_cast<int>(data.size()))
        data.resize(len);

    if (!zone_range_ok(addr, static_cast<int>(data.size()), req.out))
        return;

    for (size_t i = 0; i < data.size(); ++i)
        zone_write(addr + static_cast<int>(i), data[i]);

    req.out += fmt::format("Loaded {} bytes\n", data.size());
}

void cmd_save_binary(Request& req)
{
    int addr = 0, len = 0;
    if (!need_args(req, 3) || !get_number(req, 1, addr) || !get_number(req, 2, len))
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

void cmd_disassemble(Request& req)
{
    int addr = cpu.get_pc(), lines = 1;
    if (!req.args.empty() && !get_number(req, 0, addr))
        return;
    if (req.args.size() > 1 && !get_number(req, 1, lines))
        return;

    for (int i = 0; i < lines; ++i)
    {
        auto pc = static_cast<uint16_t>(addr);
        std::array<uint8_t, 4> bytes{};
        for (int j = 0; j < 4; ++j)
            bytes[j] = read_byte(static_cast<uint16_t>(pc + j));

        char text[64]{};
        auto len = Disassemble(bytes.data(), pc, text, sizeof(text));

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
    int ms = 0;
    if (!need_args(req, 2) || !get_number(req, 0, ms))
        return;

    // The text is the rest of the line after the delay, spaces included
    auto text = req.rest.substr(req.rest.find(req.args[0]) + req.args[0].size() + 1);
    start_typing(req, ms, std::vector<int>(text.begin(), text.end()));
}

void cmd_send_keys_ascii(Request& req)
{
    int ms = 0;
    if (!need_args(req, 2) || !get_number(req, 0, ms))
        return;

    std::vector<int> chars;
    for (size_t i = 1; i < req.args.size(); ++i)
    {
        int code;
        if (!get_number(req, i, code))
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
            req.out += fmt::format("Error. Invalid condition: {}\n", condition);
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
        req.out += fmt::format("Memory {:04X}H-{:04X}H: {}\n", mbp.from, mbp.to, types[mbp.type]);
    }
}

void cmd_set_membreakpoint(Request& req)
{
    int addr = 0, type = 0, len = 1;
    if (!need_args(req, 2) || !get_number(req, 0, addr) || !get_number(req, 1, type))
        return;
    if (req.args.size() > 2 && !get_number(req, 2, len))
        return;

    if (type < MEMBP_NONE || type > MEMBP_READWRITE || len < 1 || addr < 0 || addr > 0xffff)
    {
        req.out += "Error. Usage: set-membreakpoint address type(0-3) [length]\n";
        return;
    }

    auto to = std::min(addr + len - 1, 0xffff);

    // Replace any overlapping ranges
    mem_breakpoints.erase(std::remove_if(mem_breakpoints.begin(), mem_breakpoints.end(),
        [&](auto& mbp) { return mbp.from <= to && mbp.to >= addr; }), mem_breakpoints.end());

    if (type != MEMBP_NONE)
        mem_breakpoints.push_back({ addr, to, type });

    update_trace();
}

void cmd_clear_membreakpoints(Request&)
{
    mem_breakpoints.clear();
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
                s += fmt::format("{:04X} ", addr);
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

const std::vector<Command> commands
{
    { "help", "?", "[command]", "Lists commands, or shows help for one", cmd_help },
    { "get-version", nullptr, "", "Shows the emulator version", cmd_version },
    { "noop", nullptr, "", "Does nothing", cmd_noop },
    { "quit", "exit", "", "Closes the connection", cmd_quit },
    { "exit-emulator", nullptr, "", "Exits SimCoupe", cmd_exit_emulator },

    { "get-registers", "gr", "", "Shows the CPU registers and SAM paging ports", cmd_get_registers },
    { "set-register", "sr", "reg=value ...", "Sets registers (PC, SP, AF ... IX, I, R, IM, IFF1, IFF2) or the LMPR, HMPR, VMPR and BORDER ports", cmd_set_register },
    { "enter-cpu-step", "pause", "", "Stops the emulation (cpu-step mode)", cmd_enter_step },
    { "exit-cpu-step", "resume", "", "Resumes the emulation", cmd_exit_step },
    { "cpu-step", "cs", "", "Runs one instruction, then shows the registers", cmd_cpu_step },
    { "run", nullptr, "[instructions]", "Runs until a breakpoint fires (or after the given number of instructions), then stops", cmd_run },
    { "run-frames", "rf", "[frames]", "Runs the given number of frames (default 1), then stops", cmd_run_frames },
    { "reset-cpu", nullptr, "", "Resets the SAM", cmd_reset },
    { "hard-reset-cpu", nullptr, "", "Clears RAM and resets the SAM", cmd_hard_reset },
    { "generate-nmi", nullptr, "", "Generates an NMI", cmd_nmi },
    { "set-speed", nullptr, "[normal|turbo|percent]", "Sets or shows the emulation speed (turbo = unthrottled)", cmd_set_speed },
    { "get-tstates", nullptr, "", "Shows the T-state count within the current frame", cmd_get_tstates },
    { "disassemble", "d", "[address] [lines]", "Disassembles from the CPU's view of memory (default PC)", cmd_disassemble },

    { "read-memory", nullptr, "address [length]", "Reads memory as hex, from the current memory zone", cmd_read_memory },
    { "write-memory", nullptr, "address value [value ...]", "Writes bytes to the current memory zone", cmd_write_memory },
    { "write-memory-raw", nullptr, "address hexbytes", "Writes a hex string to the current memory zone", cmd_write_memory_raw },
    { "load-binary", nullptr, "file address [length]", "Loads a file into the current memory zone (length 0 = whole file)", cmd_load_binary },
    { "save-binary", nullptr, "file address length", "Saves memory from the current memory zone to a file", cmd_save_binary },
    { "get-memory-zones", "gmz", "", "Lists memory zones (-1 = CPU view; others are pages: RAM, EXT, ROM)", cmd_get_memory_zones },
    { "set-memory-zone", "smz", "zone", "Selects the memory zone used by the memory commands", cmd_set_memory_zone },
    { "get-paging-state", nullptr, "", "Shows the paging ports and the page in each 16K section", cmd_get_paging_state },

    { "send-keys-event", nullptr, "key 1|0", "Presses (1) or releases (0) a key: a SAM key name (shift, symbol, cntrl, return, up, f1, ...) or a decimal ASCII code", cmd_send_keys_event },
    { "send-keys-string", nullptr, "ms text", "Types text, holding each key for ms milliseconds (rounded to 20ms frames)", cmd_send_keys_string },
    { "send-keys-ascii", nullptr, "ms code [code ...]", "Types decimal ASCII codes (13 = Return)", cmd_send_keys_ascii },
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
    { "set-membreakpoint", nullptr, "address type [length]", "Stops on memory access in the CPU's view: type 0 = clear, 1 = read, 2 = write, 3 = read/write", cmd_set_membreakpoint },
    { "clear-membreakpoints", nullptr, "", "Clears all memory breakpoints", cmd_clear_membreakpoints },
    { "get-breakpoint-log", nullptr, "", "Shows the log written by breakpoint actions and stops", cmd_get_breakpoint_log },
    { "clear-breakpoint-log", nullptr, "", "Clears the breakpoint log", cmd_clear_breakpoint_log },
    { "prints", nullptr, "text", "Adds text to the log", cmd_prints },
    { "printe", nullptr, "expression", "Adds an expression and its value to the log", cmd_printe },
    { "putv", nullptr, "expression", "Adds the value of an expression to the log", cmd_putv },
    { "printregs", nullptr, "", "Adds the registers to the log", cmd_printregs },

    { "cpu-code-coverage", nullptr, "enabled yes|no | clear | get | get-physical", "Records executed addresses; get lists them in the CPU's view, get-physical as page:offset", cmd_coverage },
};

void execute(const std::string& line, Request& req)
{
    auto text = trim(line);
    if (text.empty())
        return;

    auto space = text.find_first_of(" \t");
    auto name = to_lower(text.substr(0, space));
    req.rest = (space == std::string::npos) ? "" : trim(text.substr(space));
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

    for (int section = 0; section < 4; ++section)
    {
        auto addr = section * MEM_PAGE_SIZE + PtrOffset(phys);
        if (AddrPage(static_cast<uint16_t>(addr)) != PtrPage(phys))
            continue;

        for (auto& mbp : mem_breakpoints)
        {
            if ((mbp.type & type) && addr >= mbp.from && addr <= mbp.to)
            {
                stop_with(fmt::format("Memory breakpoint fired: {} {:04X}H",
                    (type == MEMBP_READ) ? "read" : "write", addr));
                return true;
            }
        }
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

void process_input()
{
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
            stop_with(fmt::format("Breakpoint fired: {}", bp.condition));
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

    return stop;
}

void OnFrameEnd()
{
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
