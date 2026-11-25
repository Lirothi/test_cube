#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Every command-line switch the program reads (architecture plan R3.2): its name, the value it takes, one line of help,
// and whether a Shipping build keeps it. `--help` prints this table; Shipping's filter (app/Shipping.cpp) keeps exactly
// the switches marked for it. tools/check_switches.py holds the table and the code together: a switch the code reads is
// in the table, and a table row the code no longer reads is a finding. Where each switch is parsed is still its own
// site (main.cpp, the VFS tools, the cook, the log, the memory gates) -- R3.2b moves the parsing into the table.
namespace boot
{
struct Switch
{
    const char* name;     // "--shot"
    const char* value;    // "=<path>", "[=<sec>]" or "" for a flag
    const char* help;     // one line
    bool shipping;        // a Shipping build keeps it (every other switch is dropped there, with a warning)
};

// The table, sorted by name.
const Switch* Switches(std::size_t& count);
// The row of a switch by its bare name ("--shot", without the value), or null.
const Switch* FindSwitch(std::string_view name);
// The help text: every switch, or only those a Shipping build keeps.
std::string HelpText(bool shippingOnly);
}
