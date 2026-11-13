// Part of SimCoupe - A SAM Coupe emulator
//
// Symbol.cpp: Symbol management
//
//  Copyright (c) 1999-2014 Simon Owen
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

#include "SimCoupe.h"
#include "Symbol.h"

#include "CPU.h"
#include "Memory.h"

namespace Symbol
{
static const int MAX_SYMBOL_OFFSET = 3;

typedef std::map <uint16_t, std::string> AddrToSym;
typedef std::map <std::string, uint16_t> SymToAddr;

static AddrToSym port_symbols;
static AddrToSym ram_symbols, rom_symbols, samdos2_symbols;
static SymToAddr symbol_values, rom_values, samdos2_values;

// Symbols loaded by the remote control (name, value), kept in load order. While any are loaded
// they are the only user symbols: ram_symbols and symbol_values are rebuilt from them, and the
// debugger's own symbol loading (Update) leaves them alone.
static std::vector<std::pair<std::string, uint16_t>> remote_list;
static bool remote_symbols;

// pyz80 symbol files use cPickler format
static void ReadcPickler(FILE* file, AddrToSym& symtab, SymToAddr* valtab_ptr)
{
    std::string name;

    char line[128]{};
    while (std::fgets(line, sizeof(line), file) != nullptr)
    {
        if (auto psz = std::strchr(line, '\''))
        {
            auto end_ptr = std::strchr(++psz, '\'');
            if (end_ptr)
            {
                *end_ptr = '\0';
                name = psz;
            }
        }
        // The symbol values are integers, with an I type marker
        else if (auto psz = std::strchr(line, 'I'); psz && !name.empty())
        {
            auto value = static_cast<uint16_t>(strtoul(psz + 1, nullptr, 0));
            symtab[value] = name;

            if (valtab_ptr)
                (*valtab_ptr)[tolower(name)] = value;

            name.clear();
        }
    }
}

// pyz80 map files use a simple addr=name format
static void ReadSimple(FILE* file_, AddrToSym& symtab, SymToAddr* valtab_ptr)
{
    char line[128]{};
    while (std::fgets(line, sizeof(line), file_) != nullptr)
    {
        if (auto psz = std::strtok(line, " ="))
        {
            if (*psz == ';')
                continue;

            auto addr = static_cast<uint16_t>(std::strtoul(psz, nullptr, 16));

            if ((psz = std::strtok(nullptr, " =")) != nullptr)
            {
                psz[strcspn(psz, "\r\n")] = '\0';
                symtab[addr] = psz;

                if (valtab_ptr && *psz)
                    (*valtab_ptr)[tolower(psz)] = addr;
            }
        }
    }
}

static bool Load(const std::string& path, AddrToSym& symtab, SymToAddr* valtab_ptr = nullptr)
{
    symtab.clear();
    if (valtab_ptr)
        valtab_ptr->clear();

    unique_FILE file = fopen(path.c_str(), "r");
    if (!file)
        return false;

    if (fgetc(file) == '(' || fgetc(file) == 'd')
        ReadcPickler(file, symtab, valtab_ptr);
    else if (fseek(file, 0, SEEK_SET) == 0)
        ReadSimple(file, symtab, valtab_ptr);
    else
        return false;

    return true;
}

static bool LoadComet(AddrToSym& symtab, SymToAddr* valtab_ptr = nullptr)
{
    if (!HasCometSymbols())
        return false;

    auto sym_ptr = PageReadPtr(0x1b) + 0x3fff;
    if (!*sym_ptr)
        return false;

    symtab.clear();
    if (valtab_ptr)
        valtab_ptr->clear();

    while (*sym_ptr)
    {
        auto len = *sym_ptr;
        auto name = std::string(sym_ptr - len, sym_ptr);
        std::reverse(name.begin(), name.end());
        sym_ptr -= static_cast<size_t>(1) + len;

        auto flags = sym_ptr[0];
        if (flags == 0xff)
        {
            auto value = (sym_ptr[-2] << 8) | sym_ptr[-1];
            symtab[value] = name;
            if (valtab_ptr)
                (*valtab_ptr)[tolower(name)] = value;
        }

        sym_ptr -= 3;
    }

    return true;
}

void Update(const std::string& path)
{
    if (port_symbols.empty())
        Load(OSD::MakeFilePath(PathType::Resource, "samports.map"), port_symbols);
    if (rom_symbols.empty())
        Load(OSD::MakeFilePath(PathType::Resource, "samrom.map"), rom_symbols, &rom_values);
    if (samdos2_symbols.empty())
        Load(OSD::MakeFilePath(PathType::Resource, "samdos2.map"), samdos2_symbols, &samdos2_values);
    if (!remote_symbols && !path.empty() && !Load(path, ram_symbols, &symbol_values))
        LoadComet(ram_symbols, &symbol_values);
}

////////////////////////////////////////////////////////////////////////////////
// Symbol files for the remote control. Unlike Load above these are parsed strictly: a bad
// line fails the whole file (nothing is loaded) and the error names the line.

static constexpr size_t MAX_REMOTE_FILE = 16 * 1024 * 1024;
static constexpr size_t MAX_REMOTE_SYMBOLS = 500000;

struct ParsedSymbol
{
    std::string name;
    int64_t value;
};

static bool ValidName(const std::string& name)
{
    if (name.empty() || std::isdigit(static_cast<uint8_t>(name[0])))
        return false;

    for (auto ch : name)
    {
        if (!std::isalnum(static_cast<uint8_t>(ch)) && !std::strchr("_.@?!#", ch))
            return false;
    }
    return true;
}

// Number as written by assemblers: 0x1F, $1F, 1Fh or decimal
static bool ParseValue(std::string s, int64_t& value)
{
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2), base = 16;
    else if (s.size() > 1 && s[0] == '$')
        s = s.substr(1), base = 16;
    else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H'))
        s.pop_back(), base = 16;

