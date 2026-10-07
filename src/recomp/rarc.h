#pragma once
// Host-side helpers for the game's archive formats, used to rebuild disc files the port serves
// in place of the originals (unused_maps.cpp): Yay0/Yaz0 decompression, a RARC archive as an
// editable directory tree, and a literal-only Yay0 wrapper the game's loader accepts.
#include "recomp/ctx.h"

#include <string>
#include <vector>

namespace recomp::rarc {

using Bytes = std::vector<u8>;

Bytes readDisc(const char* path);           // whole disc file (empty if missing)
Bytes decompress(const Bytes& data);        // Yay0/Yaz0 -> raw; other data returned as is
Bytes yay0Store(const Bytes& data);         // valid Yay0 stream made of literal bytes only

struct Item {
    std::string name;
    bool dir = false;
    u16 id = 0xFFFF;   // file id as stored
    u8 flags = 0x11;   // file: 0x01 file | 0x10 preload to main RAM
    Bytes data;        // files
    int node = -1;     // dirs: index into Archive::nodes
};
struct Node {
    std::string name;
    std::vector<Item> items;  // without "." and ".."
};

struct Archive {
    std::vector<Node> nodes;  // nodes[0] is the root
    u16 nextFileId = 0;

    bool parse(const Bytes& raw);  // uncompressed RARC
    Bytes build() const;           // uncompressed RARC

    // Path lookups ("jmp/characterinfo"); nullptr when absent.
    Item* find(const std::string& path);
    // Adds (or replaces) a file, creating directories on the way.
    void put(const std::string& path, Bytes data);
    void remove(const std::string& path);
};

}  // namespace recomp::rarc
