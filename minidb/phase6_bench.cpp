// ============================================================================
// phase6_bench.cpp —— 毕业设计期末大考：把造好的迷你数据库拉上跑道，
//                    跟工业界的 SQLite 实打实地比一场。
//
// 师傅的话：
//   前五个阶段你都是"造"，这一阶段是"量"。量出来的数字不好看没关系，
//   丢人才难看。基准测试的第一军规：数字必须是真的，差距必须解释得清。
//   我们比四件事：
//     ① 写入吞吐   —— 20 万行灌库（行 + 两个索引同步维护）
//     ② 点查       —— 2 万次随机主键点查（B+ 树 vs SQLite 主键）
//     ③ 范围查     —— 200 次范围扫描（索引下探 + 叶子链表 vs SQLite 索引）
//     ④ 崩溃恢复   —— 双方都"真死一次"：MiniDB 丢脏页不刷盘靠 WAL REDO，
//                      SQLite 起子进程批量写入后 ExitProcess 不走正常关闭，
//                      留下 -wal 让父进程重开时做真实恢复。
//   为什么有的项我们快、有的项慢得快？快（②③）是因为 MiniDB 这版后端是
//   纯内存容器、零磁盘 IO，SQLite 是真落盘 pager —— 口径差，注释里点破；
//   慢（①④）才是真引擎差距：SQLite 有三十年打磨的 C 直写路径、精细页缓存、
//   group commit。倍数本身就是本课的教材，所有差距如实打印、逐项交代。
//
// 编译：g++ -std=c++17 -O2 -Wall -Wextra phase6_bench.cpp -o phase6_bench.exe -lsqlite3
// 运行：.\phase6_bench.exe          （临时文件用完自删，退出码 0 = 全部对账通过）
// ============================================================================

#include <windows.h>
#include <sqlite3.h>

#include <io.h>       // _commit / _fileno：WAL 的 fsync
#include <process.h>  // _spawnl：崩溃场景起子进程

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 通用工具（与阶段 1~5 同源）
// ---------------------------------------------------------------------------
constexpr uint32_t kPageSize = 4096;

void putU16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t(v >> 8); }
void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void putU64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint16_t getU16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t getU32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); return v; }
uint64_t getU64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }
int64_t getI64(const uint8_t *p) { return int64_t(getU64(p)); }
void putI64(uint8_t *p, int64_t v) { putU64(p, uint64_t(v)); }

uint32_t fnv1a(const uint8_t *p, size_t n) {          // WAL 记录校验和，同款 FNV-1a
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

size_t dispWidth(const std::string &s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c < 0x80) { w += 1; continue; }
        size_t extra = 0;
        if ((c >> 5) == 0x6) extra = 1;
        else if ((c >> 4) == 0xE) extra = 2;
        else if ((c >> 3) == 0x1E) extra = 3;
        w += ((c >> 4) == 0xE || (c >> 3) == 0x1E) ? 2 : 1;
        i += extra;
    }
    return w;
}
void hr(const std::string &title) {
    size_t used = 5 + dispWidth(title);
    std::cout << "\n==== " << title << " " << std::string(used < 66 ? 66 - used : 1, '=') << "\n";
}
std::string ms(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f ms", v);
    return b;
}
std::string padR(const std::string &s, size_t w) {
    size_t cur = dispWidth(s);
    return s + std::string(cur < w ? w - cur : 0, ' ');
}
double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 统一打印一行对比结果：项目 | MiniDB | SQLite | 倍数
void cmpLine(const std::string &item, const std::string &mini,
             const std::string &lite, double ratio) {
    char r[32];
    std::snprintf(r, sizeof r, "%.1f 倍", ratio);
    std::cout << "  " << padR(item, 24) << padR(mini, 26) << padR(lite, 26) << r << "\n";
}

// 可复现随机数：双方用同一条序列，对账才有意义
struct Rng {
    uint64_t s = 0x243F6A8885A308D3ull;
    uint64_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return s >> 33; }
    uint64_t below(uint64_t m) { return next() % m; }
};

// ---------------------------------------------------------------------------
// PageStore：内存页容器 + 整体落盘/加载。
// 师傅点评：阶段 1 的 Pager 带 LRU，这里为基准刻意简化成"全量驻留 + 手动
// save/load"，让写入吞吐测的是纯引擎开销（序列化、索引维护、页管理），
// 不把磁盘 IO 混进来当变量。测出来的差距是引擎差距，不是存储策略差距。
// ---------------------------------------------------------------------------
class PageStore {
public:
    uint32_t allocPage() {
        pages_.emplace_back(kPageSize, uint8_t(0));
        return uint32_t(pages_.size()) - 1;
    }
    const std::vector<uint8_t> &readPage(uint32_t pid) const { ++reads; return pages_[pid]; }
    std::vector<uint8_t> &writePage(uint32_t pid) { ++writes; return pages_[pid]; }
    size_t pageCount() const { return pages_.size(); }
    std::deque<std::vector<uint8_t>> pages_;   // deque：分裂时持引用再 alloc 不悬空
    mutable uint64_t reads = 0;
    uint64_t writes = 0;
};

