// Builds the PAL Hidden Mansion data overlay for the US base from the user's US + PAL
// discs (C++ version of tools/hidden_mansion_data.py; docs/HIDDEN_MANSION.md):
//
//   Map/map2u.szp      copied unchanged from the PAL disc (served for /Map/map2.szp while
//                      the hidden-mansion flag is set).
//   Event/eventNN.szp  the US archive with only text/eventNN.txt (the language-neutral event
//                      script) swapped for the PAL one, stored as literal-only Yay0.
//   manifest.txt       one "guest path -> overlay file [condition]" line per redirect.
//
// and the reverse, the USA Hidden Mansion for the European base (usa/, docs/SETUP.md):
//
//   usa/Map/map2.szp      the US /Map/map2.szp (served for /Map/map2u.szp).
//   usa/Event/eventNN.szp the PAL archive (all languages) with text/eventNN.txt swapped for
//                         the US script.
//   usa/manifest.txt
#include "setup/hm_overlay.h"

#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace setup {
namespace {

using Bytes = std::vector<std::uint8_t>;

// Event scripts the PAL release changed for the Hidden Mansion.
constexpr const char* kEvents[] = {"08", "16", "73"};

std::uint32_t be32(const Bytes& b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("truncated archive");
    return std::uint32_t(b[o]) << 24 | std::uint32_t(b[o + 1]) << 16 | std::uint32_t(b[o + 2]) << 8 | b[o + 3];
}
std::uint16_t be16(const Bytes& b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("truncated archive");
    return std::uint16_t(b[o] << 8 | b[o + 1]);
}
void put32(Bytes& b, size_t o, std::uint32_t v) {
    b[o] = std::uint8_t(v >> 24);
    b[o + 1] = std::uint8_t(v >> 16);
    b[o + 2] = std::uint8_t(v >> 8);
    b[o + 3] = std::uint8_t(v);
}

Bytes yay0(const Bytes& d) {
    const std::uint32_t size = be32(d, 4);
    std::uint32_t linkOff = be32(d, 8), chunkOff = be32(d, 12), maskOff = 16, mask = 0;
    int bits = 0;
    Bytes out;
    out.reserve(size);
    while (out.size() < size) {
        if (bits == 0) {
            mask = be32(d, maskOff);
            maskOff += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            if (chunkOff >= d.size()) throw std::runtime_error("bad Yay0 data");
            out.push_back(d[chunkOff++]);
        } else {
            const std::uint16_t link = be16(d, linkOff);
            linkOff += 2;
            const size_t dist = (link & 0xFFF) + 1u;
            size_t n = link >> 12;
            if (n == 0) {
                if (chunkOff >= d.size()) throw std::runtime_error("bad Yay0 data");
                n = d[chunkOff++] + 0x12u;
            } else {
                n += 2;
            }
            if (dist > out.size()) throw std::runtime_error("bad Yay0 back-reference");
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        bits--;
    }
    out.resize(size);
    return out;
}

Bytes yaz0(const Bytes& d) {
    const std::uint32_t size = be32(d, 4);
    Bytes out;
    out.reserve(size);
    size_t src = 16;
    while (out.size() < size) {
        if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
        const std::uint8_t code = d[src++];
        for (int bit = 0; bit < 8 && out.size() < size; bit++) {
            if (code & (0x80 >> bit)) {
                if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
                out.push_back(d[src++]);
            } else {
                if (src + 1 >= d.size()) throw std::runtime_error("bad Yaz0 data");
                const std::uint8_t b1 = d[src], b2 = d[src + 1];
                src += 2;
                const size_t dist = (size_t(b1 & 0xF) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                if (n == 0) {
                    if (src >= d.size()) throw std::runtime_error("bad Yaz0 data");
                    n = d[src++] + 0x12u;
                } else {
                    n += 2;
                }
                if (dist > out.size()) throw std::runtime_error("bad Yaz0 back-reference");
                for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

Bytes decompress(const Bytes& d) {
    if (d.size() >= 16 && std::memcmp(d.data(), "Yay0", 4) == 0) return yay0(d);
    if (d.size() >= 16 && std::memcmp(d.data(), "Yaz0", 4) == 0) return yaz0(d);
    return d;
}

// A valid Yay0 stream made only of literal bytes (what tools/hidden_mansion_data.py writes).
Bytes yay0Store(const Bytes& data) {
    const std::uint32_t nwords = std::uint32_t((data.size() + 31) / 32);
    const std::uint32_t linkOff = 16 + 4 * nwords;
    Bytes out(16 + 4 * size_t(nwords));
    std::memcpy(out.data(), "Yay0", 4);
    put32(out, 4, std::uint32_t(data.size()));
    put32(out, 8, linkOff);
    put32(out, 12, linkOff);
    for (std::uint32_t i = 0; i < nwords; i++) put32(out, 16 + 4 * i, 0xFFFFFFFFu);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

struct RarcEntry {
    size_t entryOffset;  // of the 20-byte file entry
    std::uint8_t flags;
    std::uint32_t dataRel, size;
};

// Every file in an uncompressed RARC archive: path -> entry.
std::map<std::string, RarcEntry> rarcFiles(const Bytes& arc) {
    if (arc.size() < 0x40 || std::memcmp(arc.data(), "RARC", 4) != 0) throw std::runtime_error("not a RARC archive");
    const size_t info = 0x20;
    const size_t nodeOff = be32(arc, info + 4) + info;
    const size_t entryOff = be32(arc, info + 12) + info;
    const size_t strOff = be32(arc, info + 20) + info;
    auto name = [&](size_t off) {
        std::string s;
        for (size_t i = strOff + off; i < arc.size() && arc[i]; i++) s += char(arc[i]);
        return s;
    };
    std::map<std::string, RarcEntry> files;
    int depth = 0;
    auto walk = [&](auto& self, std::uint32_t node, const std::string& prefix) -> void {
        if (++depth > 64) throw std::runtime_error("RARC directory loop");
        const size_t n = nodeOff + size_t(node) * 16;
        const std::uint16_t count = be16(arc, n + 10);
        const std::uint32_t first = be32(arc, n + 12);
        for (std::uint32_t i = first; i < first + count; i++) {
            const size_t e = entryOff + size_t(i) * 20;
            const std::uint32_t flagsName = be32(arc, e + 4);
            const std::uint32_t a = be32(arc, e + 8), size = be32(arc, e + 12);
            const std::string ename = name(flagsName & 0xFFFFFF);
            if (ename == "." || ename == "..") continue;
            if ((flagsName >> 24) & 0x02) self(self, a, prefix + ename + "/");
            else files[prefix + ename] = {e, std::uint8_t(flagsName >> 24), a, size};
        }
        --depth;
    };
    walk(walk, 0, "");
    return files;
}

Bytes member(const Bytes& arc, const RarcEntry& e) {
    const size_t dataOff = be32(arc, 0x0C) + 0x20;
    if (dataOff + e.dataRel + e.size > arc.size()) throw std::runtime_error("RARC member out of range");
    return Bytes(arc.begin() + std::ptrdiff_t(dataOff + e.dataRel), arc.begin() + std::ptrdiff_t(dataOff + e.dataRel + e.size));
}

// Copy of a RARC archive with one file's contents replaced: the new data is appended to
// the data section; directory/entry/string tables stay, so ids and names are stable.
Bytes rarcReplace(const Bytes& arc, const RarcEntry& target, const Bytes& data) {
    const size_t dataOff = be32(arc, 0x0C) + 0x20;
    const std::uint32_t dataLen = be32(arc, 0x10), mram = be32(arc, 0x14), aram = be32(arc, 0x18);
    Bytes out(arc.begin(), arc.begin() + std::ptrdiff_t(dataOff + dataLen));
    out.resize(out.size() + ((32 - out.size() % 32) % 32), 0);
    const std::uint32_t newRel = std::uint32_t(out.size() - dataOff);
    out.insert(out.end(), data.begin(), data.end());
    out.resize(out.size() + ((32 - out.size() % 32) % 32), 0);
    const std::uint32_t grow = std::uint32_t(out.size() - (dataOff + dataLen));
    put32(out, target.entryOffset + 8, newRel);
    put32(out, target.entryOffset + 12, std::uint32_t(data.size()));
    put32(out, 0x04, std::uint32_t(out.size()));
    put32(out, 0x10, dataLen + grow);
    if (target.flags & 0x10) put32(out, 0x14, mram + grow);       // preloaded to main RAM
    else if (target.flags & 0x20) put32(out, 0x18, aram + grow);  // preloaded to ARAM
    return out;
}

bool writeFile(const std::filesystem::path& p, const Bytes& b) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    return bool(o);
}

}  // namespace

bool buildHiddenMansionOverlay(const Disc& us, const Disc& pal, const std::filesystem::path& out, std::string& error) {
    try {
        std::error_code ec;
        std::filesystem::remove_all(out, ec);
        std::filesystem::create_directories(out / "Map");
        std::filesystem::create_directories(out / "Event");
        std::string manifest;
        Bytes map;
        if (!pal.readFile("/Map/map2u.szp", map)) throw std::runtime_error("the PAL disc has no /Map/map2u.szp");
        if (!writeFile(out / "Map" / "map2u.szp", map)) throw std::runtime_error("could not write the overlay");
        manifest += "/Map/map2.szp -> Map/map2u.szp [hidden]\n";
        // Event archives: `into` with its script replaced by the one from `from`.
        auto events = [&](const Disc& into, const Disc& from, const std::filesystem::path& dir, const std::string& prefix,
                          std::string& list) {
            for (const char* ev : kEvents) {
                const std::string file = std::string("event") + ev + ".szp";
                const std::string path = "/Event/" + file, script = std::string("text/event") + ev + ".txt";
                Bytes baseRaw, fromRaw;
                if (!into.readFile(path.c_str(), baseRaw) || !from.readFile(path.c_str(), fromRaw))
                    throw std::runtime_error("missing " + path);
                const Bytes u = decompress(baseRaw), p = decompress(fromRaw);
                const auto uf = rarcFiles(u), pf = rarcFiles(p);
                const auto ui = uf.find(script), pi = pf.find(script);
                if (ui == uf.end() || pi == pf.end()) throw std::runtime_error(path + " has no " + script);
                const Bytes newScript = member(p, pi->second);
                if (member(u, ui->second) == newScript) continue;  // identical: nothing to serve
                const Bytes patched = rarcReplace(u, ui->second, newScript);
                // Same checks as the Python tool: the swapped member, and nothing else changed.
                const auto cf = rarcFiles(patched);
                if (member(patched, cf.at(script)) != newScript) throw std::runtime_error("archive rewrite failed");
                for (auto& [k, e] : uf)
                    if (k != script && member(patched, cf.at(k)) != member(u, e)) throw std::runtime_error("archive rewrite failed");
                const Bytes packed = yay0Store(patched);
                if (decompress(packed) != patched) throw std::runtime_error("Yay0 packing failed");
                if (!writeFile(dir / "Event" / file, packed)) throw std::runtime_error("could not write the overlay");
                list += path + " -> " + prefix + "Event/" + file + "\n";
            }
        };
        events(us, pal, out, "", manifest);
        std::ofstream(out / "manifest.txt", std::ios::binary) << manifest;

        // The reverse: USA Hidden Mansion data for the European game.
        const auto usa = out / "usa";
        std::filesystem::create_directories(usa / "Map");
        std::filesystem::create_directories(usa / "Event");
        std::string reverse;
        Bytes usMap;
        if (!us.readFile("/Map/map2.szp", usMap)) throw std::runtime_error("the USA disc has no /Map/map2.szp");
        if (!writeFile(usa / "Map" / "map2.szp", usMap)) throw std::runtime_error("could not write the overlay");
        reverse += "/Map/map2u.szp -> usa/Map/map2.szp [hidden]\n";
        events(pal, us, usa, "usa/", reverse);
        std::ofstream(usa / "manifest.txt", std::ios::binary) << reverse;
        return true;
    } catch (const std::exception& e) {
        error = std::string("Building the Hidden Mansion data failed: ") + e.what();
        return false;
    }
}

}  // namespace setup