    if (s.empty() || s.size() > 18 || !std::isxdigit(static_cast<uint8_t>(s[0])))
        return false;

    char* end{};
    value = std::strtoll(s.c_str(), &end, base);
    return !*end;
}

static std::vector<std::string> Words(const std::string& line)
{
    std::vector<std::string> words;
    std::istringstream ss(line);
    for (std::string w; ss >> w; )
        words.push_back(w);
    return words;
}

static std::string StripLine(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    return line;
}

static std::string LineError(int line_no, const std::string& what)
{
    return fmt::format("line {}: {}", line_no, what);
}

// pyz80 --exportfile: a pickle (protocol 0) of a name to value dict. Python 3 writes the names
// as "V<name>" and Python 2 as "S'<name>'", each preceded by "s" after the first, then "p<n>"
// (memo) and "I<value>", closed by "."
static bool ParsePickle(const std::vector<std::string>& lines, std::vector<ParsedSymbol>& syms, std::string& error)
{
    std::string name;
    bool have_name = false, ended = false;

    for (size_t i = 0; i < lines.size(); i++)
    {
        auto line_no = static_cast<int>(i) + 1;
        auto line = lines[i];
        if (line.empty())
            continue;

        if (ended)
        {
            error = LineError(line_no, "text after the pickle's end marker");
            return false;
        }

        if (i == 0)
        {
            if (line.compare(0, 2, "(d") != 0)
            {
                error = LineError(line_no, "not a pickle dictionary (expected \"(dp0\")");
                return false;
            }
            continue;
        }

        if (line[0] == 's' && line.size() > 1)
            line.erase(0, 1);

        if (line == ".")
        {
            if (have_name)
            {
                error = LineError(line_no, "name without a value: " + name);
                return false;
            }
            ended = true;
            continue;
        }

        if (line[0] == 'V' || (line[0] == 'S' && line.size() > 1 && line[1] == '\''))
        {
            if (have_name)
            {
                error = LineError(line_no, "name without a value: " + name);
                return false;
            }

            if (line[0] == 'V')
                name = line.substr(1);
            else if (line.size() >= 3 && line.back() == '\'')
                name = line.substr(2, line.size() - 3);
            else
            {
                error = LineError(line_no, "bad name");
                return false;
            }

            if (!ValidName(name))
            {
                error = LineError(line_no, "invalid symbol name: " + name);
                return false;
            }
            have_name = true;
        }
        else if (line[0] == 'p')
        {
            // memo number, nothing to do
            if (line.size() < 2 || line.find_first_not_of("0123456789", 1) != std::string::npos)
            {
                error = LineError(line_no, "bad memo line: " + line);
                return false;
            }
        }
        else if (line[0] == 'I' || line[0] == 'L')
        {
            auto text = line.substr(1);
            if (line[0] == 'L' && !text.empty() && text.back() == 'L')
                text.pop_back();

            char* end{};
            auto value = std::strtoll(text.c_str(), &end, 10);
            if (!have_name || text.empty() || *end || text.size() > 18)
            {
                error = LineError(line_no, have_name ? "bad value: " + line : "value without a name");
                return false;
            }

            syms.push_back({ name, value });
            have_name = false;
        }
        else
        {
            error = LineError(line_no, "unrecognised pickle line: " + line);
            return false;
        }

        if (syms.size() > MAX_REMOTE_SYMBOLS)
        {
            error = "too many symbols";
            return false;
        }
    }

    if (!ended || have_name)
    {
        error = LineError(static_cast<int>(lines.size()), "file ends early (no pickle end marker \".\")");
        return false;
    }
    return true;
}

