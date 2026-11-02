#ifndef __APP_PROJECT_FILE_H
#define __APP_PROJECT_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "app/Model.h"
#include "app/SampleLib.h"
#include "app/Services.h"

// One project = one file on the SD card: a small header (for the PROJECT screen's slot list) and the Project as it is in memory. The
// layout is pinned by static_asserts (host and ARM GCC lay it out the same); a change bumps kProjectVersion. A file is checked (magic,
// version, size, CRC) and every value is clamped into range before it replaces the project, so a damaged card cannot crash the sequencer.
namespace app {

constexpr uint16_t kProjectVersion = 9;   // 2: the samples by path after the project; 3: the lanes' MIDI; 4: the effects; 5: a part's preset as a 16-bit number; 6: seven parts; 7: slices; 8: the LOOP page and loop points; 9: GLOBAL's CLOCK means something (1-8 still load)

struct ProjectHeader {
    char magic[4];          // "DBPJ"
    uint16_t version;
    uint16_t headerSize;
    uint32_t payloadSize;   // sizeof(Project)
    uint32_t crc;           // CRC-32 of the payload (version 2: and of the sample table after it)
    char name[16];
    uint16_t bpm;
    uint8_t usedPatterns;
    uint8_t songs;
};

class ByteSink {
   public:
    virtual ~ByteSink() {}
    virtual bool write(const void* data, size_t len) = 0;
};
class ByteSource {
   public:
    virtual ~ByteSource() {}
    virtual bool read(void* data, size_t len) = 0;
};

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);
void makeHeader(const Project& pj, ProjectHeader& h);
// `lib`: the samples the project's numbers refer to (their paths go into the file); null writes no sample paths.
bool writeProject(const Project& pj, ByteSink& out, const SampleLib* lib = nullptr);
// Reads into `scratch` and only copies it to `out` when the file is good.
// `lib`: the samples on this card: a version 2 file's sample references are looked up by path and renumbered; `missing` counts the
// ones this card does not have (their lanes play sample 0).
bool readProject(ByteSource& in, Project& scratch, Project& out, const SampleLib* lib = nullptr, int* missing = nullptr);
bool readHeader(ByteSource& in, ProjectHeader& h);
void headerInfo(const ProjectHeader& h, SlotInfo& out);
// Clamp every field into its range (a file from a damaged card, or an older layout).
void sanitize(Project& pj);

}  // namespace app

#endif
