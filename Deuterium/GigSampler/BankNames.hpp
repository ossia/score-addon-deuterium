#pragma once
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Deuterium::Gig
{
// Which parser libgig would be given for the file
enum class BankKind
{
  Gig,
  Dls,
  Sf2
};

struct BankEntryName
{
  // The instrument's (gig, dls) or preset's (sf2) name, bytes as stored
  std::string name;
  // DLS only: the raw MIDI locale of the instrument's 'insh' chunk
  uint32_t dlsBank{};
  uint32_t dlsProgram{};
};

// Reads the instrument / preset names of a bank straight from the RIFF chunks
// that hold them, without building libgig's object tree: listing a library of
// thousands of banks spends its time there otherwise.
//
// The result is what enumerating the bank through libgig gives, or nothing
// when this reader cannot vouch for that: libgig rejects a whole file when
// any object it builds is malformed, so everything it would read on the way
// is checked here, and a file that is not laid out exactly as expected is
// left to libgig itself. `whyNot` then receives what was unexpected.
std::optional<std::vector<BankEntryName>> readBankNames(
    const QString& filePath, BankKind kind, const char** whyNot = nullptr);
}
