#include "board/DebugHook.h"

#include "board/CardSoak.h"

#include "midi/MidiIn.h"

#if DBOY_DEBUG_HOOK

namespace board {

// 38 KB: it lives in DTCM (the same section as the LCD's strip buffer), which has room; AXI RAM does not.
__attribute__((section(".dtcm_bss"))) alignas(8) DebugMailbox debugMail;

static_assert(offsetof(DebugMailbox, ev) == 4 * 4, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, loops) == 4 * 36, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, toastOn) == 4 * 53, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, toast) == 4 * 54, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, capRequest) == 4 * 78, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, voices) == 4 * 80, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, cfgVoices) == 4 * 101, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, partPreset) == 4 * 108, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, waveSlots) == 4 * 115, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, isrBlocks) == 4 * 118, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, isrVoices) == 4 * 129, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, soak) == 4 * 136, "tools/board_script.py reads the mailbox by word index");
static_assert(offsetof(DebugMailbox, cap) == 4 * 148, "tools/board_script.py reads the mailbox by word index");
static_assert(sizeof(DebugMailbox::soak) == sizeof(SoakStatus), "the mailbox holds the soak status");

void debugHookInit() {
    DebugMailbox& m = debugMail;
    m.head = m.tail = 0;
    m.capRequest = m.capDone = 0;
    m.version = kDebugVersion;
    m.magic = kDebugMagic;   // last: the script waits for it
}

static void apply(app::Ui& ui, DebugGovernorFn setGovernor, DebugLeanFn setLean, uint32_t w) {
    const uint8_t type = (uint8_t)(w & 0xFF), a = (uint8_t)((w >> 8) & 0xFF);
    const int b = (int16_t)(w >> 16);
    switch (type) {
        case DbgKeyDown: if (a < (uint8_t)app::Key::Count) ui.keyDown((app::Key)a); break;
        case DbgKeyUp: if (a < (uint8_t)app::Key::Count) ui.keyUp((app::Key)a); break;
        case DbgLayerDown: if (a < 10) ui.layerDown(a); break;
        case DbgLayerUp: if (a < 10) ui.layerUp(a); break;
        case DbgTurn: if (a < 8 && b != 0) ui.turn(a, b); break;
        case DbgMuteLane: if (a < app::kLanes) ui.pattern().lanes[a].mute = b != 0; break;
        case DbgSoloPart:
            for (int l = 0; l < app::kLanes; ++l) ui.pattern().lanes[l].mute = a != 255 && ui.pattern().lanes[l].part != a;
            break;
        case DbgLoadPreset: ui.debugLoadPreset(a, b); break;
        case DbgMidiNote: {
            midi::Event e;
            e.type = b > 0 ? midi::Type::NoteOn : midi::Type::NoteOff;
            e.channel = 15;
            e.data1 = a;
            e.data2 = (uint8_t)(b > 0 ? b : 0);
            midi::rxQueue.push(e);   // (the UART is the other producer: idle while a script drives the board)
            break;
        }
        case DbgPartVoices: ui.debugSetVoices(a, b); break;
        case DbgGovernor: if (setGovernor) setGovernor(b != 0); break;
        case DbgSoak: soakStart((uint32_t)b); break;
        case DbgLean: if (setLean) setLean(b != 0); break;
        case DbgLanePart: if (a < app::kLanes && b >= 0 && b < app::kParts) ui.pattern().lanes[a].part = (uint8_t)b; break;
        default: break;
    }
}

void debugHookService(app::Ui& ui, DebugGovernorFn setGovernor, DebugLeanFn setLean, const gfx::Canvas& canvas) {
    DebugMailbox& m = debugMail;
    if (m.head - m.tail > (uint32_t)kDebugEvents) m.tail = m.head - kDebugEvents;   // the script overran the ring: drop the oldest
    while (m.tail != m.head) {
        const uint32_t w = m.ev[m.tail % kDebugEvents];
        m.tail = m.tail + 1;
        apply(ui, setGovernor, setLean, w);
    }
    const uint32_t req = m.capRequest;
    if (req) {
        const uint32_t strip = req - 1;
        if (strip < (uint32_t)(gfx::kHeight / kCapRows)) {
            canvas.raster(m.cap, (int)strip * kCapRows, kCapRows);
            m.capDone = req;
        }
        m.capRequest = 0;
    }
}

}  // namespace board

#endif