// sjasmplus --sym=file: one "name: EQU 0x0000ABCD" line per label (the value is the CPU-view
// address, with no page); ";" starts a comment
static bool ParseSjasmSym(const std::vector<std::string>& lines, std::vector<ParsedSymbol>& syms, std::string& error)
{
    for (size_t i = 0; i < lines.size(); i++)
    {
        auto line = lines[i];
        if (auto semi = line.find(';'); semi != std::string::npos)
            line.erase(semi);

        auto words = Words(line);
        if (words.empty())
            continue;

        auto line_no = static_cast<int>(i) + 1;
        auto name = words[0];
        if (!name.empty() && name.back() == ':')
            name.pop_back();

        int64_t value = 0;
        if (words.size() != 3 || tolower(words[1]) != "equ" || !ParseValue(words[2], value))
        {
            error = LineError(line_no, "expected \"name: EQU value\"");
            return false;
        }
        if (!ValidName(name))
        {
            error = LineError(line_no, "invalid symbol name: " + name);
            return false;
        }

        syms.push_back({ name, value });
        if (syms.size() > MAX_REMOTE_SYMBOLS)
        {
            error = "too many symbols";
            return false;
        }
    }
    return true;
}

// The label table at the end of an sjasmplus listing made with --lstlab: "Value Label" header,
// a rule, then "0x8008   name" lines, with an "X" before the name for an unused label
static bool ParseSjasmListing(const std::vector<std::string>& lines, size_t header, std::vector<ParsedSymbol>& syms, std::string& error)
{
    for (size_t i = header + 2; i < lines.size(); i++)
    {
        auto words = Words(lines[i]);
        if (words.empty())
            continue;

        auto line_no = static_cast<int>(i) + 1;
        int64_t value = 0;
        if (words.size() == 3 && words[1] == "X")
            words.erase(words.begin() + 1);

        if (words.size() != 2 || words[0].compare(0, 2, "0x") != 0 || !ParseValue(words[0], value) || !ValidName(words[1]))
        {
            error = LineError(line_no, "expected \"0xVALUE [X] name\" in the label table");
            return false;
        }

        syms.push_back({ words[1], value });
        if (syms.size() > MAX_REMOTE_SYMBOLS)
        {
            error = "too many symbols";
            return false;
        }
    }
    return true;
}

// "8000=name" with the address in hex (the format the debugger's .map files use)
static bool ParseSimple(const std::vector<std::string>& lines, std::vector<ParsedSymbol>& syms, std::string& error)
{
    for (size_t i = 0; i < lines.size(); i++)
    {
        auto line = lines[i];
        if (line.empty() || line[0] == ';')
            continue;
        if (Words(line).empty())
            continue;

        auto line_no = static_cast<int>(i) + 1;
        auto eq = line.find('=');
        auto addr_text = (eq == std::string::npos) ? "" : line.substr(0, eq);
        auto name = (eq == std::string::npos) ? "" : line.substr(eq + 1);

        int64_t value = 0;
        if (eq == std::string::npos || addr_text.empty() || addr_text.size() > 4 ||
            addr_text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
            !ParseValue("0x" + addr_text, value))
        {
            error = LineError(line_no, "expected \"hexaddress=name\"");
            return false;
        }
        if (!ValidName(name))
        {
            error = LineError(line_no, "invalid symbol name: " + name);
            return false;
        }

        syms.push_back({ name, value });
        if (syms.size() > MAX_REMOTE_SYMBOLS)
        {
            error = "too many symbols";
            return false;
        }
    }
    return true;
}

