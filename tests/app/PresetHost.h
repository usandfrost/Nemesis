#ifndef __TESTS_PRESET_HOST_H
#define __TESTS_PRESET_HOST_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "app/Params.h"
#include "app/PresetFile.h"

// The preset library on the host: a folder of *.PRS files (the repo's content/presets, DBOY_PRESET_DIR) behind the same app::PresetStorage the card
// implements, with the index kept in a temporary folder (the repo's folder stays clean). Counts what is read, so the tests can see the laziness.
struct DirPresets : app::PresetStorage {
    std::string dir, indexPath;
    int listCalls = 0, fileReads = 0, indexWrites = 0;
    bool failFiles = false;   // readFile fails (a card that cannot be read)
    explicit DirPresets(const std::string& d, const std::string& index) : dir(d), indexPath(index) {}
    bool list(void (*fn)(void*, const char*, uint32_t, uint32_t), void* ctx) override {
        ++listCalls;
        DIR* d = opendir(dir.c_str());
        if (!d) return false;
        std::string names[2048];
        int n = 0;
        while (dirent* e = readdir(d)) if (e->d_name[0] != '.' && n < 2048) names[n++] = e->d_name;
        closedir(d);
        for (int i = 0; i < n; ++i) {
            struct stat st;
            if (stat((dir + "/" + names[i]).c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
            fn(ctx, names[i].c_str(), (uint32_t)st.st_size, (uint32_t)st.st_mtime);
        }
        return true;
    }
    bool readFile(const char* name, uint8_t* buf, size_t cap, size_t& n) override {
        ++fileReads;
        n = 0;
        if (failFiles) return false;
        FILE* f = fopen((dir + "/" + name).c_str(), "rb");
        if (!f) return false;
        n = fread(buf, 1, cap, f);
        const bool more = fgetc(f) != EOF;
        fclose(f);
        return !more;
    }
    bool indexSize(size_t& n) override {
        struct stat st;
        if (stat(indexPath.c_str(), &st) != 0) return false;
        n = (size_t)st.st_size;
        return true;
    }
    bool readIndex(size_t off, uint8_t* buf, size_t len) override {
        FILE* f = fopen(indexPath.c_str(), "rb");
        if (!f) return false;
        const bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
        fclose(f);
        return ok;
    }
    bool fileExists(const char* name) override {
        struct stat st;
        return stat((dir + "/" + name).c_str(), &st) == 0;
    }
    bool removeFile(const char* name) override { return ::remove((dir + "/" + name).c_str()) == 0; }
    bool failWrites = false;   // writeFile fails (a card that cannot be written)
    int fileWrites = 0;
    bool writeFile(const char* name, const uint8_t* buf, size_t n) override {
        if (failWrites) return false;
        ++fileWrites;
        const std::string tmp = dir + "/SAVING.TMP";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return false;
        const bool ok = fwrite(buf, 1, n, f) == n;
        fclose(f);
        return ok && rename(tmp.c_str(), (dir + "/" + name).c_str()) == 0;
    }
    bool writeIndex(size_t off, const uint8_t* buf, size_t len) override {
        if (off == 0) ++indexWrites;
        FILE* f = fopen(indexPath.c_str(), off == 0 ? "wb" : "r+b");
        if (!f) return false;
        const bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fwrite(buf, 1, len, f) == len;
        fclose(f);
        return ok;
    }
};

// The repo's library, opened once per process (the first call reads the files and writes the index in a temporary folder).
inline DirPresets& shippedPresets() {
    static DirPresets* p = nullptr;
    if (!p) {
        char tmpl[] = "/tmp/dboy_prs_XXXXXX";
        const char* t = mkdtemp(tmpl);
        static std::string tmpRoot;   // removed at exit (every run used to leave a /tmp/dboy_prs_XXXXXX behind)
        if (t) { tmpRoot = t; atexit([] { const std::string c = "rm -rf '" + tmpRoot + "'"; if (system(c.c_str())) {} }); }
        // a copy of the repo's folder: tests that save presets write there, never into the repo
        const std::string root = t ? t : "/tmp", lib = root + "/lib";
        mkdir(lib.c_str(), 0755);
        // DBOY_PRESET_SRC: another folder of .PRS files in place of the repo's (content/tools/render_listen renders a new batch from it). Otherwise the library
        // is everything the card is given: content/presets and the folders in DBOY_PRESET_MORE (tests/CMakeLists.txt)
        std::vector<std::string> srcs;
        if (getenv("DBOY_PRESET_SRC")) srcs.push_back(getenv("DBOY_PRESET_SRC"));
        else {
            srcs.push_back(DBOY_PRESET_DIR);
#ifdef DBOY_PRESET_MORE
            std::string more = DBOY_PRESET_MORE;   // folders separated by ':'
            for (size_t a = 0; a <= more.size();) {
                const size_t b = more.find(':', a), e = b == std::string::npos ? more.size() : b;
                if (e > a) srcs.push_back(more.substr(a, e - a));
                a = e + 1;
            }
#endif
        }
        const char* src = srcs[0].c_str();
        for (const std::string& dir : srcs) {
            DIR* d = opendir(dir.c_str());
            if (!d) continue;
            while (dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                FILE* in = fopen((dir + "/" + e->d_name).c_str(), "rb");
                FILE* out = fopen((lib + "/" + e->d_name).c_str(), "wb");
                char buf[512];
                size_t n;
                while (in && out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
                if (in) fclose(in);
                if (out) fclose(out);
            }
            closedir(d);
        }
        p = new DirPresets(lib, root + "/INDEX.PIX");
        app::presetsInit(new app::PresetInfo[app::kPresetCap], app::kPresetCap);
        const app::PresetOpen r = app::presetsOpen(*p);
        if (r.count == 0) fprintf(stderr, "no presets in %s\n", src);
    }
    return *p;
}

#endif
