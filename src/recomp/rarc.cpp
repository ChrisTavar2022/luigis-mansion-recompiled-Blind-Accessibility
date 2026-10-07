// Archive helpers for host-built disc files (see rarc.h).
#include "recomp/rarc.h"

#include <dolphin/dvd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace recomp::rarc {
namespace {

u32 be32(const Bytes& b, size_t o) { return u32(b[o]) << 24 | u32(b[o + 1]) << 16 | u32(b[o + 2]) << 8 | b[o + 3]; }
u16 be16(const Bytes& b, size_t o) { return u16(b[o] << 8 | b[o + 1]); }
void put32(Bytes& b, size_t o, u32 v) {
    for (int i = 0; i < 4; i++) b[o + i] = u8(v >> (24 - 8 * i));
}
void put16(Bytes& b, size_t o, u16 v) {
    b[o] = u8(v >> 8);
    b[o + 1] = u8(v);
}
void align32(Bytes& b) { b.resize((b.size() + 31) & ~size_t(31), 0); }

u16 nameHash(const std::string& s) {
    u16 h = 0;
    for (char c : s) h = u16(h * 3 + u8(c));
    return h;
}

Bytes yaz0(const Bytes& in) {
    const u32 size = be32(in, 4);
    Bytes out;
    out.reserve(size);
    size_t p = 16;
    while (out.size() < size && p < in.size()) {
        const u8 code = in[p++];
        for (int b = 7; b >= 0 && out.size() < size && p < in.size(); b--) {
            if (code & (1 << b)) {
                out.push_back(in[p++]);
            } else {
                if (p + 1 >= in.size()) return out;
                const u8 b1 = in[p++], b2 = in[p++];
                const size_t dist = size_t((b1 & 0x0F) << 8 | b2) + 1;
                size_t n = b1 >> 4;
                n = n ? n + 2 : (p < in.size() ? size_t(in[p++]) : 0) + 0x12;
                if (dist > out.size()) return out;
                for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
            }
        }
    }
    return out;
}

Bytes yay0(const Bytes& in) {
    const u32 size = be32(in, 4);
    u32 linkOff = be32(in, 8), chunkOff = be32(in, 12), maskOff = 16, mask = 0;
    int bits = 0;
    Bytes out;
    out.reserve(size);
    while (out.size() < size) {
        if (bits == 0) {
            if (maskOff + 4 > in.size()) return out;
            mask = be32(in, maskOff);
            maskOff += 4;
            bits = 32;
        }
        if (mask & 0x80000000u) {
            if (chunkOff >= in.size()) return out;
            out.push_back(in[chunkOff++]);
        } else {
            if (linkOff + 2 > in.size()) return out;
            const u16 link = be16(in, linkOff);
            linkOff += 2;
            const size_t dist = (link & 0xFFF) + 1;
            size_t n = link >> 12;
            n = n ? n + 2 : (chunkOff < in.size() ? size_t(in[chunkOff++]) : 0) + 0x12;
            if (dist > out.size()) return out;
            for (size_t i = 0; i < n; i++) out.push_back(out[out.size() - dist]);
        }
        mask <<= 1;
        bits--;
    }
    return out;
}

// Directory node type: "ROOT" for the root, else the first four characters of the name in
// upper case, padded with spaces (how the game's archives are built; JKRArchive looks
// resources up by this type).
void nodeType(u8* out, const std::string& name, bool root) {
    if (root) {
        std::memcpy(out, "ROOT", 4);
        return;
    }
    for (int i = 0; i < 4; i++) out[i] = i < int(name.size()) ? u8(std::toupper(u8(name[i]))) : ' ';
}

}  // namespace

Bytes readDisc(const char* path) {
    DVDFileInfo fi{};
    if (!DVDOpen(path, &fi)) return {};
    Bytes buf((fi.length + 31) & ~31u);
    const s32 got = DVDReadPrio(&fi, buf.data(), s32(buf.size()), 0, 2);
    DVDClose(&fi);
    if (got <= 0) return {};
    buf.resize(fi.length);
    return buf;
}

Bytes decompress(const Bytes& data) {
    if (data.size() >= 16 && !std::memcmp(data.data(), "Yaz0", 4)) return yaz0(data);
    if (data.size() >= 16 && !std::memcmp(data.data(), "Yay0", 4)) return yay0(data);
    return data;
}

