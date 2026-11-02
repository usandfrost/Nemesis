#pragma once
#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

// An independent look at a card image: shares no code with FatFs or the firmware, and reports what a computer's disk check would object to. A lost
// cluster (allocated, in no file) is counted and is not a problem: it wastes room and hurts nothing. Nor is a chain longer than its file (`longer`), which a
// disk check trims: FatFs writes the chain before the size, so a power cut while a file grows leaves one. Only the rebuildable preset index may ever show this.
struct FatAudit {
    std::vector<std::string> problems;
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs;
    uint32_t lost = 0;
    std::vector<std::string> longer;
    bool fatsDiffer = false;

    static uint32_t r32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
    static uint32_t r16(const uint8_t* p) { return (uint32_t)(p[0] | p[1] << 8); }

    const uint8_t* img = nullptr;
    uint32_t spc = 0, fat1 = 0, fatSize = 0, data = 0, clusters = 0;
    std::map<uint32_t, std::string> owner;

    uint32_t fat(uint32_t c) const { return r32(img + (size_t)fat1 * 512 + c * 4) & 0x0FFFFFFF; }
    const uint8_t* cluster(uint32_t c) const { return img + (size_t)(data + (c - 2) * spc) * 512; }

    std::vector<uint32_t> chain(uint32_t c, const std::string& who) {
        std::vector<uint32_t> out;
        while (c >= 2 && c < clusters) {
            auto it = owner.find(c);
            if (it != owner.end()) { problems.push_back("cross-link: cluster " + std::to_string(c) + " in " + who + " and " + it->second); break; }
            owner[c] = who;
            out.push_back(c);
            const uint32_t n = fat(c);
            if (n == 0) { problems.push_back("cluster in use marked free: " + who); break; }
            if (n >= 0x0FFFFFF8) break;
            if (n >= clusters) { problems.push_back("FAT entry outside the volume: " + who); break; }
            c = n;
        }
        return out;
    }

    void walk(uint32_t first, const std::string& path, int depth) {
        if (depth > 16) { problems.push_back("too deep: " + path); return; }
        std::vector<uint8_t> raw;
        for (uint32_t c : chain(first, path.empty() ? "/" : path)) raw.insert(raw.end(), cluster(c), cluster(c) + spc * 512);
        std::string lfn;
        for (size_t i = 0; i + 32 <= raw.size(); i += 32) {
            const uint8_t* e = raw.data() + i;
            if (e[0] == 0) break;
            if (e[0] == 0xE5) { lfn.clear(); continue; }
            if ((e[11] & 0x3F) == 0x0F) {
                std::string part;
                for (int k : {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30}) { if (e[k] == 0 || e[k] == 0xFF) break; part += (char)e[k]; }
                lfn = part + lfn;
                continue;
            }
            if (e[11] & 0x08) { lfn.clear(); continue; }
            if (e[0] == '.') { lfn.clear(); continue; }
            std::string name;
            for (int k = 0; k < 8 && e[k] != ' '; ++k) name += (char)e[k];
            if (e[8] != ' ') { name += '.'; for (int k = 8; k < 11 && e[k] != ' '; ++k) name += (char)e[k]; }
            for (int k = 0; k < 11; ++k)
                if (e[k] < 0x20 && e[k] != 0x05 && !(k == 0)) { problems.push_back("garbage entry in " + path); lfn.clear(); name.clear(); break; }
            if (name.empty()) continue;
            if (!lfn.empty()) name = lfn;
            lfn.clear();
            const std::string p = path + "/" + name;
            const uint32_t c = r16(e + 20) << 16 | r16(e + 26), size = r32(e + 28);
            if (e[11] & 0x10) {
                if (c < 2 || c >= clusters) { problems.push_back("folder with a bad cluster: " + p); continue; }
                dirs.insert(p);
                walk(c, p, depth + 1);
            } else {
                std::vector<uint8_t> body;
                const auto ch = c ? chain(c, p) : std::vector<uint32_t>{};
                const size_t need = (size + spc * 512 - 1) / (spc * 512);
                if (ch.size() < need) problems.push_back("file cut short: " + p);
                else if (ch.size() > need) longer.push_back(p);
                for (uint32_t x : ch) body.insert(body.end(), cluster(x), cluster(x) + spc * 512);
                if (body.size() > size) body.resize(size);
                files[p] = body;
            }
        }
    }

    explicit FatAudit(const uint8_t* image) : img(image) {
        const uint32_t part = r32(img + 446 + 8);
        const uint8_t* b = img + (size_t)part * 512;
        if (r16(b + 11) != 512 || r16(b + 510) != 0xAA55) { problems.push_back("no boot sector"); return; }
        spc = b[13];
        const uint32_t reserved = r16(b + 14), nf = b[16], total = r32(b + 32);
        fatSize = r32(b + 36);
        fat1 = part + reserved;
        data = fat1 + nf * fatSize;
        clusters = (total - reserved - nf * fatSize) / spc + 2;
        if (nf == 2 && memcmp(img + (size_t)fat1 * 512, img + (size_t)(fat1 + fatSize) * 512, (size_t)fatSize * 512)) fatsDiffer = true;
        for (uint32_t c = 2; c < clusters; ++c) {
            const uint32_t v = fat(c);
            if (v != 0 && v < 0x0FFFFFF7 && (v < 2 || v >= clusters)) problems.push_back("FAT entry " + std::to_string(c) + " outside the volume");
        }
        walk(r32(b + 44), "", 0);
        for (uint32_t c = 2; c < clusters; ++c) if (fat(c) && !owner.count(c)) ++lost;
    }
};
