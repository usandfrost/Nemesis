// Host tests for firmware/src/midi/*: parser, queue, router. No framework: a failed CHECK prints and counts.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "midi/MidiOutRing.h"
#include "midi/MidiParser.h"
#include "midi/MidiRouter.h"
#include "midi/MidiTypes.h"
#include "midi/SysexIn.h"

using namespace midi;

static int gFail = 0, gChecks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++gChecks;                                                                    \
        if (!(cond)) { ++gFail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static std::vector<Event> parse(Parser& p, std::initializer_list<int> bytes) {
    std::vector<Event> out;
    for (int b : bytes) {
        Event e;
        if (p.feed(static_cast<uint8_t>(b), e) && e.type < Type::Clock) out.push_back(e);   // (real time has its own test)
    }
    return out;
}

static void testParserBasics() {
    Parser p;
    auto ev = parse(p, {0x90, 60, 100});
    CHECK(ev.size() == 1 && ev[0].type == Type::NoteOn && ev[0].channel == 0 && ev[0].data1 == 60 && ev[0].data2 == 100);
    ev = parse(p, {0x83, 60, 40});
    CHECK(ev.size() == 1 && ev[0].type == Type::NoteOff && ev[0].channel == 3 && ev[0].data2 == 40);
    ev = parse(p, {0xB0, 1, 127});
    CHECK(ev.size() == 1 && ev[0].type == Type::ControlChange && ev[0].data1 == 1 && ev[0].data2 == 127);
    ev = parse(p, {0xC5, 9});
    CHECK(ev.size() == 1 && ev[0].type == Type::ProgramChange && ev[0].channel == 5 && ev[0].data1 == 9);
    ev = parse(p, {0xD2, 77});
    CHECK(ev.size() == 1 && ev[0].type == Type::ChannelPressure && ev[0].data1 == 77);
    ev = parse(p, {0xE0, 0x00, 0x40});
    CHECK(ev.size() == 1 && ev[0].type == Type::PitchBend && ev[0].data1 == 0 && ev[0].data2 == 0x40);
    ev = parse(p, {0xA1, 64, 20});
    CHECK(ev.size() == 1 && ev[0].type == Type::PolyPressure);
}

static void testRunningStatus() {
    Parser p;
    auto ev = parse(p, {0x90, 60, 100, 62, 90, 64, 80});
    CHECK(ev.size() == 3);
    CHECK(ev[1].data1 == 62 && ev[2].data1 == 64 && ev[2].data2 == 80);
    // running status survives a real-time byte between messages
    ev = parse(p, {0x90, 60, 100, 0xF8, 62, 90});
    CHECK(ev.size() == 2);
    // note-on velocity 0 is a note-off, with release velocity 64
    ev = parse(p, {0x90, 60, 0});
    CHECK(ev.size() == 1 && ev[0].type == Type::NoteOff && ev[0].data1 == 60 && ev[0].data2 == 64);
    // running status after that carries over too
    ev = parse(p, {61, 0});
    CHECK(ev.size() == 1 && ev[0].type == Type::NoteOff && ev[0].data1 == 61);
}

static void testRealtimeEvents() {
    Parser p;
    Event e;
    CHECK(p.feed(0xF8, e) && e.type == Type::Clock);
    CHECK(p.feed(0xFA, e) && e.type == Type::Start);
    CHECK(p.feed(0xFB, e) && e.type == Type::Continue);
    CHECK(p.feed(0xFC, e) && e.type == Type::Stop);
    CHECK(!p.feed(0xFE, e));   // active sensing is not for anyone here
    CHECK(!p.feed(0x90, e));
    CHECK(p.feed(0xF8, e) && e.type == Type::Clock);   // in the middle of a note, which goes on
    CHECK(!p.feed(60, e));
    CHECK(p.feed(0xFA, e) && e.type == Type::Start);
    CHECK(p.feed(100, e) && e.type == Type::NoteOn && e.data1 == 60 && e.data2 == 100);
}

static void testRealtimeInsideMessage() {
    Parser p;
    auto ev = parse(p, {0x90, 0xF8, 60, 0xFE, 100});   // clock and active sensing inside a note-on
    CHECK(ev.size() == 1 && ev[0].type == Type::NoteOn && ev[0].data1 == 60 && ev[0].data2 == 100);
    ev = parse(p, {0xE0, 0x10, 0xFA, 0x20});
    CHECK(ev.size() == 1 && ev[0].type == Type::PitchBend && ev[0].data1 == 0x10 && ev[0].data2 == 0x20);
}

static void testSysexAndCommon() {
    Parser p;
    auto ev = parse(p, {0xF0, 0x7D, 1, 2, 3, 0xF7, 60, 100});   // data after F7 has no status
    CHECK(ev.empty());
    ev = parse(p, {0x90, 60, 100, 0xF0, 1, 2, 0xF7, 62, 90});   // sysex cancels running status
    CHECK(ev.size() == 1 && ev[0].data1 == 60);
    ev = parse(p, {0x90, 60, 100, 0xF2, 5, 6, 62, 90});          // song position (system common) cancels it too
    CHECK(ev.size() == 1);
    ev = parse(p, {0x90, 60, 100, 0xF0, 0x90, 64, 100, 0xF7});   // a status byte inside sysex ends it (and is a new status)
    CHECK(ev.size() == 2 && ev[1].data1 == 64);
}

static void testStrayAndPartial() {
    Parser p;
    CHECK(parse(p, {60, 100, 20}).empty());                       // data bytes with no status
    auto ev = parse(p, {0x90, 60, 0x91, 61, 100});                // partial note-on abandoned by a new status
    CHECK(ev.size() == 1 && ev[0].channel == 1 && ev[0].data1 == 61);
    ev = parse(p, {0x90, 60, 100});
    p.reset();
    CHECK(parse(p, {62, 90}).empty());                            // reset() forgets running status
}

static void testChannelFilter() {
    Parser p;
    p.setChannel(3);   // channel 3 = wire channel 2
    CHECK(parse(p, {0x90, 60, 100}).empty());
    auto ev = parse(p, {0x92, 60, 100});
    CHECK(ev.size() == 1 && ev[0].channel == 2);
    p.setChannel(0);   // omni
    CHECK(parse(p, {0x90, 60, 100}).size() == 1);
    p.setChannel(99);  // out of range falls back to omni
    CHECK(parse(p, {0x9F, 60, 100}).size() == 1);
}

static void testQueue() {
    SpscQueue<8> q;
    Event e;
    CHECK(!q.pop(e));
    for (int i = 0; i < 8; ++i) { Event x; x.data1 = i; CHECK(q.push(x)); }
    { Event x; x.data1 = 99; CHECK(!q.push(x)); }       // full: newest dropped
    CHECK(q.dropped() == 1 && q.size() == 8);
    for (int i = 0; i < 8; ++i) { CHECK(q.pop(e)); CHECK(e.data1 == i); }
    CHECK(!q.pop(e));
    for (int round = 0; round < 1000; ++round) {        // wrap-around, many times
        Event x; x.data1 = static_cast<uint8_t>(round);
        CHECK(q.push(x));
        CHECK(q.pop(e) && e.data1 == static_cast<uint8_t>(round));
    }
}

static void testQueueTwoThreads() {
    SpscQueue<64> q;
    const uint32_t kTotal = 2000000;
    std::atomic<bool> done{false};
    uint32_t received = 0, bad = 0;
    std::thread consumer([&] {
        uint32_t expect = 0;
        Event e;
        for (;;) {
            if (q.pop(e)) {
                const uint32_t v = static_cast<uint32_t>(e.data1) | (static_cast<uint32_t>(e.data2) << 8) | (static_cast<uint32_t>(e.channel) << 16);
                if (v < expect) ++bad;          // sequence numbers must never go backwards
                expect = v + 1;
                ++received;
            } else if (done.load()) {
                if (!q.pop(e)) break;
                ++received;
            }
        }
    });
    uint32_t sent = 0;
    for (uint32_t i = 0; i < kTotal; ++i) {
        Event e;
        e.data1 = i & 0xFF; e.data2 = (i >> 8) & 0xFF; e.channel = (i >> 16) & 0xFF;
        if (q.push(e)) ++sent;
    }
    done = true;
    consumer.join();
    CHECK(bad == 0);
    CHECK(received == sent);                          // nothing lost except what the queue counted as dropped
    CHECK(sent + q.dropped() == kTotal);
}

struct MockSink {
    std::string log;
    float lastBend = 99, lastWheel = 99, lastPress = 99;
    void noteOn(int n, float v) { char b[48]; std::snprintf(b, sizeof b, "on%d/%.3f ", n, v); log += b; }
    void noteOff(int n) { log += "off" + std::to_string(n) + " "; }
    void pitchBend(float v) { lastBend = v; }
    void modWheel(float v) { lastWheel = v; }
    void channelPressure(float v) { lastPress = v; }
    void allNotesOff() { log += "ALLOFF "; }
};

static Event ev(Type t, int d1, int d2) { Event e; e.type = t; e.data1 = static_cast<uint8_t>(d1); e.data2 = static_cast<uint8_t>(d2); return e; }

static void testRouter() {
    Router<MockSink> r;
    MockSink s;
    r.apply(ev(Type::NoteOn, 60, 127), s);
    r.apply(ev(Type::NoteOff, 60, 0), s);
    CHECK(s.log == "on60/1.000 off60 ");

    // sustain: note-off is held until the pedal lifts, then released exactly once
    s.log.clear();
    r.apply(ev(Type::ControlChange, 64, 127), s);
    r.apply(ev(Type::NoteOn, 62, 64), s);
    r.apply(ev(Type::NoteOff, 62, 0), s);
    CHECK(s.log == "on62/0.504 ");
    r.apply(ev(Type::ControlChange, 64, 0), s);
    CHECK(s.log == "on62/0.504 off62 ");
    r.apply(ev(Type::ControlChange, 64, 0), s);                  // lifting again changes nothing
    CHECK(s.log == "on62/0.504 off62 ");

    // re-striking a held note cancels its pending release
    s.log.clear();
    r.apply(ev(Type::ControlChange, 64, 100), s);
    r.apply(ev(Type::NoteOn, 65, 100), s);
    r.apply(ev(Type::NoteOff, 65, 0), s);
    r.apply(ev(Type::NoteOn, 65, 100), s);
    r.apply(ev(Type::ControlChange, 64, 0), s);
    CHECK(s.log.find("off65") == std::string::npos);

    // all notes off clears pending releases as well
    s.log.clear();
    r.apply(ev(Type::ControlChange, 64, 127), s);
    r.apply(ev(Type::NoteOn, 70, 100), s);
    r.apply(ev(Type::NoteOff, 70, 0), s);
    r.apply(ev(Type::ControlChange, 123, 0), s);
    r.apply(ev(Type::ControlChange, 64, 0), s);
    CHECK(s.log.find("ALLOFF") != std::string::npos && s.log.find("off70") == std::string::npos);

    // pitch bend: 8192 = centre, 0 = -1, 16383 just under +1
    r.apply(ev(Type::PitchBend, 0x00, 0x40), s);
    CHECK(s.lastBend == 0.0f);
    r.apply(ev(Type::PitchBend, 0x00, 0x00), s);
    CHECK(s.lastBend == -1.0f);
    r.apply(ev(Type::PitchBend, 0x7F, 0x7F), s);
    CHECK(s.lastBend > 0.999f && s.lastBend < 1.0f);

    // mod wheel, channel pressure, reset controllers
    r.apply(ev(Type::ControlChange, 1, 127), s);
    CHECK(s.lastWheel == 1.0f);
    r.apply(ev(Type::ChannelPressure, 64, 0), s);
    CHECK(std::fabs(s.lastPress - 64.0f / 127.0f) < 1e-6f);
    r.apply(ev(Type::ControlChange, 121, 0), s);
    CHECK(s.lastBend == 0.0f && s.lastWheel == 0.0f && s.lastPress == 0.0f && !r.sustainDown());
}

static void testSysexIn() {
    SysexIn in;
    uint8_t got[kSysexMax];
    int n = 0;
    const uint8_t ours[] = {0xF0, 0x7D, 0x4E, 0x50, 0x01, 0x02, 0x03, 0x06, 0xF7};
    for (uint8_t b : ours) in.feed(b);
    CHECK(in.take(got, n) && n == 9 && memcmp(got, ours, 9) == 0);
    CHECK(!in.take(got, n));
    // real time in the middle of it is not part of it
    for (uint8_t b : ours) { in.feed(b); in.feed(0xF8); }
    CHECK(in.take(got, n) && n == 9 && memcmp(got, ours, 9) == 0);
    // somebody else's SysEx, and plain notes, are not ours
    for (uint8_t b : {0xF0, 0x43, 0x10, 0x4C, 0x00, 0xF7, 0x90, 0x40, 0x7F}) in.feed((uint8_t)b);
    CHECK(!in.take(got, n));
    // cut off by a status byte, then whole
    for (int i = 0; i < 5; ++i) in.feed(ours[i]);
    in.feed(0x90);
    for (uint8_t b : ours) in.feed(b);
    CHECK(in.take(got, n) && n == 9);
    CHECK(!in.take(got, n));
    // two wait while the main loop is at the card; a third is counted, not mixed in
    for (int k = 0; k < 3; ++k) for (uint8_t b : ours) in.feed(b);
    CHECK(in.take(got, n) && in.take(got, n) && !in.take(got, n));
    CHECK(in.cut() >= 2);
    // too long for a slot
    in.feed(0xF0); in.feed(0x7D); in.feed(0x4E); in.feed(0x50);
    for (int i = 0; i < 400; ++i) in.feed(1);
    in.feed(0xF7);
    CHECK(!in.take(got, n));
    for (uint8_t b : ours) in.feed(b);
    CHECK(in.take(got, n) && n == 9);
}

static void testOutMerge() {
    OutRing<1024> ring(0);
    OutMerge<200> merge;
    uint8_t b = 0;
    const uint8_t sx[] = {0xF0, 1, 2, 3, 0xF7};
    auto drain = [&](std::vector<uint8_t>& out) { while (merge.next(ring, b)) out.push_back(b); };
    // what the ring holds goes first: a SysEx only starts on an empty ring, so it cannot land inside a note
    ring.put(0x90, 60, 100);
    ring.put(0x80, 60, 0);
    CHECK(merge.next(ring, b) && b == 0x90);
    CHECK(!merge.set(ring, sx, 5));
    std::vector<uint8_t> out;
    drain(out);
    const std::vector<uint8_t> want = {60, 100, 0x80, 60, 0};
    CHECK(out == want);
    CHECK(merge.set(ring, sx, 5));
    CHECK(!merge.set(ring, sx, 5));
    out.clear();
    drain(out);
    CHECK(out.size() == 5 && out[0] == 0xF0 && out[4] == 0xF7);
    CHECK(!merge.busy());
    // the next message waits for the ring: notes get through between the messages of a long transfer
    ring.put(0x90, 62, 100);
    CHECK(!merge.set(ring, sx, 5));
    out.clear();
    drain(out);
    CHECK(out.size() == 3 && merge.set(ring, sx, 5));
    drain(out);
    // a note queued during a SysEx does not get into it
    CHECK(merge.set(ring, sx, 5));
    CHECK(merge.next(ring, b) && b == 0xF0);
    ring.put(0x90, 61, 100);
    out.clear();
    drain(out);
    const std::vector<uint8_t> want2 = {1, 2, 3, 0xF7, 0x90, 61, 100};
    CHECK(out == want2);
}

int main() {
    testSysexIn();
    testOutMerge();
    testParserBasics();
    testRunningStatus();
    testRealtimeInsideMessage();
    testRealtimeEvents();
    testSysexAndCommon();
    testStrayAndPartial();
    testChannelFilter();
    testQueue();
    testQueueTwoThreads();
    testRouter();
    std::printf("%d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
