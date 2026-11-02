#!/usr/bin/env python3
"""board_script.py - drive the Drumboy through the debug probe: press keys, turn knobs, read what the firmware is doing, take a picture of the screen.

    board_script.py SCRIPT.txt            run a script file
    board_script.py -c "key PLAY; wait 2; status"      run commands given on the command line (separated by ;)

It needs the firmware built with DBOY_DEBUG_HOOK (the default while bringing up; src/board/DebugHook.h), an ST-Link on the board's SWD header, and
OpenOCD (the copy inside STM32CubeIDE is found automatically; DBOY_OPENOCD and DBOY_OPENOCD_SCRIPTS override). If no OpenOCD is running on port 4444
this starts one (it attaches without resetting the board) and stops it again at the end. The ELF (default firmware/build/dboy_cathode.elf) is only
read for the address of the mailbox: it must be the build that is on the board.

Script commands (one per line, `#` starts a comment; times are seconds):
    key NAME [SEC]        press and release a key (held SEC, default 0.08).  Names: STOP PLAY REC UP ADD DOWN LEFT OK RIGHT COPY CLEAR PASTE MUTE FILL
                          STEP FILE PART MIXER OSC1 OSC2 SUB FLT1 FLT2 GRIT AMP MOD GLOB LANE INFO SONG SHIFT
    down NAME / up NAME   press / release separately (for chords such as SHIFT + a key)
    layer N [SEC]         a layer key, N = 1..10, pressed and released;   layerdown N / layerup N
    turn KNOB DELTA       turn a knob by DELTA detents;  KNOB = T1..T4 (top row) or B1..B4 (bottom row)
    mute LANE [0|1]       mute (default) or unmute a lane, 1..10;   solo A|B|C|all   mute every lane that plays on another part (all: unmute)
    governor on|off       the degrade governor (off: every voice stays, so a load measurement is not changed by it)
    lean on|off           the CATHODE parts play through the lean voice or the CATHODE engine; stays until the next reset. The board's default build has only the lean voice: `off` does nothing
    preset A|B ID         load library preset ID into that part (ids as in the .PRS files and the export);   note N [VEL] / noteoff N: MIDI note on channel 16
    wait SEC              let the board run
    reset                 reset the board and wait until it has booted
    cycles [SEC]          the audio load over SEC seconds from the audio interrupt's own cycle counters: reads right even when the audio is so heavy that the main loop starves
    profile [SEC]         wait SEC (default 3), then print where the audio load goes: each synth part, sampler, effects, sequencer
    lanepart N P          lane N (1-10) plays on part P (A-G), in the pattern on screen
    status                print what the firmware reports (mode, part, playing, audio load, degrade level, toast ...)
    expect FIELD OP VALUE      check a status field now (OP is == != < <= > >=); a failure ends the run with exit status 1
    expect toast contains TEXT
    snap FILE.png         save the screen as a PNG
    soak ROUNDS [SECS]    the card test on the board (board/CardSoak.h), with a progress line. A TEST CARD ONLY: it writes. Ends the run on any error
    log TEXT              print TEXT
Fields: voices partLoad samplerLoad fxLoad seqLoad (lists, for status/profile only), loops revision nowMs mode part playing songPlaying pattern step degrade load(%) loadAvg(%) loadMax(%) overruns sdramFreeKb tilesSent presentUs toastOn toast
"""
import glob, os, re, socket, struct, subprocess, sys, tempfile, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
KEYS = "STOP PLAY REC UP ADD DOWN LEFT OK RIGHT COPY CLEAR PASTE MUTE FILL STEP FILE PART MIXER OSC1 OSC2 SUB FLT1 FLT2 GRIT AMP MOD GLOB LANE INFO SONG SHIFT".split()
MODES = "Play Step Sound Mixer Browse ProjectM Song Fx Save Record".split()
KNOBS = {"T1": 0, "T2": 1, "T3": 2, "T4": 3, "B1": 4, "B2": 5, "B3": 6, "B4": 7}
MAGIC, VERSION, N_EV, STRIP_ROWS, W, H = 0x4442474D, 4, 32, 20, 960, 540
PARTS = "ABCDEFG"
EV = {"keydown": 1, "keyup": 2, "layerdown": 3, "layerup": 4, "turn": 5, "mute": 6, "solo": 7, "governor": 8, "preset": 9, "note": 10, "voices": 11, "lean": 12, "lanepart": 13, "soak": 14}
STATUS = ["loops", "revision", "nowMs", "mode", "part", "playing", "songPlaying", "pattern", "step", "degrade", "loadLast10", "loadAvg10", "loadMax10",
          "overruns", "sdramFreeKb", "tilesSent", "presentUs", "toastOn"]   # words 36..53


