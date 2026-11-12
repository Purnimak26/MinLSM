#include <string>
#include <vector>
#include <algorithm>

#include "memtable.h"
#include "sstable.h"

namespace lsm {

inline std::vector<SstEntry> merge_two(const std::vector<SstEntry>& older,
                                       const std::vector<SstEntry>& newer) {
    std::vector<SstEntry> out;
    out.reserve(older.size() + newer.size());

    size_t i = 0, j = 0;
    while (i < older.size() && j < newer.size()) {
        if (older[i].key < newer[j].key) {
            out.push_back(older[i++]);
        } else if (newer[j].key < older[i].key) {
            out.push_back(newer[j++]);
        } else {
            out.push_back(newer[j++]);
            ++i;
        }
    }

    while (i < older.size()) out.push_back(older[i++]);
    while (j < newer.size()) out.push_back(newer[j++]);

    return out;
}

void compact(const std::vector<std::string>& paths,
             const std::string& out_path,
             bool drop_tombstones = false) {
    if (paths.empty()) {
        return;
    }

    std::vector<SstEntry> acc;
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        acc = merge_two(acc, SSTable(*it).read_all());
    }

    if (drop_tombstones) {
        acc.erase(std::remove_if(acc.begin(), acc.end(),
                                 [](const SstEntry& e) { return e.tombstone; }),
                  acc.end());
    }

    SSTable::build(out_path, acc);
}

} 