static void RebuildRemote()
{
    ram_symbols.clear();
    symbol_values.clear();

    auto sorted = remote_list;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {
        return a.second != b.second ? a.second < b.second : tolower(a.first) < tolower(b.first); });

    // Where several labels share an address the first by name is the one shown
    for (auto& [name, value] : sorted)
    {
        ram_symbols.emplace(value, name);
        symbol_values[tolower(name)] = value;
    }
}

bool LoadFile(const std::string& path, bool append, std::string& error, size_t& loaded, size_t& skipped)
{
    loaded = skipped = 0;

    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec)
    {
        error = "can't open " + path;
        return false;
    }
    if (size > MAX_REMOTE_FILE)
    {
        error = "file is too big";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "can't open " + path;
        return false;
    }

    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line); )
        lines.push_back(StripLine(line));

    // Find the first line with something on it to tell the format
    size_t first = 0;
    while (first < lines.size() && (Words(lines[first]).empty() || lines[first][0] == ';'))
        first++;
    if (first == lines.size())
    {
        error = "file is empty";
        return false;
    }

    std::vector<ParsedSymbol> syms;
    size_t header = lines.size();
    for (size_t i = 0; i + 1 < lines.size(); i++)
    {
        auto words = Words(lines[i]);
        if (words.size() == 2 && words[0] == "Value" && words[1] == "Label" && lines[i + 1].compare(0, 3, "---") == 0)
        {
            header = i;
            break;
        }
    }

    bool ok;
    if (lines[0].compare(0, 2, "(d") == 0)
        ok = ParsePickle(lines, syms, error);
    else if (header < lines.size())
        ok = ParseSjasmListing(lines, header, syms, error);
    else if (Words(lines[first]).size() == 3 && tolower(Words(lines[first])[1]) == "equ")
        ok = ParseSjasmSym(lines, syms, error);
    else if (lines[first].find('=') != std::string::npos)
        ok = ParseSimple(lines, syms, error);
    else
    {
        error = LineError(static_cast<int>(first) + 1, "unrecognised symbol file format");
        ok = false;
    }

    if (!ok)
        return false;

    // Values that can't be a 16-bit address (large constants) are not usable as labels
    std::vector<std::pair<std::string, uint16_t>> accepted;
    for (auto& sym : syms)
    {
        if (sym.value < 0 || sym.value > 0xffff)
            skipped++;
        else
            accepted.push_back({ sym.name, static_cast<uint16_t>(sym.value) });
    }
    loaded = accepted.size();

    if (!append)
        remote_list.clear();

    // A name loaded again replaces the old one (names are not case sensitive)
    for (auto& [name, value] : accepted)
    {
        auto it = std::find_if(remote_list.begin(), remote_list.end(), [&](auto& e) { return tolower(e.first) == tolower(name); });
        if (it != remote_list.end())
            *it = { name, value };
        else
            remote_list.push_back({ name, value });
    }

    remote_symbols = true;
    RebuildRemote();
    return true;
}

void ClearRemote()
{
    remote_list.clear();
    remote_symbols = false;
    ram_symbols.clear();
    symbol_values.clear();
}

bool HasRemote()
{
    return remote_symbols && !remote_list.empty();
}

size_t RemoteCount()
{
    return remote_list.size();
}

std::vector<std::pair<uint16_t, std::string>> ListRemote(const std::string& prefix)
{
    std::vector<std::pair<uint16_t, std::string>> list;
    auto lprefix = tolower(prefix);

    for (auto& [name, value] : remote_list)
    {
        if (tolower(name).compare(0, lprefix.size(), lprefix) == 0)
            list.push_back({ value, name });
    }

    std::sort(list.begin(), list.end(), [](auto& a, auto& b) {
        return a.first != b.first ? a.first < b.first : tolower(a.second) < tolower(b.second); });
    return list;
}