class Fail(Exception):
    pass


def find_openocd():
    exe = os.environ.get("DBOY_OPENOCD")
    scripts = os.environ.get("DBOY_OPENOCD_SCRIPTS")
    if not exe:
        c = glob.glob("/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/*externaltools.openocd*/tools/bin/openocd")
        exe = c[0] if c else None
    if not scripts:
        c = glob.glob("/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/*debug.openocd*/resources/openocd/st_scripts")
        scripts = c[0] if c else None
    if not exe or not scripts:
        raise Fail("OpenOCD not found: install STM32CubeIDE or set DBOY_OPENOCD and DBOY_OPENOCD_SCRIPTS")
    return exe, scripts


class Ocd:
    """OpenOCD's telnet port."""
    def __init__(self):
        self.proc = None
        try:
            self.sock = socket.create_connection(("127.0.0.1", 4444), timeout=2)
        except OSError:
            exe, scripts = find_openocd()
            self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", prefix="openocd_")
            self.proc = subprocess.Popen([exe, "-s", scripts, "-f", os.path.join(HERE, "board", "openocd.cfg")], stdout=self.log, stderr=subprocess.STDOUT)
            for _ in range(60):
                time.sleep(0.25)
                try:
                    self.sock = socket.create_connection(("127.0.0.1", 4444), timeout=2)
                    break
                except OSError:
                    if self.proc.poll() is not None:
                        break
            else:
                self.sock = None
            if self.proc.poll() is not None or not getattr(self, "sock", None):
                self.log.seek(0)
                raise Fail("OpenOCD did not start (is the ST-Link connected and no other tool using it?):\n" + self.log.read()[-600:])
        self.sock.settimeout(20)
        self.sock.settimeout(0.5)   # the banner and its prompt, which belong to nobody
        try:
            while self.sock.recv(65536):
                pass
        except socket.timeout:
            pass
        self.sock.settimeout(20)

    def cmd(self, c):
        self.sock.sendall((c + "\n").encode())
        out = b""
        # OpenOCD echoes the command, prints its answer, then the prompt "> ": wait for the prompt that comes AFTER the echo
        while not (out.rstrip().endswith(b">") and (not c or c.encode() in out)):
            try:
                d = self.sock.recv(65536)
            except socket.timeout:
                raise Fail("OpenOCD did not answer '%s'" % c)
            if not d:
                raise Fail("OpenOCD closed the connection")
            out += d
        return out.decode(errors="replace").replace("\x00", "")

    def read_words(self, addr, n):
        t = self.cmd("mdw 0x%x %d" % (addr, n))
        w = []
        for line in t.splitlines():
            m = re.match(r"\s*0x[0-9a-f]{8}: ((?:[0-9a-f]{8} ?)+)", line)
            if m:
                w += [int(x, 16) for x in m.group(1).split()]
        if len(w) != n:
            raise Fail("reading %d words at 0x%x gave %d: %r" % (n, addr, len(w), t[-200:]))
        return w

    def write_word(self, addr, v):
        self.cmd("mww 0x%x 0x%x" % (addr, v & 0xFFFFFFFF))

    def dump(self, addr, size):
        f = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        f.close()
        try:
            self.cmd("dump_image %s 0x%x %d" % (f.name, addr, size))
            data = open(f.name, "rb").read()
        finally:
            os.unlink(f.name)
        if len(data) != size:
            raise Fail("dump_image gave %d of %d bytes" % (len(data), size))
        return data

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass
        if self.proc:
            self.proc.terminate()
            self.proc.wait(5)


