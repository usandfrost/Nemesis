#ifndef __BOARD_CARD_SOAK_H
#define __BOARD_CARD_SOAK_H

#include <stdint.h>

#include "app/Model.h"
#include "app/SampleLib.h"

// The card under load, on the board, for a test card only: tools/board_script.py starts it through the debug mailbox. Each round writes a file in a size and at a
// buffer alignment picked at random, reads it back, and reads back another from earlier; now and then a project is saved and loaded, a preset saved over, a file
// removed, and the whole file system checked again. It stops at the first thing that is wrong and leaves the card as it is, to be looked at. Writes to
// /CATHODE/SOAK, project slot 8 and a preset called SOAK TEST. Debug builds only.
namespace board {

enum class SoakError : uint32_t { None, Refused, Open, Write, Close, ReadBack, Mismatch, EarlierFile, ProjectSave, ProjectLoad, ProjectDiffers, PresetSave, Remove, CardCheck };

struct SoakStatus {            // the mailbox's soak words, in this order
    uint32_t running, rounds, target, fileWrites, fileReads, projectSaves, presetSaves, cardChecks, megabytes, error, errorRound, errorDetail;
};
constexpr int kSoakWords = sizeof(SoakStatus) / sizeof(uint32_t);

extern SoakStatus soakStatus;

void soakStart(uint32_t rounds);
// Main loop, once a pass: one round if one is due. `scratch` is a project's worth of room the load may use.
void soakStep(const app::Project& pj, app::Project& scratch, const app::SampleLib& lib);

}  // namespace board

#endif