std::optional<int> FindRemote(const std::string& name)
{
    if (!HasRemote())
        return std::nullopt;

    if (auto it = symbol_values.find(tolower(name)); it != symbol_values.end())
        return static_cast<int>(it->second);
    return std::nullopt;
}

std::string NameRemote(uint16_t addr, int max_offset)
{
    if (!HasRemote())
        return "";

    auto it = ram_symbols.upper_bound(addr);
    if (it == ram_symbols.begin())
        return "";

    --it;
    auto offset = addr - it->first;
    if (offset > max_offset)
        return "";

    return offset ? fmt::format("{}+0x{:X}", it->second, offset) : it->second;
}

std::optional<int> LookupSymbol(const std::string& symbol)
{
    if (auto it = symbol_values.find(tolower(symbol)); it != symbol_values.end())
        return static_cast<int>((*it).second);
    if (auto it = rom_values.find(tolower(symbol)); it != rom_values.end())
        return static_cast<int>((*it).second);
    if (auto it = samdos2_values.find(tolower(symbol)); it != samdos2_values.end())
        return static_cast<int>((*it).second);

    return std::nullopt;
}

std::string LookupAddr(uint16_t addr, uint16_t lookup_context, int max_len, bool allow_offset)
{
    bool is_rom_addr = (addr < 0x4000 && AddrPage(addr) == ROM0) || (addr >= 0xc000 && AddrPage(addr) == ROM1);
    bool rom_context = AddrPage(lookup_context) == ROM0 || AddrPage(lookup_context) == ROM1;

    bool is_sysvars_addr = addr >= 0x4000 && addr < 0x5d00 && AddrPage(addr) == 0;

    bool samdos2_paged = (read_byte(0x4096) == 0x20) &&
        std::string_view(reinterpret_cast<const char*>(AddrReadPtr(0x50af)), 6) == "SAMDOS";
    bool is_samdos2_addr = samdos2_paged && addr >= 0x4000 && addr < 0x8000;
    bool samdos2_context = samdos2_paged && lookup_context >= 0x4000 && lookup_context < 0x8000;

    const auto& symtab =
        remote_symbols ? ram_symbols :
        (is_samdos2_addr && samdos2_context) ? samdos2_symbols :
        (is_rom_addr && rom_context) ? rom_symbols :
        (is_sysvars_addr) ? rom_symbols :
        ram_symbols;

    auto it = symtab.find(addr);
    if (it == symtab.end() && allow_offset)
    {
        for (int i = 1; i <= MAX_SYMBOL_OFFSET; i++)
        {
            it = symtab.find(addr - i);
            if (it != symtab.end())
                break;
        }

        // Allow space for +N offset
        max_len -= 2;
    }

    if (it != symtab.end())
    {
        auto [base_addr, name] = *it;
        auto symbol = name;

        if (max_len > 0)
            symbol = symbol.substr(0, max_len);

        if (base_addr != addr)
        {
            symbol += '+';
            symbol += '0' + (addr - base_addr);
        }

        return symbol;
    }

    return "";
}

std::string LookupPort(uint8_t port, bool input_port)
{
    uint16_t port_entry = input_port ? port : (0x8000 | port);

    auto it = port_symbols.find(port_entry);
    if (it == port_symbols.end() && !input_port)
        it = port_symbols.find(port);

    return (it != port_symbols.end()) ? (*it).second : "";
}

bool HasCometSymbols()
{
    auto mem = std::string(PageReadPtr(0x1c) + 0x1000, PageReadPtr(0x1c) + 0x1200);
    auto sym_ptr = PageReadPtr(0x1b) + 0x3fff;
    return mem.find("COMET Z80 assembler") != mem.npos && *sym_ptr;
}

bool SaveComet(const std::string& path)
{
    AddrToSym symtab;
    if (!LoadComet(symtab))
        return false;

    try
    {
        std::ofstream ofs(path, std::ofstream::out);

        for (auto& sym : symtab)
        {
            auto [addr, name] = sym;
            ofs << fmt::format("{:04X}={}", addr, name) << std::endl;
        }
    }
    catch (...)
    {
        TRACE("Failed to write Comet symbols\n");
        return false;
    }

    return true;
}

} // namespace Symbol