Bytes yay0Store(const Bytes& data) {
    const u32 nwords = u32((data.size() + 31) / 32);
    const u32 linkOff = 16 + 4 * nwords;
    Bytes out(16 + 4 * size_t(nwords));
    std::memcpy(out.data(), "Yay0", 4);
    put32(out, 4, u32(data.size()));
    put32(out, 8, linkOff);
    put32(out, 12, linkOff);
    for (u32 i = 0; i < nwords; i++) put32(out, 16 + 4 * i, 0xFFFFFFFFu);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

bool Archive::parse(const Bytes& d) {
    nodes.clear();
    if (d.size() < 0x40 || std::memcmp(d.data(), "RARC", 4) != 0) return false;
    const size_t info = 0x20;
    const u32 numNodes = be32(d, info), nodeOff = be32(d, info + 4) + info;
    const u32 numEntries = be32(d, info + 8), entryOff = be32(d, info + 12) + info;
    const u32 strOff = be32(d, info + 20) + info;
    const size_t dataOff = size_t(be32(d, 0x0C)) + 0x20;
    nextFileId = be16(d, info + 24);
    if (size_t(nodeOff) + size_t(numNodes) * 16 > d.size() || size_t(entryOff) + size_t(numEntries) * 20 > d.size())
        return false;
    auto str = [&](u32 off) {
        std::string s;
        for (size_t i = size_t(strOff) + off; i < d.size() && d[i]; i++) s += char(d[i]);
        return s;
    };
    nodes.resize(numNodes);
    for (u32 n = 0; n < numNodes; n++) {
        const size_t no = nodeOff + size_t(n) * 16;
        nodes[n].name = str(be32(d, no + 4));
        const u16 count = be16(d, no + 10);
        const u32 first = be32(d, no + 12);
        for (u32 i = first; i < first + count && i < numEntries; i++) {
            const size_t e = entryOff + size_t(i) * 20;
            const u32 flagsName = be32(d, e + 4), a = be32(d, e + 8), size = be32(d, e + 12);
            Item it;
            it.name = str(flagsName & 0xFFFFFF);
            if (it.name == "." || it.name == "..") continue;
            it.flags = u8(flagsName >> 24);
            it.id = be16(d, e);
            if (it.flags & 0x02) {
                if (a >= numNodes) return false;
                it.dir = true;
                it.node = int(a);
            } else {
                if (dataOff + a + size > d.size()) return false;
                it.data.assign(d.begin() + std::ptrdiff_t(dataOff + a), d.begin() + std::ptrdiff_t(dataOff + a + size));
            }
            nodes[n].items.push_back(std::move(it));
        }
    }
    return !nodes.empty();
}

Bytes Archive::build() const {
    // Nodes in breadth-first order from the root; entries grouped by node, each directory's
    // items followed by "." and "..", as in the game's own archives.
    std::vector<int> order{0}, newIndex(nodes.size(), -1), parent(nodes.size(), -1);
    newIndex[0] = 0;
    for (size_t i = 0; i < order.size(); i++)
        for (const Item& it : nodes[size_t(order[i])].items)
            if (it.dir && it.node >= 0 && newIndex[size_t(it.node)] < 0) {
                newIndex[size_t(it.node)] = int(order.size());
                parent[size_t(it.node)] = order[i];
                order.push_back(it.node);
            }

    Bytes strings;
    std::map<std::string, u32> strIndex;
    auto addStr = [&](const std::string& s) {
        auto f = strIndex.find(s);
        if (f != strIndex.end()) return f->second;
        const u32 off = u32(strings.size());
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        strIndex.emplace(s, off);
        return off;
    };
    addStr(".");
    addStr("..");

    size_t numEntries = 0;
    for (int n : order) numEntries += nodes[size_t(n)].items.size() + 2;

    Bytes nodeTab(order.size() * 16), entryTab(numEntries * 20), data;
    size_t e = 0;
    u16 fileId = 0;
    for (size_t ni = 0; ni < order.size(); ni++) {
        const Node& node = nodes[size_t(order[ni])];
        const size_t no = ni * 16;
        nodeType(&nodeTab[no], node.name, ni == 0);
        put32(nodeTab, no + 4, addStr(node.name));
        put16(nodeTab, no + 8, nameHash(node.name));
        put16(nodeTab, no + 10, u16(node.items.size() + 2));
        put32(nodeTab, no + 12, u32(e));
        auto entry = [&](u16 id, const std::string& name, u8 flags, u32 a, u32 size) {
            const size_t eo = e++ * 20;
            put16(entryTab, eo, id);
            put16(entryTab, eo + 2, nameHash(name));
            put32(entryTab, eo + 4, u32(flags) << 24 | addStr(name));
            put32(entryTab, eo + 8, a);
            put32(entryTab, eo + 12, size);
        };
        for (const Item& it : node.items) {
            if (it.dir) {
                entry(0xFFFF, it.name, 0x02, u32(newIndex[size_t(it.node)]), 0x10);
            } else {
                const u32 at = u32(data.size());
                data.insert(data.end(), it.data.begin(), it.data.end());
                align32(data);
                entry(it.id != 0xFFFF ? it.id : fileId, it.name, it.flags, at, u32(it.data.size()));
            }
            fileId++;
        }
        const int up = parent[size_t(order[ni])];
        entry(0xFFFF, ".", 0x02, u32(ni), 0x10);
        entry(0xFFFF, "..", 0x02, up < 0 ? 0xFFFFFFFFu : u32(newIndex[size_t(up)]), 0x10);
    }
    align32(strings);

    // Header (0x20) + info (0x20) + nodes + entries + strings, each table 32-byte aligned.
    Bytes out(0x40);
    out.insert(out.end(), nodeTab.begin(), nodeTab.end());
    align32(out);
    const u32 entryOff = u32(out.size() - 0x20);
    out.insert(out.end(), entryTab.begin(), entryTab.end());
    align32(out);
    const u32 strOff = u32(out.size() - 0x20);
    out.insert(out.end(), strings.begin(), strings.end());
    align32(out);
    const u32 dataOff = u32(out.size() - 0x20);
    out.insert(out.end(), data.begin(), data.end());
    std::memcpy(out.data(), "RARC", 4);
    put32(out, 0x04, u32(out.size()));
    put32(out, 0x08, 0x20);
    put32(out, 0x0C, dataOff);
    put32(out, 0x10, u32(data.size()));
    put32(out, 0x14, u32(data.size()));  // all files preloaded to main RAM
    put32(out, 0x20, u32(order.size()));
    put32(out, 0x24, 0x20);
    put32(out, 0x28, u32(numEntries));
    put32(out, 0x2C, entryOff);
    put32(out, 0x30, u32(strings.size()));
    put32(out, 0x34, strOff);
    put16(out, 0x38, std::max(nextFileId, fileId));
    return out;
}

Item* Archive::find(const std::string& path) {
    if (nodes.empty()) return nullptr;
    int node = 0;
    size_t pos = 0;
    for (;;) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        Item* found = nullptr;
        for (Item& it : nodes[size_t(node)].items)
            if (it.name == part) found = &it;
        if (!found || slash == std::string::npos) return found;
        if (!found->dir) return nullptr;
        node = found->node;
        pos = slash + 1;
    }
}