def mailbox_address(elf):
    nm = subprocess.run(["arm-none-eabi-nm", "-C", elf], capture_output=True, text=True)
    for line in nm.stdout.splitlines():
        if line.endswith(" board::debugMail"):
            return int(line.split()[0], 16)
    raise Fail("%s has no board::debugMail: build with DBOY_DEBUG_HOOK=ON" % elf)


def write_png(path, rgb, w, h):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class Board:
    def __init__(self, elf):
        self.base = mailbox_address(elf)
        self.ocd = Ocd()
        self.touched = False   # set when a command changes what must be put back at the end (the governor, lane mutes)
        try:
            self.problem = self.check()
        except Exception:
            self.ocd.close()   # an OpenOCD this script started must not outlive a failed start: it would hold the ST-Link
            raise

    def w(self, i):   # address of mailbox word i
        return self.base + 4 * i

    def check(self):
        """None when the mailbox is there, else why not. Only `reset` may run without it (a board that has not booted yet)."""
        magic, version = self.ocd.read_words(self.base, 2)
        if magic != MAGIC or version != VERSION:
            return "the board has no matching debug mailbox (magic %08x version %d): it is not running yet, or flash the build this ELF came from" % (magic, version)
        # RAM keeps its contents across a reset, so a mailbox can look fine on a board that is not running: the main loop must be counting
        a = self.ocd.read_words(self.w(36), 1)[0]
        time.sleep(0.2)
        if self.ocd.read_words(self.w(36), 1)[0] == a:
            return "the firmware is not running (the mailbox is stale): after a flash the CPU can sit in the bootloader's fault loop. Run the script command `reset`"
        return None

    def need(self):
        if self.problem:
            raise Fail(self.problem)

    def send(self, typ, a=0, b=0):
        self.need()
        deadline = time.time() + 5
        while True:
            head, tail = self.ocd.read_words(self.w(2), 2)
            if head - tail < N_EV - 1:
                break
            if time.time() > deadline:
                raise Fail("the firmware is not reading events (main loop stopped?)")
            time.sleep(0.02)
        self.ocd.write_word(self.w(4 + head % N_EV), typ | (a << 8) | ((b & 0xFFFF) << 16))
        self.ocd.write_word(self.w(2), head + 1)

    def status(self):
        self.need()
        w = self.ocd.read_words(self.w(36), 18)
        s = dict(zip(STATUS, w))
        raw = self.ocd.read_words(self.w(54), 24)
        s["toast"] = b"".join(struct.pack("<I", x) for x in raw).split(b"\0")[0].decode(errors="replace")
        s["mode"] = MODES[s["mode"]] if s["mode"] < len(MODES) else s["mode"]
        n = len(PARTS)
        v = self.ocd.read_words(self.w(80), 2 * n + 7)
        s["startOnMaxKc"], s["startPatchMaxKc"], s["startOnSumKc"], s["starts"] = v[2 * n + 3:2 * n + 7]   # a note start, thousands of cycles
        s["voices"] = v[0:n]
        s["cfgVoices"] = self.ocd.read_words(self.w(101), n)
        s["waveSlots"], s["waveResident"], refused = self.ocd.read_words(self.w(115), 3)
        s["waveRefused"] = refused - (1 << 32) if refused >= 1 << 31 else refused
        s["partPreset"] = [x - (1 << 32) if x >= 1 << 31 else x for x in self.ocd.read_words(self.w(108), n)]
        s["partLoad"] = [x / 10.0 for x in v[n:2 * n]]   # % of the audio block budget, averaged over the last second
        s["samplerLoad"], s["fxLoad"], s["seqLoad"] = v[2 * n] / 10.0, v[2 * n + 1] / 10.0, v[2 * n + 2] / 10.0
        s["step"] = s["step"] - (1 << 32) if s["step"] >= 1 << 31 else s["step"]
        s["load"] = s["loadLast10"] / 10.0
        s["loadAvg"] = s["loadAvg10"] / 10.0
        s["loadMax"] = s["loadMax10"] / 10.0
        return s

    def snap(self, path):
        self.need()
        out = bytearray()
        for strip in range(H // STRIP_ROWS):
            self.ocd.write_word(self.w(78), strip + 1)
            deadline = time.time() + 2
            while self.ocd.read_words(self.w(79), 1)[0] != strip + 1:
                if time.time() > deadline:
                    raise Fail("the firmware did not capture strip %d (main loop stopped?)" % strip)
                time.sleep(0.01)
            px = self.ocd.dump(self.w(148), W * STRIP_ROWS * 2)
            for (v,) in struct.iter_unpack("<H", px):
                out += bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
        write_png(path, bytes(out), W, H)

    def reset(self):
        self.ocd.cmd("reset_config none")
        self.ocd.cmd("cortex_m reset_config sysresetreq")
        self.ocd.cmd("reset run")
        deadline = time.time() + 20
        while time.time() < deadline:
            time.sleep(0.5)
            try:
                m, v = self.ocd.read_words(self.base, 2)
                if m == MAGIC and v == VERSION:
                    self.problem = None
                    if self.status()["loops"] > 5:
                        return
            except Fail:
                pass
        raise Fail("the board did not come back after a reset")


def key_index(name):
    n = name.upper()
    if n not in KEYS:
        raise Fail("no key '%s' (keys: %s)" % (name, " ".join(KEYS)))
    return KEYS.index(n)


def compare(a, op, b):
    return {"==": a == b, "!=": a != b, "<": a < b, "<=": a <= b, ">": a > b, ">=": a >= b}[op]


def run_line(bd, line, lineno, out_dir):
    f = line.split()
    c = f[0].lower()
    try:
        if c == "key":
            bd.send(EV["keydown"], key_index(f[1])); time.sleep(float(f[2]) if len(f) > 2 else 0.08); bd.send(EV["keyup"], key_index(f[1])); time.sleep(0.05)
        elif c == "down":
            bd.send(EV["keydown"], key_index(f[1]))
        elif c == "up":
            bd.send(EV["keyup"], key_index(f[1]))
        elif c == "layer":
            n = int(f[1]) - 1
            bd.send(EV["layerdown"], n); time.sleep(float(f[2]) if len(f) > 2 else 0.08); bd.send(EV["layerup"], n); time.sleep(0.05)
        elif c in ("layerdown", "layerup"):
            bd.send(EV[c], int(f[1]) - 1)
        elif c == "turn":
            bd.send(EV["turn"], KNOBS[f[1].upper()], int(f[2])); time.sleep(0.05)
        elif c == "mute":     # mute LANE (1..10) 0|1
            bd.touched = True
            bd.send(EV["mute"], int(f[1]) - 1, int(f[2]) if len(f) > 2 else 1)
        elif c == "solo":     # solo PART (A, B, C): every lane on another part is muted; "solo all" unmutes everything
            bd.touched = True
            bd.send(EV["solo"], 255 if f[1].lower() == "all" else PARTS.index(f[1].upper()))
        elif c == "governor":  # governor on|off: off keeps every voice, so a measurement is not changed by the load shedding
            bd.touched = True
            bd.send(EV["governor"], 0, 0 if f[1].lower() == "off" else 1)
        elif c == "lanepart":
            bd.send(EV["lanepart"], int(f[1]) - 1, PARTS.index(f[2].upper())); time.sleep(0.1)
        elif c == "lean":     # lean on|off: the CATHODE parts play through the lean voice (a DBOY_LEAN build) or the CATHODE engine
            bd.send(EV["lean"], 0, 0 if f[1].lower() == "off" else 1); time.sleep(0.3)
        elif c == "preset":   # preset PART(A-G) ID: load a library preset into a synth part (as the BROWSE list's OK does)
            bd.send(EV["preset"], PARTS.index(f[1].upper()), int(f[2])); time.sleep(0.3)
        elif c == "voices":   # voices PART(A-G) N: the part's VOICES setting
            bd.send(EV["voices"], PARTS.index(f[1].upper()), int(f[2])); time.sleep(0.2)
        elif c == "note":     # note N [VEL]: MIDI note on channel 16, which plays the part on screen;  noteoff N
            bd.send(EV["note"], int(f[1]), int(f[2]) if len(f) > 2 else 100)
        elif c == "noteoff":
            bd.send(EV["note"], int(f[1]), 0)
        elif c == "cycles":   # cycles [SEC]: the audio load from the interrupt's own counters (works even if the audio starves the main loop)
            sec = float(f[1]) if len(f) > 1 else 2.0
            n = len(PARTS)
            a = bd.ocd.read_words(bd.w(118), n + 4); t0 = time.time(); time.sleep(sec)
            b = bd.ocd.read_words(bd.w(118), n + 4); dt = time.time() - t0
            blocks = (b[0] - a[0]) & 0xFFFFFFFF
            if blocks == 0:
                raise Fail("no audio blocks in %.1f s: the audio interrupt is not running" % sec)
            per = [((b[i] - a[i]) & 0xFFFFFFFF) / blocks for i in range(1, n + 4)]
            bud = 399092.97
            vo = bd.ocd.read_words(bd.w(129), n)
            print("  from the interrupt: %.0f%% of the budget over %d blocks (%.0f blocks/s, needs 1378)  | parts A-G %s%% with %s voices sounding  sampler %.1f%%  effects %.1f%%"
                  % (100 * per[0] / bud, blocks, blocks / dt, "/".join("%.1f" % (100 * x / bud) for x in per[1:n + 1]), "/".join(map(str, vo)), 100 * per[n + 1] / bud, 100 * per[n + 2] / bud))
        elif c == "wait":
            time.sleep(float(f[1]))
        elif c == "reset":
            bd.reset()
        elif c == "status":
            s = bd.status()
            print("  mode %-8s part %d  playing %d  pattern %d step %3d  degrade %d | audio load %.1f%% (avg %.1f%%, worst %.1f%%, %d overruns) | loops %d  rev %d | toast: %r"
                  % (s["mode"], s["part"], s["playing"], s["pattern"], s["step"], s["degrade"], s["load"], s["loadAvg"], s["loadMax"], s["overruns"], s["loops"], s["revision"], s["toast"]))
        elif c == "profile":   # where the audio load goes, sampled for SEC seconds (default 3)
            time.sleep(float(f[1]) if len(f) > 1 else 3.0)
            s = bd.status()
            print("  load avg %.1f%% (worst block %.0f%%, %d overruns) | parts A-G %s%% with %s voices | sampler %.1f%% | effects %.1f%% | sequencer %.1f%% | degrade %d"
                  % (s["loadAvg"], s["loadMax"], s["overruns"], "/".join("%.1f" % x for x in s["partLoad"]), "/".join(map(str, s["voices"])),
                     s["samplerLoad"], s["fxLoad"], s["seqLoad"], s["degrade"]))
            print("  wavetable banks: %d slots, %d resident, refused %d; SDRAM free %d KB" % (s["waveSlots"], s["waveResident"], s["waveRefused"], s["sdramFreeKb"]))
            print("  note starts: %d, the engine's noteOn at most %d kcycles (avg %d), applying the patch at most %d kcycles  [one audio block = %d kcycles]"
                  % (s["starts"], s["startOnMaxKc"], s["startOnSumKc"] // max(s["starts"], 1), s["startPatchMaxKc"], 399))
        elif c == "expect":
            s = bd.status()
            if f[1] == "toast" and f[2] == "contains":
                want = " ".join(f[3:])
                ok, shown = want.lower() in s["toast"].lower(), s["toast"]
            else:
                key, op, val = f[1], f[2], " ".join(f[3:])
                have = s[key]
                want = type(have)(val) if not isinstance(have, str) else val
                ok, shown = compare(have, op, want), have
            print("  %s expect %s" % ("ok  " if ok else "FAIL", " ".join(f[1:]) + ("" if ok else "   (is %r)" % (shown,))))
            if not ok:
                raise Fail("expectation failed on line %d: %s" % (lineno, line))
        elif c == "snap":
            path = f[1] if os.path.isabs(f[1]) else os.path.join(out_dir, f[1])
            bd.snap(path); print("  screen saved to", path)
        elif c == "soak":     # soak ROUNDS [timeout s]: the card test (board/CardSoak.h): A TEST CARD ONLY, it writes. Waits for it, and fails on any error
            rounds = int(f[1]); limit = float(f[2]) if len(f) > 2 else rounds * 5.0
            bd.touched = True
            bd.send(EV["soak"], 0, rounds)
            names = ["none", "card refused", "open", "write", "close", "read back", "mismatch", "an earlier file differs", "project save", "project load", "project differs", "preset save", "remove", "card check"]
            t0 = time.time(); shown = 0.0
            while True:
                time.sleep(2)
                w = bd.ocd.read_words(bd.w(136), 12)
                if time.time() - shown > 20 or not w[0]:
                    shown = time.time()
                    print("  soak %d of %d rounds, %d writes, %d reads, %d MB, %d project saves, %d presets, %d card checks, %.0f s" % (w[1], w[2], w[3], w[4], w[8], w[5], w[6], w[7], time.time() - t0), flush=True)
                if w[9]:
                    raise Fail("soak stopped in round %d: %s (detail %d). The card is left as it is." % (w[10], names[w[9]] if w[9] < len(names) else w[9], w[11]))
                if not w[0] and w[2] == rounds and w[1] == rounds:
                    print("  soak finished clean"); break
                if time.time() - t0 > limit:
                    raise Fail("soak: no end after %.0f s, at round %d" % (limit, w[1]))
        elif c == "soakstat":
            print("  soak words", bd.ocd.read_words(bd.w(136), 12))
        elif c == "log":
            print("  " + line.split(None, 1)[1] if len(f) > 1 else "")
        else:
            raise Fail("unknown command '%s'" % f[0])
    except (IndexError, KeyError, ValueError) as e:
        raise Fail("line %d: '%s' is not understood (%s: %s)" % (lineno, line, type(e).__name__, e))


def main(argv):
    elf = os.path.join(ROOT, "firmware/build/dboy_cathode.elf")
    out_dir = os.getcwd()
    lines = None
    i = 0
    args = argv[1:]
    while i < len(args):
        if args[i] == "--elf": elf = args[i + 1]; i += 2
        elif args[i] == "--out": out_dir = args[i + 1]; i += 2
        elif args[i] == "-c": lines = [x.strip() for x in args[i + 1].split(";")]; i += 2
        elif args[i] in ("-h", "--help"): print(__doc__); return 0
        else: lines = [l.rstrip("\n") for l in open(args[i])]; i += 1
    if lines is None:
        print(__doc__); return 2
    os.makedirs(out_dir, exist_ok=True)
    bd = None
    try:
        bd = Board(elf)
        for n, raw in enumerate(lines, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            print("%3d  %s" % (n, line))
            run_line(bd, line, n, out_dir)
        print("done")
        return 0
    except Fail as e:
        print("board_script: " + str(e), file=sys.stderr)
        return 1
    finally:
        if bd:
            if bd.touched:   # leave the board as a player would have it: the governor on and no lane muted by `solo` or `mute`
                try:
                    bd.problem = None
                    bd.send(EV["governor"], 0, 1)
                    bd.send(EV["solo"], 255)
                except Exception:
                    pass
            bd.ocd.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