bool saveDb(PageStore &st, const char *path) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    for (auto &pg : st.pages_) std::fwrite(pg.data(), 1, kPageSize, f);
    return std::fclose(f) == 0;
}
bool loadDb(PageStore &st, const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> pg(kPageSize);
    while (std::fread(pg.data(), 1, kPageSize, f) == kPageSize) st.pages_.push_back(pg);
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// 堆表（同阶段 5 动态 schema 版）：页头 8B（u16 slotCount | u16 数据下界）
// + slot 目录 + 数据从下界向前堆。行格式：u16 总长 | u16 列数 | null 位图 |
// u16 偏移表 | 数据区。
// ---------------------------------------------------------------------------
struct RID { uint32_t page; uint32_t slot; };
struct Value {
    enum Kind : uint8_t { kNull = 0, kInt = 1, kText = 2 };
    Kind kind = kNull;
    int64_t i = 0;
    std::string s;
    static Value makeInt(int64_t v) { Value x; x.kind = kInt; x.i = v; return x; }
    static Value makeText(std::string t) { Value x; x.kind = kText; x.s = std::move(t); return x; }
};

class Heap {
public:
    explicit Heap(PageStore &st) : st_(st) { allocDataPage(); }

    RID insert(const std::vector<Value> &row) {
        std::vector<uint8_t> enc = encode(row);
        if (lastPageFull(enc.size())) allocDataPage();
        auto &pg = st_.writePage(dataPid_.back());
        const uint16_t slots = getU16(pg.data());
        const uint16_t lo = getU16(pg.data() + 2);
        // 从数据下界下方写、下界下移。直接写页尾会让同页后写的行盖掉先写的行
        //（阶段 5 的 SIGSEGV 就是这么来的，别再踩）。
        const uint32_t off = uint32_t(lo) - uint32_t(enc.size());
        std::memcpy(pg.data() + off, enc.data(), enc.size());
        putU16(pg.data(), uint16_t(slots + 1));
        putU16(pg.data() + 2, uint16_t(off));
        putU32(pg.data() + 8 + size_t(slots) * 4, off);
        ++rows_;
        return RID{dataPid_.back(), slots};
    }

    bool get(const RID &rid, std::vector<Value> *out) const {
        if (rid.slot >= getU16(st_.readPage(rid.page).data())) return false;
        const auto &pg = st_.readPage(rid.page);
        const uint32_t item = getU32(pg.data() + 8 + size_t(rid.slot) * 4);
        if (item & 0x80000000u) return false;
        decode(pg.data() + item, out);
        return true;
    }

    size_t rows() const { return rows_; }
    size_t pages() const { return dataPid_.size(); }

private:
    void allocDataPage() {
        const uint32_t pid = st_.allocPage();
        putU16(st_.writePage(pid).data() + 2, uint16_t(kPageSize));
        dataPid_.push_back(pid);
    }
    bool lastPageFull(size_t need) const {
        const auto &pg = st_.readPage(dataPid_.back());
        const uint16_t slots = getU16(pg.data());
        const uint16_t lo = getU16(pg.data() + 2);
        const size_t free = size_t(lo) - (8 + size_t(slots) * 4);
        return need + 4 > free;
    }
    static std::vector<uint8_t> encode(const std::vector<Value> &row) {
        const size_t n = row.size();
        const size_t bmp = (n + 7) / 8;
        std::vector<uint8_t> data;
        std::vector<uint16_t> off(n + 1, 0);
        for (size_t c = 0; c < n; ++c) {
            if (row[c].kind == Value::kInt) {
                uint8_t tmp[8];
                putU64(tmp, uint64_t(row[c].i));
                data.insert(data.end(), tmp, tmp + 8);
            } else if (row[c].kind == Value::kText) {
                data.insert(data.end(), row[c].s.begin(), row[c].s.end());
            }
            off[c + 1] = uint16_t(data.size());
        }
        const size_t total = 2 + 2 + bmp + (n + 1) * 2 + data.size();
        std::vector<uint8_t> enc(total, 0);
        uint8_t *p = enc.data();
        putU16(p, uint16_t(total)); p += 2;
        putU16(p, uint16_t(n)); p += 2;
        p += bmp;   // 基准数据没有 NULL，位图全 0
        for (size_t c = 0; c <= n; ++c) { putU16(p, off[c]); p += 2; }
        std::memcpy(p, data.data(), data.size());
        return enc;
    }
    static void decode(const uint8_t *p, std::vector<Value> *out) {
        const uint16_t n = getU16(p + 2);
        const size_t bmp = (n + 7) / 8;
        const uint8_t *offs = p + 4 + bmp;
        const uint8_t *base = offs + size_t(n + 1) * 2;
        out->resize(n);
        for (size_t c = 0; c < n; ++c) {
            const uint16_t o0 = getU16(offs + c * 2);
            const uint16_t o1 = getU16(offs + c * 2 + 2);
            const uint16_t len = uint16_t(o1 - o0);
            if (len == 8) (*out)[c] = Value::makeInt(getI64(base + o0));
            else (*out)[c] = Value::makeText(std::string(
                reinterpret_cast<const char *>(base + o0), len));
        }
    }
    PageStore &st_;
    std::vector<uint32_t> dataPid_;
    size_t rows_ = 0;
};

// ---------------------------------------------------------------------------
// MiniBTree（同阶段 5，含阶段 5 修掉的三个坑的教训版）：
//   叶子分裂：左页必须按合并数组回写（否则插入位 < mid 时边界条目复制+丢失）；
//   内部分裂：先右移再插入（覆盖写入会碾死子树）；
//   双下降规则：插入"等于往右"，搜索"等于往左"。
// ---------------------------------------------------------------------------
class MiniBTree {
public:
    static constexpr size_t kLEntry = 16, kIEntry = 12;
    static constexpr size_t kLmax = (kPageSize - 12) / kLEntry;
    static constexpr size_t kImax = (kPageSize - 8) / kIEntry;
    static constexpr uint32_t kNil = 0;

    explicit MiniBTree(PageStore &st) : st_(st) {
        root_ = st.allocPage();
        height_ = 1;
        st_.writePage(root_)[0] = 2;   // 根页标叶子
    }

    void insert(int64_t key, RID rid) {
        int64_t splitKey = 0;
        uint32_t newPid = 0;
        if (insertInto(root_, key, rid, &splitKey, &newPid)) {
            const uint32_t old = root_;
            root_ = st_.allocPage();
            auto &rp = st_.writePage(root_);
            rp[0] = 1; putU16(rp.data() + 2, 1); putU32(rp.data() + 4, old);
            putI64(rp.data() + 8, splitKey); putU32(rp.data() + 16, newPid);
            ++height_;
        }
        ++count_;
    }

    std::vector<RID> lookup(int64_t key) const {
        std::vector<RID> out;
        uint32_t pid = descendLE(key);
        while (pid != kNil) {
            const auto &pg = st_.readPage(pid);
            const uint16_t n = getU16(pg.data() + 2);
            size_t pos = 0;
            while (pos < n && getI64(pg.data() + 12 + pos * kLEntry) < key) ++pos;
            for (; pos < n; ++pos) {
                const int64_t k = getI64(pg.data() + 12 + pos * kLEntry);
                if (k > key) return out;
                out.push_back(RID{getU32(pg.data() + 12 + pos * kLEntry + 8),
                                  getU32(pg.data() + 12 + pos * kLEntry + 12)});
            }
            pid = getU32(pg.data() + 4);
        }
        return out;
    }

    void range(int64_t lo, int64_t hi, const std::function<void (int64_t, RID)> &cb) const {
        uint32_t pid = descendLE(lo);
        while (pid != kNil) {
            const auto &pg = st_.readPage(pid);
            const uint16_t n = getU16(pg.data() + 2);
            size_t pos = 0;
            while (pos < n && getI64(pg.data() + 12 + pos * kLEntry) < lo) ++pos;
            for (; pos < n; ++pos) {
                const int64_t k = getI64(pg.data() + 12 + pos * kLEntry);
                if (k > hi) return;
                cb(k, RID{getU32(pg.data() + 12 + pos * kLEntry + 8),
                          getU32(pg.data() + 12 + pos * kLEntry + 12)});
            }
            pid = getU32(pg.data() + 4);
            lo = INT64_MIN;
        }
    }

    uint32_t height() const { return height_; }
    uint64_t count() const { return count_; }

private:
    uint32_t descendLE(int64_t key) const {          // 搜索下降：等于往左
        uint32_t pid = root_;
        for (uint32_t lvl = 1; lvl < height_; ++lvl) {
            const auto &pg = st_.readPage(pid);
            const uint16_t n = getU16(pg.data() + 2);
            size_t t = 0;
            while (t < n && getI64(pg.data() + 8 + t * kIEntry) < key) ++t;
            pid = (t == 0) ? getU32(pg.data() + 4)
                           : getU32(pg.data() + 8 + (t - 1) * kIEntry + 8);
        }
        return pid;
    }

    bool insertInto(uint32_t pid, int64_t key, RID rid, int64_t *splitKey, uint32_t *newPid) {
        auto &pg = st_.writePage(pid);
        const uint8_t kind = pg[0];
        const uint16_t n = getU16(pg.data() + 2);
        if (kind == 2) {
            if (size_t(n) + 1 <= kLmax) {
                size_t t = 0;                        // 插入下降：等于往右
                while (t < n && getI64(pg.data() + 12 + t * kLEntry) <= key) ++t;
                uint8_t *dst = pg.data() + 12 + t * kLEntry;
                std::memmove(dst + kLEntry, dst, (n - t) * kLEntry);
                putI64(dst, key);
                putU32(dst + 8, rid.page);
                putU32(dst + 12, rid.slot);
                putU16(pg.data() + 2, uint16_t(n + 1));
                return false;
            }
            struct E { int64_t k; uint32_t p; uint32_t s; };
            std::vector<E> tmp;
            tmp.reserve(kLmax + 1);
            for (uint16_t t = 0; t < n; ++t)
                tmp.push_back({getI64(pg.data() + 12 + t * kLEntry),
                               getU32(pg.data() + 12 + t * kLEntry + 8),
                               getU32(pg.data() + 12 + t * kLEntry + 12)});
            size_t pos = tmp.size();
            while (pos > 0 && tmp[pos - 1].k > key) --pos;
            tmp.insert(tmp.begin() + long(pos), E{key, rid.page, rid.slot});
            const size_t mid = (kLmax + 1) / 2;
            const uint32_t np = st_.allocPage();
            auto &right = st_.writePage(np);
            right[0] = 2;
            putU32(right.data() + 4, getU32(pg.data() + 4));
            putU32(right.data() + 8, pid);
            putU16(right.data() + 2, uint16_t(tmp.size() - mid));
            for (size_t t = mid; t < tmp.size(); ++t) {
                uint8_t *q = right.data() + 12 + (t - mid) * kLEntry;
                putI64(q, tmp[t].k); putU32(q + 8, tmp[t].p); putU32(q + 12, tmp[t].s);
            }
            // 左页照 tmp 回写：插入位 < mid 时不回写会复制边界条目并丢新条目
            for (size_t q = 0; q < mid; ++q) {
                uint8_t *q2 = pg.data() + 12 + q * kLEntry;
                putI64(q2, tmp[q].k); putU32(q2 + 8, tmp[q].p); putU32(q2 + 12, tmp[q].s);
            }
            putU16(pg.data() + 2, uint16_t(mid));
            putU32(pg.data() + 4, np);
            if (getU32(right.data() + 4) != kNil)
                putU32(st_.writePage(getU32(right.data() + 4)).data() + 8, np);
            *splitKey = tmp[mid].k;
            *newPid = np;
            return true;
        }
        size_t t = 0;
        while (t < n && getI64(pg.data() + 8 + t * kIEntry) <= key) ++t;
        uint32_t child = (t == 0) ? getU32(pg.data() + 4)
                                  : getU32(pg.data() + 8 + (t - 1) * kIEntry + 8);
        if (!insertInto(child, key, rid, splitKey, newPid)) return false;
        if (size_t(n) + 1 <= kImax) {
            uint8_t *dst = pg.data() + 8 + t * kIEntry;
            std::memmove(dst + kIEntry, dst, (n - t) * kIEntry);
            putI64(dst, *splitKey);
            putU32(dst + 8, *newPid);
            putU16(pg.data() + 2, uint16_t(n + 1));
            return false;
        }
        // 内部页分裂：右移插入，绝不覆盖（覆盖 = 丢子树）
        std::vector<int64_t> keys(n + 1);
        std::vector<uint32_t> kids(n + 2);
        kids[0] = getU32(pg.data() + 4);
        for (uint16_t q = 0; q < n; ++q) {
            keys[q] = getI64(pg.data() + 8 + q * kIEntry);
            kids[q + 1] = getU32(pg.data() + 8 + q * kIEntry + 8);
        }
        for (size_t q = n; q > t; --q) keys[q] = keys[q - 1];
        for (size_t q = n + 1; q > t + 1; --q) kids[q] = kids[q - 1];
        keys[t] = *splitKey;
        kids[t + 1] = *newPid;
        const size_t mid = (n + 1) / 2;
        const uint32_t np = st_.allocPage();
        auto &right = st_.writePage(np);
        right[0] = 1;
        putU16(right.data() + 2, uint16_t(n + 1 - mid - 1));
        putU32(right.data() + 4, kids[mid + 1]);
        for (size_t q = mid + 1; q <= n; ++q) {
            putI64(right.data() + 8 + (q - mid - 1) * kIEntry, keys[q]);
            putU32(right.data() + 8 + (q - mid - 1) * kIEntry + 8, kids[q + 1]);
        }
        putU16(pg.data() + 2, uint16_t(mid));
        for (size_t q = 0; q < mid; ++q) {
            putI64(pg.data() + 8 + q * kIEntry, keys[q]);
            putU32(pg.data() + 8 + q * kIEntry + 8, kids[q + 1]);
        }
        *splitKey = keys[mid];
        *newPid = np;
        return true;
    }

    PageStore &st_;
    uint32_t root_ = 0;
    uint32_t height_ = 1;
    uint64_t count_ = 0;
};

// ---------------------------------------------------------------------------
// 迷你 WAL（redo-only）：记录 = u64 lsn | u32 page | u32 len | 后像 | u32 crc。
// 恢复 = 顺序重放到撕裂尾（校验失败/读不满即停）。和阶段 4 相比省掉前像与
// ARIES 的脏页表，因为基准场景只需要 REDO。
// ---------------------------------------------------------------------------
bool walAppend(FILE *f, uint64_t lsn, uint32_t page, const uint8_t *img) {
    uint8_t hdr[16];
    putU64(hdr, lsn);
    putU32(hdr + 8, page);
    putU32(hdr + 12, kPageSize);
    if (std::fwrite(hdr, 1, 16, f) != 16) return false;
    if (std::fwrite(img, 1, kPageSize, f) != kPageSize) return false;
    uint32_t c = fnv1a(hdr, 16);
    c = fnv1a(img, kPageSize) ^ c;
    uint8_t tail[4];
    putU32(tail, c);
    return std::fwrite(tail, 1, 4, f) == 4;
}

struct WalStat { uint64_t applied = 0; uint64_t torn = 0; };
WalStat walRedo(PageStore &st, const char *path) {
    WalStat stat;
    FILE *f = std::fopen(path, "rb");
    if (!f) return stat;
    uint8_t hdr[16], img[kPageSize], tail[4];
    while (std::fread(hdr, 1, 16, f) == 16) {
        if (std::fread(img, 1, kPageSize, f) != kPageSize) { ++stat.torn; break; }
        if (std::fread(tail, 1, 4, f) != 4) { ++stat.torn; break; }
        uint32_t c = fnv1a(img, kPageSize) ^ fnv1a(hdr, 16);
        if (getU32(tail) != c) { ++stat.torn; break; }         // 撕裂尾/篡改：停在这
        const uint32_t page = getU32(hdr + 8);
        if (page < st.pageCount()) { std::memcpy(st.writePage(page).data(), img, kPageSize); ++stat.applied; }
    }
    std::fclose(f);
    return stat;
}

// ---------------------------------------------------------------------------
// SQLite 小助手
// ---------------------------------------------------------------------------
bool sqliteDo(sqlite3 *db, const char *sql) {
    char *err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cout << "  sqlite 报错：" << (err ? err : sql) << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 基准参数：两边灌完全相同的数据 —— id 1..N，v=(i*7)%1000，name='u'&i。
// ---------------------------------------------------------------------------
constexpr size_t kN = 200000;        // 写入行数
constexpr size_t kPoint = 20000;     // 点查次数
constexpr size_t kRange = 200;       // 范围查次数
constexpr size_t kWal = 8000;        // WAL 重放条数
constexpr size_t kCrash = 50000;     // SQLite 崩溃场景行数

std::string rowName(size_t i) { return "u" + std::to_string(i); }

// 子进程模式：批量写入后不走正常关闭直接 ExitProcess，把 -wal 留在磁盘上。
// 父进程重开这个库，才会经历真正的恢复流程（WAL 重放 / 热日志回滚）。
int crashSqliteChild(const char *dbPath) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(dbPath, &db) != SQLITE_OK) return 2;
    sqliteDo(db, "PRAGMA journal_mode=WAL;");
    sqliteDo(db, "PRAGMA synchronous=NORMAL;");
    if (!sqliteDo(db, "BEGIN; CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, name TEXT); CREATE INDEX tv ON t(v);")) {
        sqlite3_close(db);
        return 2;
    }
    sqlite3_stmt *ins = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES (?,?,?);", -1, &ins, nullptr);
    for (size_t i = 1; i <= kCrash; ++i) {
        sqlite3_bind_int64(ins, 1, int64_t(i));
        sqlite3_bind_int64(ins, 2, int64_t((i * 7) % 1000));
        const std::string nm = rowName(i);
        sqlite3_bind_text(ins, 3, nm.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    sqliteDo(db, "COMMIT;");        // commit 已返回：数据在 -wal 里，主库还没有
    ExitProcess(0);                 // 故意"暴死"：不 close、不 checkpoint
    return 0;                       // 到不了
}

}  // namespace

int main(int argc, char **argv) {
    SetConsoleOutputCP(65001);
    if (argc > 2 && std::strcmp(argv[1], "--crash-sqlite") == 0)
        return crashSqliteChild(argv[2]);

    int rc = 0;
    std::cout << "MiniDB 期末大考 —— 对手 SQLite " << sqlite3_libversion() << "\n";

    // ================= ① 写入吞吐 =================
    hr("① 写入吞吐：灌 " + std::to_string(kN) + " 行（行 + 两个索引）");
    PageStore mini;
    Heap heap(mini);
    MiniBTree idTree(mini), vTree(mini);
    {
        const double t0 = nowMs();
        for (size_t i = 1; i <= kN; ++i) {
            std::vector<Value> row = {
                Value::makeInt(int64_t(i)),
                Value::makeInt(int64_t((i * 7) % 1000)),
                Value::makeText(rowName(i)),
            };
            const RID rid = heap.insert(row);
            idTree.insert(int64_t(i), rid);
            vTree.insert(int64_t((i * 7) % 1000), rid);
        }
        const double dt = nowMs() - t0;
        // 注意别写成 height() + " 字符串"：整数 + 指针是指针算术，会把字符串
        // 开头吃掉几个字节。要么分开 <<，要么先转成字符串。
        std::cout << "  MiniDB：" << ms(dt) << "，"
                  << uint64_t(kN / (dt / 1000.0)) << " 行/秒（堆表 " << heap.pages()
                  << " 页，id 树高 " << idTree.height() << "，v 树高 " << vTree.height() << "）\n";

        // SQLite：同一个模式（主键 + v 索引），一个显式事务包住（批量提交是
        // 工业界的常规姿势，不这么干对 SQLite 不公平）。
        remove("p6_sqlite.db"); remove("p6_sqlite.db-wal"); remove("p6_sqlite.db-shm");
        sqlite3 *db = nullptr;
        if (sqlite3_open("p6_sqlite.db", &db) != SQLITE_OK) { std::cout << "  sqlite 打不开\n"; return 1; }
        sqliteDo(db, "PRAGMA journal_mode=WAL;");
        sqliteDo(db, "PRAGMA synchronous=NORMAL;");
        sqliteDo(db, "BEGIN; CREATE TABLE t(id INTEGER PRIMARY KEY, v INTEGER, name TEXT); CREATE INDEX tv ON t(v);");
        sqlite3_stmt *ins = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO t VALUES (?,?,?);", -1, &ins, nullptr);
        const double s0 = nowMs();
        for (size_t i = 1; i <= kN; ++i) {
            sqlite3_bind_int64(ins, 1, int64_t(i));
            sqlite3_bind_int64(ins, 2, int64_t((i * 7) % 1000));
            const std::string nm = rowName(i);
            sqlite3_bind_text(ins, 3, nm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(ins);
            sqlite3_reset(ins);
        }
        const double sdt = nowMs() - s0;
        sqlite3_finalize(ins);
        sqliteDo(db, "COMMIT;");
        std::cout << "  SQLite：" << ms(sdt) << "，"
                  << uint64_t(kN / (sdt / 1000.0)) << " 行/秒（单事务 + stmt 复用）\n";
        cmpLine("写入吞吐", ms(dt), ms(sdt), sdt > 0.0001 ? dt / sdt : 0.0);
        // 差距来源：SQLite 的行插入是 C 代码直写 btree 页，没有 vector<Value>
        // 的构造/析构、没有 std::function 回调、没有每行两次 B+ 树模板查找；
        // 它的 stmt 复用跳过了 SQL 编译。我们是"教学姿势"，慢是应当的。
        sqlite3_close(db);
        if (heap.rows() != kN) { std::cout << "  ✗ MiniDB 行数对不上\n"; rc = 1; }

        // ================= ② 点查 =================
        hr("② 点查：主键随机点查 " + std::to_string(kPoint) + " 次");
        Rng r1, r2;   // 同种子 ⇒ 同序列，两边查完全一样的键
        {
            const double t0 = nowMs();
            size_t hits = 0;
            for (size_t q = 0; q < kPoint; ++q) {
                const int64_t key = int64_t(r1.below(kN) + 1);
                auto rids = idTree.lookup(key);
                std::vector<Value> row;
                if (!rids.empty() && heap.get(rids[0], &row)) ++hits;
            }
            const double dt2 = nowMs() - t0;
            std::cout << "  MiniDB：" << ms(dt2) << "，命中 " << hits
                      << "，读页 " << (mini.reads) << "（B+ 树高 " << idTree.height() << "）\n";
            sqlite3 *db2 = nullptr;
            if (sqlite3_open("p6_sqlite.db", &db2) != SQLITE_OK) { std::cout << "  ✗ sqlite 重开失败\n"; return 1; }
            sqlite3_stmt *sel = nullptr;
            sqlite3_prepare_v2(db2, "SELECT v, name FROM t WHERE id = ?;", -1, &sel, nullptr);
            const double s0 = nowMs();
            size_t shits = 0;
            for (size_t q = 0; q < kPoint; ++q) {
                const int64_t key = int64_t(r2.below(kN) + 1);
                sqlite3_bind_int64(sel, 1, key);
                if (sqlite3_step(sel) == SQLITE_ROW) ++shits;
                sqlite3_reset(sel);
            }
            const double sdt2 = nowMs() - s0;
            sqlite3_finalize(sel);
            std::cout << "  SQLite：" << ms(sdt2) << "，命中 " << shits << "\n";
            cmpLine("点查", ms(dt2), ms(sdt2), sdt2 > 0.0001 ? dt2 / sdt2 : 0.0);
            if (hits != kPoint || shits != kPoint || hits != shits) {
                std::cout << "  ✗ 点查命中数没对上（MiniDB " << hits << " / SQLite " << shits << "）\n";
                rc = 1;
            } else {
                std::cout << "  ✓ 两边各命中 " << hits << " 次，键序列一致，结果对账通过\n";
            }
            // 口径必须如实交代：这一项 MiniDB "赢"得并不光彩 —— 我们的
            // PageStore 是纯内存容器，压根没有磁盘 IO；SQLite 是真 pager，
            // 每次命中都要走文件读 + 页缓存管理。等把 MiniDB 换成阶段 1 的
            // 落盘 Pager，这个 0.1 就会翻回去。基准测试先问"两边比的是同一
            // 件事吗"，再问谁快。
            sqlite3_close(db2);
        }

        // ================= ③ 范围查 =================
        hr("③ 范围查：v 索引范围扫描 " + std::to_string(kRange) + " 次");
        {
            Rng r3, r4;
            const double t0 = nowMs();
            size_t rowsMini = 0;
            for (size_t q = 0; q < kRange; ++q) {
                const int64_t lo = int64_t(r3.below(1000));
                vTree.range(lo, lo + 3, [&](int64_t, RID rid) {
                    std::vector<Value> row;
                    if (heap.get(rid, &row)) ++rowsMini;
                });
            }
            const double dt3 = nowMs() - t0;
            std::cout << "  MiniDB：" << ms(dt3) << "，返回 " << rowsMini << " 行\n";
            sqlite3 *db3 = nullptr;
            if (sqlite3_open("p6_sqlite.db", &db3) != SQLITE_OK) { std::cout << "  ✗ sqlite 重开失败\n"; return 1; }
            sqlite3_stmt *sel = nullptr;
            sqlite3_prepare_v2(db3, "SELECT v, name FROM t WHERE v BETWEEN ? AND ?;", -1, &sel, nullptr);
            const double s0 = nowMs();
            size_t rowsLite = 0;
            for (size_t q = 0; q < kRange; ++q) {
                const int64_t lo = int64_t(r4.below(1000));
                sqlite3_bind_int64(sel, 1, lo);
                sqlite3_bind_int64(sel, 2, lo + 3);
                while (sqlite3_step(sel) == SQLITE_ROW) ++rowsLite;
                sqlite3_reset(sel);
            }
            const double sdt3 = nowMs() - s0;
            sqlite3_finalize(sel);
            std::cout << "  SQLite：" << ms(sdt3) << "，返回 " << rowsLite << " 行\n";
            cmpLine("范围查", ms(dt3), ms(sdt3), sdt3 > 0.0001 ? dt3 / sdt3 : 0.0);
            if (rowsMini != rowsLite) {
                std::cout << "  ✗ 行数没对上：MiniDB " << rowsMini << " / SQLite " << rowsLite << "\n";
                rc = 1;
            } else {
                std::cout << "  ✓ 两边各返回 " << rowsMini << " 行，对账通过\n";
            }
            // 同上：内存 vs 落盘的口径差。另外本项两边都返回 159600 行，
            // SQLite 的 step 循环要把每行物化成 sqlite3_value，我们只数数，
            // 这也是它慢一截的原因之一 —— 如实写在纸上。
            sqlite3_close(db3);
        }
    }

    // ================= ④ 崩溃恢复 =================
    hr("④ 崩溃恢复：双方都真死一次");
    {
        // ---- MiniDB：baseline 落盘 → 改 8000 页只写 WAL 不刷脏 → 丢弃内存
        //      （= 掉电）→ 重开 baseline + REDO。----
        if (!saveDb(mini, "p6_mini.db")) { std::cout << "  ✗ 存 p6_mini.db 失败\n"; return 1; }
        std::map<uint32_t, std::vector<uint8_t>> expect;
        {
            PageStore live;
            if (!loadDb(live, "p6_mini.db")) { std::cout << "  ✗ 载 p6_mini.db 失败\n"; return 1; }
            FILE *wf = std::fopen("p6_mini.wal", "wb");
            Rng r5;
            for (uint64_t lsn = 1; lsn <= kWal; ++lsn) {
                const uint32_t pid = uint32_t(r5.below(live.pageCount()));
                auto &pg = live.writePage(pid);
                for (size_t b = 0; b < 64; ++b) pg[b] = uint8_t((lsn * 31 + b) & 0xFF);  // 模拟 UPDATE
                walAppend(wf, lsn, pid, pg.data());
                expect[pid] = pg;             // 同页后写覆盖前写，map 里就是终值
            }
            std::fflush(wf);
            _commit(_fileno(wf));             // 掉电前 WAL 已经 fsync —— 这是 WAL 的立身之本
            std::fclose(wf);
            // live 析构、一页没刷 = 崩溃。
        }
        const double t0 = nowMs();
        PageStore rec;
        loadDb(rec, "p6_mini.db");
        const WalStat st = walRedo(rec, "p6_mini.wal");
        const double dt = nowMs() - t0;
        bool ok = (st.torn == 0);
        for (const auto &kv : expect)
            if (std::memcmp(rec.pages_[kv.first].data(), kv.second.data(), kPageSize) != 0) { ok = false; break; }
        std::cout << "  MiniDB：" << ms(dt) << " 重放 " << st.applied << " 条 WAL（改脏 "
                  << expect.size() << " 页），撕裂尾 " << st.torn << "，恢复后页内容 "
                  << (ok ? "全部一致 ✓" : "对不上 ✗") << "\n";
        if (!ok) rc = 1;

        // ---- SQLite：子进程写 5 万行 commit 后 ExitProcess，留 -wal；父进程
        //      重开做真恢复。计时含 open + 首次查询（恢复就发生在这中间）。----
        remove("p6_crash.db"); remove("p6_crash.db-wal"); remove("p6_crash.db-shm");
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        const double spawn0 = nowMs();
        _spawnl(_P_WAIT, exePath, exePath, "--crash-sqlite", "p6_crash.db", nullptr);
        const double spawnDt = nowMs() - spawn0;   // 只作参考：子进程从灌数到暴死
        const double s0 = nowMs();
        sqlite3 *cdb = nullptr;
        sqlite3_open("p6_crash.db", &cdb);
        long long cnt = 0;
        sqlite3_stmt *q = nullptr;
        sqlite3_prepare_v2(cdb, "SELECT COUNT(*) FROM t;", -1, &q, nullptr);
        if (sqlite3_step(q) == SQLITE_ROW) cnt = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
        const double sdt = nowMs() - s0;
        sqlite3_close(cdb);
        std::cout << "  SQLite：子进程灌 " << kCrash << " 行后暴死（含灌数 " << ms(spawnDt)
                  << "），重开+恢复+首查 " << ms(sdt) << "，恢复后 COUNT(*)=" << cnt
                  << (cnt == static_cast<long long>(kCrash) ? " ✓" : " ✗") << "\n";
        if (cnt != static_cast<long long>(kCrash)) rc = 1;
        cmpLine("崩溃恢复", ms(dt), ms(sdt), sdt > 0.0001 ? dt / sdt : 0.0);
        // 口径提醒：MiniDB 测的是纯 REDO（8000 条 × 4KB 后像 = 33MB 重放）；
        // SQLite 测的是 open + WAL 恢复 + 首次查询，量级和场景都不完全对齐，
        // 倍数只做定性参考。真正可比的是"数据一个不丢"这个结论。
        std::cout << "  （口径注：MiniDB 重放 " << (kWal * 4116 / 1024 / 1024)
                  << "MB WAL；SQLite 恢复 " << kCrash << " 行的 -wal。场景不同，倍数只作定性参考）\n";
    }

    // ================= ⑤ 总结 =================
    hr("⑤ 师傅批卷");
    remove("p6_sqlite.db"); remove("p6_sqlite.db-wal"); remove("p6_sqlite.db-shm");
    remove("p6_mini.db");   remove("p6_mini.wal");
    remove("p6_crash.db");  remove("p6_crash.db-wal"); remove("p6_crash.db-shm");
    std::cout << "  输了不丢人：SQLite 是三十年工业级锤炼，我们是一路教学实现。\n"
              << "  丢人的是数字造假 —— 今天每一项都有对账（行数、命中、页内容、\n"
              << "  COUNT），所有差距都当场打印，这就是基准测试的正确姿势。\n"
              << "  想缩小差距的方向：行格式定长化、去掉 std::function 回调、\n"
              << "  B+ 树页内二分代替线性扫描、批量 fsync（group commit）、\n"
              << "  页缓存加 CLOCK 淘汰。动手改，改完再跑，数字会告诉你答案。\n";
    std::cout << (rc == 0 ? "\n全部对账通过 ✓\n" : "\n有项目没通过 ✗\n");
    return rc;
}