void Archive::put(const std::string& path, Bytes data) {
    if (nodes.empty()) nodes.push_back({"root", {}});
    int node = 0;
    size_t pos = 0;
    for (;;) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        Item* found = nullptr;
        for (Item& it : nodes[size_t(node)].items)
            if (it.name == part) found = &it;
        if (slash == std::string::npos) {
            if (found && !found->dir) {
                found->data = std::move(data);
            } else {
                Item it;
                it.name = part;
                it.data = std::move(data);
                nodes[size_t(node)].items.push_back(std::move(it));
            }
            return;
        }
        if (!found) {
            nodes.push_back({part, {}});
            Item it;
            it.name = part;
            it.dir = true;
            it.node = int(nodes.size() - 1);
            nodes[size_t(node)].items.push_back(std::move(it));
            node = int(nodes.size() - 1);
        } else {
            node = found->node;
        }
        pos = slash + 1;
    }
}

void Archive::remove(const std::string& path) {
    const size_t slash = path.rfind('/');
    int node = 0;
    if (slash != std::string::npos) {
        Item* dir = find(path.substr(0, slash));
        if (!dir || !dir->dir) return;
        node = dir->node;
    }
    const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    auto& items = nodes[size_t(node)].items;
    std::erase_if(items, [&](const Item& it) { return it.name == name && !it.dir; });
}

}  // namespace recomp::rarc
