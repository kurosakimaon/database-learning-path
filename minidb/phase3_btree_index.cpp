// ============================================================================
// minidb · 阶段 3：落盘 B+ 树二级索引 —— 把"按值找行"从全表扫描里捞出来
// ----------------------------------------------------------------------------
// 编译：  g++ -std=c++17 -O2 -Wall -Wextra phase3_btree_index.cpp -o phase3_btree_index.exe
// 运行：  .\phase3_btree_index.exe     （5 万行实测，大约 10~20 秒）
// 退出码：0 = 全部校验通过
//
// 师傅开场白：
//   阶段 2 结尾你亲手撞上的那堵墙还记得吗？"查 age=33 的所有行"必须先全表扫描，
//   因为堆表里根本没有"值 → RID"的映射。今天就把这个映射做成真东西：一棵落盘的
//   B+ 树。注意我说的是"二级索引"：叶子存的是 (key, RID)，不是行本身。
//
//   页格式两种，都住在同一个 .db 文件里，靠页头第一个 u16 区分：
//
//     叶子页（kind=2）                          内部页（kind=1）
//     [0..1]  u16 kind=2                        [0..1]  u16 kind=1
//     [2..3]  u16 n（条目数）                    [2..3]  u16 n（key 数 = 儿子数-1）
//     [4..7]  u32 left_sibling                  [4..7]  u32 child[0]
//     [8..11] u32 right_sibling                 [8..]   n × ( i64 key | u32 child )
//     [12..]  n × ( i64 key | u32 rid页 | u16 rid槽 )
//
//   叶子之间用左右兄弟指针串成双向链表 —— 范围扫描下探到左端点后就不再碰树，
//   沿链表一路向右，这是 B+ 树（而不是 B 树）通吃数据库的独门优势。
//
//   扇出不是拍脑袋定的，是拿 4096 和 key 尺寸实算出来的，见下面常量处的注释。
//   非唯一索引的处理：同一个 key 出现多次就存多条 (key, RID) 叶子条目，
//   天然形成一个"RID 列表"，不需要额外结构。
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <io.h>

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMagic = 0x4D494E49u;   // MINI：数据库文件头
constexpr uint32_t kIdxMagic = 0x4D494E58u; // MINX：索引头页专用
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFirstDataPage = 1;
constexpr uint32_t kPageHeader = 8;
constexpr uint32_t kSlotSize = 4;
constexpr uint32_t kSlotDeleted = 0x80000000u;

// ---- B+ 树页内布局常量。扇出是算出来的，不是规定的：---------------------
//   叶子条目 = i64 key + u32 rid.page + u16 rid.slot = 14 字节
//   叶子页头 = kind(2) + n(2) + left(4) + right(4)     = 12 字节
//   → 每叶页最多 (4096-12)/14 = 291 条
//   内部条目 = i64 key + u32 child = 12 字节
//   内部页头 = kind(2) + n(2) + child0(4)              = 8 字节
//   → 每内部页最多 (4096-8)/12 = 340 个 key、341 个儿子
//   所以一棵高度 3 的树能装 341*341*291 ≈ 3400 万条；5 万条只要 2 层。
//   这就是"B+ 树矮得离谱"的算术来源，面试背一百遍不如自己算一遍。
// ---------------------------------------------------------------------------
constexpr uint32_t kLeafKind = 2;
constexpr uint32_t kIntKind = 1;
constexpr uint32_t kLeafHdr = 12;
constexpr uint32_t kLeafEnt = 14;
constexpr uint32_t kIntHdr = 8;
constexpr uint32_t kIntEnt = 12;
constexpr uint32_t kLmax = (kPageSize - kLeafHdr) / kLeafEnt;   // 291
constexpr uint32_t kImax = (kPageSize - kIntHdr) / kIntEnt;     // 340
constexpr uint32_t kIdxHeaderPage = 1;   // 索引文件里页 1 固定放索引头

// ---- 小端序列化 + 校验（与阶段 1/2 同源，逐字节，格式我说了算）------------
void putU16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t(v >> 8); }
void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void putU64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint16_t getU16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t getU32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); return v; }
uint64_t getU64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }
uint32_t fnv1a(const uint8_t *p, size_t n) {
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
void hr(const char *title) {
    size_t used = 5 + dispWidth(title);
    std::cout << "\n==== " << title << " " << std::string(used < 64 ? 64 - used : 1, '=') << "\n";
}
std::string ms(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f ms", v);
    return b;
}
std::string pct(double hit, double total) {
    if (total <= 0) return "n/a";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f%%", 100.0 * hit / total);
    return b;
}
std::string padR(const std::string &s, size_t w) {
    size_t cur = dispWidth(s);
    return s + std::string(cur < w ? w - cur : 0, ' ');
}

// ---------------------------------------------------------------------------
// Pager（与阶段 1/2 同源，故意复制以保证单文件可编译。行为与阶段 2 一致）
// ---------------------------------------------------------------------------
class Pager {
public:
    struct Stats {
        uint64_t hits = 0, misses = 0, evictions = 0, flushes = 0, reads = 0;
    };

    Pager(const std::string &path, size_t cap, bool createIfMissing) : path_(path), cap_(std::max<size_t>(1, cap)) {
        // 用 fopen 探存在性：部分 libstdc++ 下 fstream 以 in 打开缺失文件时
        // 虽未真正打开，failbit 却不置位，if (!probe) 会误判为"文件存在"。
        std::FILE *probe = std::fopen(path.c_str(), "rb");
        if (!probe) {
            if (!createIfMissing) throw std::runtime_error("文件不存在且未允许创建: " + path);
            page_count_ = 1;
            std::FILE *fp = ::fopen(path.c_str(), "w+b");
            if (!fp) throw std::runtime_error("创建失败: " + path);
            writeMetaPage(fp);
            std::fflush(fp);
            ::_commit(_fileno(fp));
            std::fclose(fp);
            return;
        }
        std::fclose(probe);
        openExisting();
    }

    uint32_t pageCount() const { return page_count_; }
    uint32_t dataPages() const { return page_count_ > 1 ? page_count_ - 1 : 0; }
    const Stats &stats() const { return st_; }
    size_t fileBytes() const { return size_t(page_count_) * kPageSize; }

    std::vector<uint8_t> readPage(uint32_t pid) {
        auto it = idx_.find(pid);
        if (it != idx_.end()) {
            ++st_.hits;
            lru_.splice(lru_.begin(), lru_, it->second);
            return it->second->bytes;
        }
        ++st_.misses;
        ++st_.reads;
        std::vector<uint8_t> buf(kPageSize, 0);
        if (pid < page_count_) {
            std::FILE *fp = ::fopen(path_.c_str(), "rb");
            if (!fp) throw std::runtime_error("打开失败: " + path_);
            if (::fseek(fp, off(pid), SEEK_SET) == 0) {
                // 短读不报错：新分配的页文件还没长到那儿，语义 = 全零页（同阶段 2）
                if (std::fread(buf.data(), 1, kPageSize, fp) != kPageSize)
                    std::fill(buf.begin(), buf.end(), uint8_t(0));
            }
            std::fclose(fp);
        }
        insert(pid, std::move(buf), false);
        return idx_[pid]->bytes;
    }

    void writePage(uint32_t pid, const std::vector<uint8_t> &page) {
        auto it = idx_.find(pid);
        if (it != idx_.end()) {
            ++st_.hits;
            it->second->bytes = page;
            it->second->dirty = true;
            lru_.splice(lru_.begin(), lru_, it->second);
        } else {
            ++st_.misses;
            insert(pid, page, true);
        }
        if (pid >= page_count_) page_count_ = pid + 1;
    }

    uint32_t allocPage() { return page_count_++; }

    void flush() {
        std::FILE *fp = ::fopen(path_.c_str(), "r+b");
        if (!fp) throw std::runtime_error("打开失败: " + path_);
        growTo(fp, page_count_);
        writeMetaPage(fp);
        for (auto &e : lru_) {
            if (!e.dirty) continue;
            if (::fseek(fp, off(e.pid), SEEK_SET) != 0 ||
                std::fwrite(e.bytes.data(), 1, kPageSize, fp) != kPageSize) {
                std::fclose(fp);
                throw std::runtime_error("写页失败 pid=" + std::to_string(e.pid));
            }
            e.dirty = false;
            ++st_.flushes;
        }
        std::fflush(fp);
        ::_commit(_fileno(fp));
        std::fclose(fp);
    }

private:
    struct Entry { uint32_t pid; std::vector<uint8_t> bytes; bool dirty; };
    static long off(uint32_t pid) { return static_cast<long>(pid) * static_cast<long>(kPageSize); }

    void growTo(std::FILE *fp, uint32_t pages) {
        const long need = off(pages);
        std::fseek(fp, 0, SEEK_END);
        long have = std::ftell(fp);
        static const std::vector<uint8_t> zeros(kPageSize, 0);
        for (; have < need; have += static_cast<long>(kPageSize))
            std::fwrite(zeros.data(), 1, kPageSize, fp);
    }

    void insert(uint32_t pid, std::vector<uint8_t> bytes, bool dirty) {
        lru_.push_front(Entry{pid, std::move(bytes), dirty});
        idx_[pid] = lru_.begin();
        if (lru_.size() > cap_) {
            Entry &v = lru_.back();
            if (v.dirty) {
                std::FILE *fp = ::fopen(path_.c_str(), "r+b");
                if (fp) {
                    growTo(fp, page_count_);
                    if (::fseek(fp, off(v.pid), SEEK_SET) == 0) {
                        std::fwrite(v.bytes.data(), 1, kPageSize, fp);
                        std::fflush(fp);
                        ::_commit(_fileno(fp));
                    }
                    std::fclose(fp);
                    ++st_.flushes;
                }
            }
            ++st_.evictions;
            idx_.erase(v.pid);
            lru_.pop_back();
        }
    }

    void writeMetaPage(std::FILE *fp) {
        std::vector<uint8_t> m(kPageSize, 0);
        putU32(m.data() + 0, kMagic);
        putU32(m.data() + 4, kVersion);
        putU32(m.data() + 8, kPageSize);
        putU32(m.data() + 12, page_count_);
        putU32(m.data() + 16, fnv1a(m.data(), 12));
        if (::fseek(fp, 0, SEEK_SET) != 0) return;
        std::fwrite(m.data(), 1, kPageSize, fp);
    }

    void openExisting() {
        std::FILE *fp = ::fopen(path_.c_str(), "rb");
        if (!fp) throw std::runtime_error("打开失败: " + path_);
        std::vector<uint8_t> m(kPageSize, 0);
        size_t got = std::fread(m.data(), 1, kPageSize, fp);
        std::fclose(fp);
        if (got < 20) throw std::runtime_error("文件太小，不是合法的 minidb 文件");
        if (getU32(m.data()) != kMagic) throw std::runtime_error("magic 不匹配，这不是 minidb 文件");
        if (fnv1a(m.data(), 12) != getU32(m.data() + 16)) throw std::runtime_error("元数据页 checksum 失败");
        if (getU32(m.data() + 8) != kPageSize) throw std::runtime_error("页尺寸不兼容");
        if (getU32(m.data() + 4) != kVersion) throw std::runtime_error("版本不兼容");
        page_count_ = getU32(m.data() + 12);
    }

    std::string path_;
    size_t cap_;
    uint32_t page_count_ = 0;
    std::list<Entry> lru_;
    std::unordered_map<uint32_t, std::list<Entry>::iterator> idx_;
    Stats st_;
};

// ---------------------------------------------------------------------------
// 行格式与 RID：与阶段 2 完全一致（users 表：id INT64 / name TEXT / age 可空 / bio 可空）
// ---------------------------------------------------------------------------
struct Row {
    int64_t id = 0;
    std::string name;
    std::optional<int32_t> age;
    std::optional<std::string> bio;
};
constexpr uint16_t kColCount = 4;

uint16_t encode(const Row &r, uint8_t *out) {
    uint8_t *base = out;
    out += 2;
    putU16(out, kColCount); out += 2;
    uint8_t nullmap = 0;
    if (!r.age) nullmap |= 1u << 2;
    if (!r.bio) nullmap |= 1u << 3;
    *out++ = nullmap;
    uint8_t *offs = out;
    out += 2 * kColCount;
    uint8_t tmp[8];
    auto putFixed = [&](int col, const uint8_t *src, size_t n) {
        putU16(offs + 2 * col, uint16_t(out - base));
        std::memcpy(out, src, n);
        out += n;
    };
    putU64(tmp, static_cast<uint64_t>(r.id));
    putFixed(0, tmp, 8);
    putFixed(1, reinterpret_cast<const uint8_t *>(r.name.data()), r.name.size());
    if (r.age) { putU32(tmp, static_cast<uint32_t>(*r.age)); putFixed(2, tmp, 4); }
    else putU16(offs + 4, uint16_t(out - base));
    if (r.bio) putFixed(3, reinterpret_cast<const uint8_t *>(r.bio->data()), r.bio->size());
    else putU16(offs + 6, uint16_t(out - base));
    putU16(base, uint16_t(out - base));
    return uint16_t(out - base);
}

Row decode(const uint8_t *rec) {
    Row r;
    const uint16_t total = getU16(rec);
    const uint8_t nullmap = rec[4];
    const uint8_t *offs = rec + 5;
    auto span = [&](int col) -> std::pair<const uint8_t *, size_t> {
        const uint16_t b = getU16(offs + 2 * col);
        const uint16_t e = (col + 1 < kColCount) ? getU16(offs + 2 * (col + 1)) : total;
        return {rec + b, size_t(e - b)};
    };
    r.id = static_cast<int64_t>(getU64(span(0).first));
    { auto s = span(1); r.name.assign(reinterpret_cast<const char *>(s.first), s.second); }
    if (!(nullmap & (1u << 2))) r.age = static_cast<int32_t>(getU32(span(2).first));
    if (!(nullmap & (1u << 3))) { auto s = span(3); r.bio = std::string(reinterpret_cast<const char *>(s.first), s.second); }
    return r;
}

uint16_t encodedSize(const Row &r) {
    return uint16_t(2 + 2 + 1 + 2 * kColCount + 8 + r.name.size() + (r.age ? 4u : 0u) +
                    (r.bio ? r.bio->size() : 0u));
}

struct RID {
    uint32_t page = 0;
    uint16_t slot = 0;
    bool operator==(const RID &o) const { return page == o.page && slot == o.slot; }
    bool operator<(const RID &o) const { return key() < o.key(); }
    uint64_t key() const { return (uint64_t(page) << 16) | slot; }
    std::string str() const { return "(" + std::to_string(page) + "," + std::to_string(slot) + ")"; }
};

// ---------------------------------------------------------------------------
// HeapTable（阶段 2 的插入/点查/扫描部分。本阶段不演示删除，碎片整理分支略去，
// 行为与阶段 2 在无删除场景下完全一致）
// ---------------------------------------------------------------------------
class HeapTable {
public:
    HeapTable(const std::string &path, size_t cachePages) : pg_(path, cachePages, true) {}

    static uint16_t slotCount(const std::vector<uint8_t> &p) { return getU16(p.data()); }
    static uint32_t slotOf(const std::vector<uint8_t> &p, uint16_t i) {
        return getU32(p.data() + kPageHeader + i * kSlotSize);
    }
    static uint32_t dataStart(const std::vector<uint8_t> &p) {
        uint32_t start = kPageSize;
        const uint16_t n = slotCount(p);
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t off = slotOf(p, i) & 0x7FFFFFFFu;
            if (off < start) start = off;
        }
        return start;
    }
    static uint32_t freeSpace(const std::vector<uint8_t> &p) {
        const uint32_t dirEnd = kPageHeader + (uint32_t(slotCount(p)) + 1) * kSlotSize;
        return dataStart(p) > dirEnd ? dataStart(p) - dirEnd : 0;
    }

    RID insert(const Row &row) {
        std::vector<uint8_t> rec(encodedSize(row));
        const uint16_t len = encode(row, rec.data());
        for (;;) {
            if (!room_.empty()) {
                const uint32_t pid = room_.back();
                auto page = pg_.readPage(pid);
                if (freeSpace(page) >= len) {
                    place(page, rec.data(), len);
                    pg_.writePage(pid, page);
                    return RID{pid, lastSlot_};
                }
                room_.pop_back();
                continue;
            }
            const uint32_t pid = pg_.allocPage();
            auto page = pg_.readPage(pid);
            place(page, rec.data(), len);
            pg_.writePage(pid, page);
            room_.push_back(pid);
            return RID{pid, lastSlot_};
        }
    }

    std::optional<Row> get(const RID &rid) {
        if (rid.page < kFirstDataPage || rid.page >= pg_.pageCount()) return std::nullopt;
        auto page = pg_.readPage(rid.page);
        if (rid.slot >= slotCount(page)) return std::nullopt;
        const uint32_t s = slotOf(page, rid.slot);
        if (s & kSlotDeleted) return std::nullopt;
        return decode(page.data() + (s & 0x7FFFFFFFu));
    }

    std::vector<std::pair<RID, Row>> scan() {
        std::vector<std::pair<RID, Row>> out;
        for (uint32_t pid = kFirstDataPage; pid < pg_.pageCount(); ++pid) {
            auto page = pg_.readPage(pid);
            const uint16_t n = slotCount(page);
            for (uint16_t s = 0; s < n; ++s) {
                const uint32_t v = slotOf(page, s);
                if (v & kSlotDeleted) continue;
                out.emplace_back(RID{pid, s}, decode(page.data() + (v & 0x7FFFFFFFu)));
            }
        }
        return out;
    }

    void flush() { pg_.flush(); }
    uint32_t dataPages() const { return pg_.dataPages(); }
    size_t fileBytes() const { return pg_.fileBytes(); }
    const Pager::Stats &pager() const { return pg_.stats(); }

private:
    // 顺序要命：先算落点，再动 slot_count（同阶段 2 的教训，注释原样保留）
    void place(std::vector<uint8_t> &page, const uint8_t *rec, uint16_t len) {
        const uint16_t n = slotCount(page);
        const uint32_t at = dataStart(page) - len;
        std::memcpy(page.data() + at, rec, len);
        putU32(page.data() + kPageHeader + n * kSlotSize, at);
        putU16(page.data(), uint16_t(n + 1));
        lastSlot_ = n;
    }

    Pager pg_;
    std::vector<uint32_t> room_;
    uint16_t lastSlot_ = 0;
};

// ---------------------------------------------------------------------------
// BPlusTree：落盘的 B+ 树，key 是 int64，value 是 RID（非唯一：同 key 多条）
//
// 文件布局：页 0 = 数据库元数据（Pager 管），页 1 = 索引头（root/height/统计），
// 页 2 起 = 节点页。索引头页自带 checksum，坏了宁可拒绝打开也不带病干活。
// ---------------------------------------------------------------------------
class BPlusTree {
public:
    struct Stats {
        uint64_t leafSplits = 0, intSplits = 0, lookups = 0, rangeScans = 0;
    };
    struct LookupInfo {
        uint32_t pagesAccessed = 0;          // 本次查询访问的节点页数（含缓存命中）
        std::vector<uint32_t> perLevel;      // 每层访问次数（0 = 根层）
    };
    struct RangeInfo {
        uint32_t leafPagesVisited = 0;       // 沿链表走过的叶子页数
        uint64_t entriesExamined = 0;        // 检查过的叶子条目数
        uint32_t treePagesAccessed = 0;      // 下探阶段访问的页数
    };

    BPlusTree(const std::string &path, size_t cap) : pg_(path, cap, true) {
        if (pg_.pageCount() <= kIdxHeaderPage) {
            // 全新索引：页 1 = 索引头，页 2 = 根（先当叶子）
            pg_.allocPage();                       // 1
            root_ = pg_.allocPage();               // 2
            auto rp = pg_.readPage(root_);
            writeLeaf(rp, {}, 0, 0);
            pg_.writePage(root_, rp);
            height_ = 1; count_ = 0; leafCnt_ = 1; intCnt_ = 0;
            saveHeader();
        } else {
            loadHeader();
        }
    }

    void insert(int64_t key, RID rid) {
        Split s = insertInto(root_, key, rid);
        if (s.didSplit) {
            const uint32_t nroot = pg_.allocPage();
            ++intCnt_;
            auto p = pg_.readPage(nroot);
            writeInt(p, root_, {{s.upKey, s.rightPid}});
            pg_.writePage(nroot, p);
            root_ = nroot;
            ++height_;
        }
        ++count_;
        saveHeader();
    }

    std::vector<RID> lookup(int64_t key, LookupInfo *info) {
        std::vector<RID> out;
        if (info) info->pagesAccessed = 0;
        ++st_.lookups;
        uint32_t pid = root_;
        for (uint32_t lvl = 0; lvl + 1 < height_; ++lvl) {
            auto page = pg_.readPage(pid);
            if (info) {
                ++info->pagesAccessed;
                if (info->perLevel.size() <= lvl) info->perLevel.resize(lvl + 1, 0);
                ++info->perLevel[lvl];
            }
            uint32_t child0 = 0;
            auto ps = readIntPairs(page, &child0);
            size_t t = 0;
            while (t < ps.size() && ps[t].key < key) ++t;   // 搜索规则：等于往左
            pid = (t == 0) ? child0 : ps[t - 1].child;
        }
        // 落到"可能出现这个 key 的最左叶子"，再沿链表向右收：同一个 key 的一串条目
        // 完全可能横跨好几片叶子（age=21 有 800 多行，一片叶子才装 291 条）。
        // 撞到比 key 大的条目就说明这一串结束了，立刻收工。
        while (pid != 0) {
            auto page = pg_.readPage(pid);
            if (getU16(page.data()) != kLeafKind) return out;   // 树高与结构不符（不应该）
            if (info) {
                ++info->pagesAccessed;
                const uint32_t lvl = height_ - 1;
                if (info->perLevel.size() <= lvl) info->perLevel.resize(lvl + 1, 0);
                ++info->perLevel[lvl];
            }
            auto es = readLeaf(page);
            size_t pos = 0;
            while (pos < es.size() && es[pos].key < key) ++pos;
            for (; pos < es.size(); ++pos) {
                if (es[pos].key > key) return out;
                out.push_back(RID{es[pos].ridPage, es[pos].ridSlot});
            }
            pid = getU32(page.data() + 8);
        }
        return out;
    }

    std::vector<std::pair<int64_t, RID>> range(int64_t lo, int64_t hi, RangeInfo *info) {
        std::vector<std::pair<int64_t, RID>> out;
        if (info) { info->leafPagesVisited = 0; info->entriesExamined = 0; info->treePagesAccessed = 0; }
        ++st_.rangeScans;
        // 下探到包含 lo 的叶子
        uint32_t pid = root_;
        for (uint32_t lvl = 0; lvl + 1 < height_; ++lvl) {
            auto page = pg_.readPage(pid);
            if (info) ++info->treePagesAccessed;
            uint32_t child0 = 0;
            auto ps = readIntPairs(page, &child0);
            size_t t = 0;
            while (t < ps.size() && ps[t].key < lo) ++t;      // 搜索规则：等于往左
            pid = (t == 0) ? child0 : ps[t - 1].child;
        }
        bool first = true;
        while (pid != 0) {
            auto page = pg_.readPage(pid);
            auto es = readLeaf(page);
            if (info) ++info->leafPagesVisited;
            size_t pos = 0;
            if (first) {
                while (pos < es.size() && es[pos].key < lo) ++pos;
                first = false;
            }
            for (; pos < es.size(); ++pos) {
                if (info) ++info->entriesExamined;
                if (es[pos].key > hi) return out;      // 叶子有序：越过右界立刻收工
                out.emplace_back(es[pos].key, RID{es[pos].ridPage, es[pos].ridSlot});
            }
            pid = getU32(page.data() + 8);             // 关键一步：沿右兄弟指针走
        }
        return out;
    }

    std::vector<std::pair<int64_t, RID>> dumpAll() {
        std::vector<std::pair<int64_t, RID>> out;
        uint32_t pid = root_;
        while (true) {
            auto page = pg_.readPage(pid);
            if (getU16(page.data()) == kLeafKind) break;
            uint32_t child0 = 0;
            (void)readIntPairs(page, &child0);
            pid = child0;
        }
        while (pid != 0) {
            auto page = pg_.readPage(pid);
            for (auto &e : readLeaf(page)) out.emplace_back(e.key, RID{e.ridPage, e.ridSlot});
            pid = getU32(page.data() + 8);
        }
        return out;
    }

    void flush() { saveHeader(); pg_.flush(); }
    uint32_t height() const { return height_; }
    uint64_t entries() const { return count_; }
    uint32_t leafPages() const { return leafCnt_; }
    uint32_t intPages() const { return intCnt_; }
    const Stats &stats() const { return st_; }
    size_t fileBytes() const { return pg_.fileBytes(); }
    uint64_t diskReads() const { return pg_.stats().reads; }
    uint64_t cacheHits() const { return pg_.stats().hits; }

private:
    struct IEntry { int64_t key; uint32_t ridPage; uint16_t ridSlot; };
    struct IPair { int64_t key; uint32_t child; };
    struct Split { bool didSplit = false; int64_t upKey = 0; uint32_t rightPid = 0; };

    static std::vector<IEntry> readLeaf(const std::vector<uint8_t> &p) {
        std::vector<IEntry> es;
        const uint16_t n = getU16(p.data() + 2);
        es.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t *q = p.data() + kLeafHdr + size_t(i) * kLeafEnt;
            es.push_back(IEntry{static_cast<int64_t>(getU64(q)), getU32(q + 8), getU16(q + 12)});
        }
        return es;
    }
    static void writeLeaf(std::vector<uint8_t> &p, const std::vector<IEntry> &es,
                          uint32_t left, uint32_t right) {
        std::fill(p.begin(), p.end(), uint8_t(0));
        putU16(p.data() + 0, uint16_t(kLeafKind));
        putU16(p.data() + 2, uint16_t(es.size()));
        putU32(p.data() + 4, left);
        putU32(p.data() + 8, right);
        for (size_t i = 0; i < es.size(); ++i) {
            uint8_t *q = p.data() + kLeafHdr + i * kLeafEnt;
            putU64(q, static_cast<uint64_t>(es[i].key));
            putU32(q + 8, es[i].ridPage);
            putU16(q + 12, es[i].ridSlot);
        }
    }
    static std::vector<IPair> readIntPairs(const std::vector<uint8_t> &p, uint32_t *child0) {
        std::vector<IPair> ps;
        const uint16_t n = getU16(p.data() + 2);
        *child0 = getU32(p.data() + 4);
        ps.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            const uint8_t *q = p.data() + kIntHdr + size_t(i) * kIntEnt;
            ps.push_back(IPair{static_cast<int64_t>(getU64(q)), getU32(q + 8)});
        }
        return ps;
    }
    static void writeInt(std::vector<uint8_t> &p, uint32_t child0, const std::vector<IPair> &ps) {
        std::fill(p.begin(), p.end(), uint8_t(0));
        putU16(p.data() + 0, uint16_t(kIntKind));
        putU16(p.data() + 2, uint16_t(ps.size()));
        putU32(p.data() + 4, child0);
        for (size_t i = 0; i < ps.size(); ++i) {
            uint8_t *q = p.data() + kIntHdr + i * kIntEnt;
            putU64(q, static_cast<uint64_t>(ps[i].key));
            putU32(q + 8, ps[i].child);
        }
    }

    // 递归下探插入。返回 Split 表示"我分裂了，请把我哥登记到你名下"。
    // 语义（本实现选定并贯彻始终）：分隔符 = 右子树的最小 key（下界），
    // child[i] 恰好覆盖 [key[i-1], key[i])。下降分两种、各管各的：
    //   插入下降：跳到第一个满足 key < key[j] 的 j（"等于往右"）→ 落最右合法叶；
    //   搜索下降：停在第一个满足 key[j] >= key 的 j（"等于往左"）→ 落最左合法叶。
    // 为什么要两套：非唯一索引里同一个 key 的一串条目可能横跨多片叶子，
    // 搜索只有落到最左那片、再沿链表向右收，才能把整串 RID 一个不落。
    // 第一版实现只写了"等于往右"，age 对账当场就挂了 0/8 —— 这不是事故，是教材。
    Split insertInto(uint32_t pid, int64_t key, RID rid) {
        auto page = pg_.readPage(pid);
        if (getU16(page.data()) == kLeafKind) {
            auto es = readLeaf(page);
            const uint32_t left = getU32(page.data() + 4), right = getU32(page.data() + 8);
            size_t pos = 0;
            while (pos < es.size() && es[pos].key < key) ++pos;
            es.insert(es.begin() + pos, IEntry{key, rid.page, rid.slot});
            if (es.size() <= kLmax) {
                writeLeaf(page, es, left, right);
                pg_.writePage(pid, page);
                return {};
            }
            // 叶子满分裂：292 条对半分，哥两个各 146 条
            const size_t mid = es.size() / 2;
            const uint32_t rightPid = pg_.allocPage();
            ++leafCnt_;
            writeLeaf(page, std::vector<IEntry>(es.begin(), es.begin() + mid), left, rightPid);
            pg_.writePage(pid, page);
            auto rp = pg_.readPage(rightPid);
            writeLeaf(rp, std::vector<IEntry>(es.begin() + mid, es.end()), pid, right);
            pg_.writePage(rightPid, rp);
            if (right != 0) {                       // 修补链表：老右兄弟的左指针改指新页
                auto op = pg_.readPage(right);
                putU32(op.data() + 4, rightPid);
                pg_.writePage(right, op);
            }
            ++st_.leafSplits;
            return Split{true, es[mid].key, rightPid};
        }
        uint32_t child0 = 0;
        auto ps = readIntPairs(page, &child0);
        size_t t = 0;
        while (t < ps.size() && !(key < ps[t].key)) ++t;
        const uint32_t childPid = (t == 0) ? child0 : ps[t - 1].child;
        Split s = insertInto(childPid, key, rid);
        if (!s.didSplit) return {};                 // 儿子没分裂，我这页一个字没动，不重写
        ps.insert(ps.begin() + t, IPair{s.upKey, s.rightPid});
        if (ps.size() <= kImax) {
            writeInt(page, child0, ps);
            pg_.writePage(pid, page);
            return {};
        }
        // 内部页满分裂：341 个 pair，中间那个 key 上推，不留在任何一页里（B+ 树特征）
        const size_t mid = ps.size() / 2;
        const uint32_t rightPid = pg_.allocPage();
        ++intCnt_;
        writeInt(page, child0, std::vector<IPair>(ps.begin(), ps.begin() + mid));
        pg_.writePage(pid, page);
        auto rp = pg_.readPage(rightPid);
        writeInt(rp, ps[mid].child, std::vector<IPair>(ps.begin() + mid + 1, ps.end()));
        pg_.writePage(rightPid, rp);
        ++st_.intSplits;
        return Split{true, ps[mid].key, rightPid};
    }

    void saveHeader() {
        auto p = pg_.readPage(kIdxHeaderPage);
        std::fill(p.begin(), p.end(), uint8_t(0));
        putU32(p.data() + 0, kIdxMagic);
        putU32(p.data() + 4, root_);
        putU32(p.data() + 8, height_);
        putU64(p.data() + 12, count_);
        putU32(p.data() + 20, leafCnt_);
        putU32(p.data() + 24, intCnt_);
        putU32(p.data() + 28, fnv1a(p.data(), 28));
        pg_.writePage(kIdxHeaderPage, p);
    }

    void loadHeader() {
        auto p = pg_.readPage(kIdxHeaderPage);
        if (getU32(p.data()) != kIdxMagic)
            throw std::runtime_error("索引头页 magic 不匹配");
        if (fnv1a(p.data(), 28) != getU32(p.data() + 28))
            throw std::runtime_error("索引头页 checksum 失败，索引文件损坏");
        root_ = getU32(p.data() + 4);
        height_ = getU32(p.data() + 8);
        count_ = getU64(p.data() + 12);
        leafCnt_ = getU32(p.data() + 20);
        intCnt_ = getU32(p.data() + 24);
    }

    Pager pg_;
    uint32_t root_ = 0, height_ = 0, leafCnt_ = 0, intCnt_ = 0;
    uint64_t count_ = 0;
    Stats st_;
};

// ---------------------------------------------------------------------------
// users 表的测试数据：与阶段 2 同分布。age = 20 + i%50，i%7==0 的行 age 为 NULL
// （NULL 不进二级索引 —— SQL 语义：IS NULL 不走这个索引）
// ---------------------------------------------------------------------------
Row makeRow(int64_t i) {
    Row r;
    r.id = i;
    r.name = "user_" + std::to_string(i);
    if (i % 7 != 0) r.age = int32_t(20 + (i % 50));
    if (i % 3 == 0) r.bio = "师傅手搓的第" + std::to_string(i) + "条备注，长度随内容变";
    return r;
}

// 确定性洗牌（固定种子，同阶段 1 的做法：可复现的"随机"才叫测试）
std::vector<int> shuffledRange(int n, uint32_t seed) {
    std::vector<int> a(n);
    for (int i = 0; i < n; ++i) a[size_t(i)] = i;
    uint32_t s = seed;
    for (int i = n - 1; i > 0; --i) {
        s = s * 1664525u + 1013904223u;
        std::swap(a[size_t(i)], a[size_t((s >> 8) % uint32_t(i + 1))]);
    }
    return a;
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int run() {
    const std::string heapDb = "p3_heap.db", idIdxDb = "p3_idx_id.db", ageIdxDb = "p3_idx_age.db";
    std::remove(heapDb.c_str()); std::remove(idIdxDb.c_str()); std::remove(ageIdxDb.c_str());
    const int N = 50000;
    int rc = 0;

    hr("① 磁盘布局实算：扇出是从 4096 里抠出来的");
    std::cout << "  叶子条目 14 字节（i64 key + u32 页号 + u16 槽号），叶页头 12 字节 → 每叶 "
              << kLmax << " 条\n";
    std::cout << "  内部条目 12 字节（i64 key + u32 儿子页），内部页头 8 字节 → 每内部页 "
              << kImax << " 个 key / " << (kImax + 1) << " 个儿子\n";
    std::cout << "  算术推论：高度 2 最多装 " << (uint64_t(kImax + 1) * kLmax)
              << " 条；高度 3 最多装 " << (uint64_t(kImax + 1) * (kImax + 1) * kLmax / 10000) << " 万条。\n";

    hr("② 灌 users 堆表：50000 行变长记录");
    std::vector<RID> rids;
    rids.reserve(size_t(N));
    HeapTable heap(heapDb, 64);
    double t0 = nowMs();
    for (int i = 0; i < N; ++i) rids.push_back(heap.insert(makeRow(int64_t(i))));
    const double tHeap = nowMs() - t0;
    heap.flush();
    char buf[96];
    std::snprintf(buf, sizeof buf, "%llu 行/秒",
                  static_cast<unsigned long long>(double(N) / (tHeap / 1000.0)));
    std::cout << "  插入 " << N << " 行耗时 " << ms(tHeap) << "（" << buf << "）\n";
    std::cout << "  堆表数据页 " << heap.dataPages() << " 页，文件 "
              << (heap.fileBytes() >> 10) << " KB\n";

    hr("③ 建两棵 B+ 树：id 索引（近似唯一）+ age 索引（非唯一，NULL 不进）");
    // 乱序插入才接近真实负载，顺序插入会让"最右叶子反复分裂"掩盖真实形态
    const std::vector<int> order = shuffledRange(N, 20261012u);
    BPlusTree idIdx(idIdxDb, 256);
    t0 = nowMs();
    for (int v : order) idIdx.insert(int64_t(v), rids[size_t(v)]);
    const double tIdIdx = nowMs() - t0;
    idIdx.flush();

    BPlusTree ageIdx(ageIdxDb, 256);
    size_t nAge = 0;
    t0 = nowMs();
    for (int i = 0; i < N; ++i) {
        if (i % 7 != 0) { ageIdx.insert(int64_t(20 + (i % 50)), rids[size_t(i)]); ++nAge; }
    }
    const double tAgeIdx = nowMs() - t0;
    ageIdx.flush();

    std::cout << "  id  索引：建树 " << ms(tIdIdx) << "，高 " << idIdx.height() << " 层，叶子 "
              << idIdx.leafPages() << " 页，内部 " << idIdx.intPages() << " 页，文件 "
              << (idIdx.fileBytes() >> 10) << " KB\n";
    std::cout << "      叶子分裂 " << idIdx.stats().leafSplits << " 次，内部分裂 "
              << idIdx.stats().intSplits << " 次（对比 " << N << " 次插入 —— 分裂只占 "
              << pct(double(idIdx.stats().leafSplits + idIdx.stats().intSplits), double(N))
              << "，均摊下来便宜得很）\n";
    std::cout << "  age 索引：建树 " << ms(tAgeIdx) << "，" << nAge << " 条（NULL age 的 "
              << (N - int(nAge)) << " 行不进索引），高 " << ageIdx.height() << " 层，叶子 "
              << ageIdx.leafPages() << " 页，内部 " << ageIdx.intPages() << " 页\n";

    hr("④ 随机点查 5000 次：B+ 树下降 vs 每次全表扫描");
    // 先把"全表扫描 + 过滤"每次的真实开销量出来（跑 3 次取平均，绝不心算外推）
    double heapScanMs = 0.0;
    uint64_t heapPageTouches = 0;
    size_t scanSink = 0;               // 防止编译器把过滤循环优化没
    const int kScanRuns = 3;
    for (int r = 0; r < kScanRuns; ++r) {
        const Pager::Stats s0 = heap.pager();
        const double a = nowMs();
        auto rows = heap.scan();
        const int64_t want = int64_t(order[size_t(r)]);
        size_t hits = 0;
        for (auto &e : rows) hits += (e.second.id == want) ? 1u : 0u;   // 模拟过滤判断
        scanSink += hits;
        heapScanMs += nowMs() - a;
        const Pager::Stats s1 = heap.pager();
        heapPageTouches = (s1.hits + s1.misses) - (s0.hits + s0.misses);
    }
    heapScanMs /= double(kScanRuns);

    const std::vector<int> probe = shuffledRange(N, 777u);
    const int Q = 5000;
    uint64_t pagesTotal = 0, lvlSum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t pointOk = 0;
    const uint64_t ch0 = idIdx.cacheHits(), rd0 = idIdx.diskReads();
    t0 = nowMs();
    for (int q = 0; q < Q; ++q) {
        const int64_t k = int64_t(probe[size_t(q)]);
        BPlusTree::LookupInfo li;
        const auto v = idIdx.lookup(k, &li);
        bool good = (v.size() == 1 && v[0] == rids[size_t(k)]);
        const auto row = good ? heap.get(v[0]) : std::nullopt;   // 二级索引最终要回表
        if (good && row && row->id == k) ++pointOk;
        pagesTotal += li.pagesAccessed;
        for (size_t j = 0; j < li.perLevel.size() && j < 8; ++j) lvlSum[j] += li.perLevel[j];
    }
    const double idxMs = nowMs() - t0;
    const uint64_t ch1 = idIdx.cacheHits(), rd1 = idIdx.diskReads();
    const double perQ = idxMs / double(Q);
    std::cout << "  " << padR("方式", 22) << padR("单次耗时", 13) << padR("单次读页", 12) << "备注\n";
    std::cout << "  " << padR("全表扫描+过滤", 22) << padR(ms(heapScanMs), 13)
              << padR(std::to_string(heapPageTouches), 12) << "树高无关，随数据量线性涨\n";
    std::cout << "  " << padR("B+ 树点查(含回表)", 22) << padR(ms(perQ), 13)
              << padR(std::to_string(pagesTotal / uint64_t(Q)), 12)
              << "树高 " << idIdx.height() << "，读页与数据量几乎无关\n";
    std::cout << "  （计时跑内过滤循环命中 " << scanSink << " 次，保证循环没被优化掉）\n";
    std::cout << "  每层平均读页：";
    for (uint32_t lvl = 0; lvl < idIdx.height(); ++lvl)
        std::cout << "第" << (lvl + 1) << "层 " << double(lvlSum[lvl]) / double(Q) << " 页  ";
    std::cout << "\n  ★ 加速比 = " << std::fixed << std::setprecision(0)
              << (heapScanMs / perQ) << " 倍（" << ms(heapScanMs) << " / " << ms(perQ)
              << "）；点查全部 " << Q << " 次共 " << ms(idxMs)
              << "，正确 " << pointOk << "/" << Q << (pointOk == size_t(Q) ? " ✓" : " ✗") << "\n";
    std::cout << "  索引层页缓存命中率 " << pct(double(ch1 - ch0), double((ch1 - ch0) + (rd1 - rd0)))
              << " —— 热页留在缓存里，这是 B+ 树快的另一半原因\n";
    if (pointOk != size_t(Q)) rc |= 1;

    hr("⑤ 范围扫描 200 次：下探一次，然后沿叶子链表走");
    const int R = 200;
    const std::vector<int> rs = shuffledRange(N, 555u);
    uint64_t leafPg = 0, entEx = 0, treePg = 0, resCnt = 0;
    size_t rangeOk = 0;
    t0 = nowMs();
    for (int q = 0; q < R; ++q) {
        int64_t lo = int64_t(rs[size_t(q)]);
        const int64_t w = 200 + int64_t(rs[size_t((q * 7 + 1) % N)] % 601);   // 宽度 200~800
        int64_t hi = lo + w;
        if (hi >= N) hi = N - 1;
        BPlusTree::RangeInfo ri;
        const auto got = idIdx.range(lo, hi, &ri);
        // id 在 [0,N) 内连续，所以区间内每个值都存在：期望条数就是区间长度，逐次全对账
        if (got.size() == size_t(hi - lo + 1)) ++rangeOk;
        leafPg += ri.leafPagesVisited; entEx += ri.entriesExamined;
        treePg += ri.treePagesAccessed; resCnt += got.size();
    }
    const double rngMs = nowMs() - t0;
    const double rngPerQ = rngMs / double(R);
    std::cout << "  平均每次：结果 " << resCnt / uint64_t(R) << " 行，下探 "
              << double(treePg) / double(R) << " 页 + 走链表访叶 "
              << double(leafPg) / double(R) << " 页，检查条目 "
              << double(entEx) / double(R) << " 条\n";
    std::cout << "  全表扫描过滤每次 " << ms(heapScanMs) << "（读 " << heapPageTouches
              << " 页）；B+ 树每次 " << ms(rngPerQ) << "\n";
    std::cout << "  ★ 加速比 = " << (heapScanMs / rngPerQ) << " 倍；范围结果逐次对账 "
              << rangeOk << "/" << R << (rangeOk == size_t(R) ? " ✓" : " ✗") << "\n";
    if (rangeOk != size_t(R)) rc |= 1;

    hr("⑥ 正确性总账 + 重开文件（落盘的索引必须原样能用）");
    size_t ageOk = 0; const int32_t ageKeys[] = {21, 23, 26, 33, 41, 47, 55, 69};
    for (int32_t k : ageKeys) {
        BPlusTree::LookupInfo li;
        const auto v = ageIdx.lookup(int64_t(k), &li);
        size_t expect = 0;
        for (int i = 0; i < N; ++i)
            if (i % 7 != 0 && 20 + (i % 50) == int(k)) ++expect;
        if (v.size() == expect) ++ageOk;
    }
    std::cout << "  非唯一点查对账（age 值 → RID 数）：" << ageOk << "/"
              << (sizeof ageKeys / sizeof ageKeys[0])
              << (ageOk == sizeof ageKeys / sizeof ageKeys[0] ? " ✓" : " ✗") << "\n";
    if (ageOk != sizeof ageKeys / sizeof ageKeys[0]) rc |= 1;

    BPlusTree idIdx2(idIdxDb, 256);          // 真正从磁盘重开一棵树
    const auto all = idIdx2.dumpAll();
    bool ordered = (all.size() == size_t(N));
    for (size_t i = 0; ordered && i < all.size(); ++i)
        if (all[i].first != int64_t(i)) ordered = false;
    size_t backOk = 0;
    for (size_t i = 0; i < all.size(); i += 50) {
        const auto row = heap.get(all[i].second);
        if (row && row->id == all[i].first) ++backOk;
    }
    std::cout << "  重开后 dumpAll（沿叶子链表）：" << all.size() << " 条、全局有序 "
              << (ordered ? "✓" : "✗") << "；抽查回表 " << backOk << "/" << (all.size() / 50)
              << (backOk == all.size() / 50 ? " ✓" : " ✗") << "\n";
    if (!ordered || backOk != all.size() / 50) rc |= 1;

    std::cout << "\n  师傅收尾：树高 " << idIdx.height() << " 层就压住了 " << N
              << " 条索引项，点查读页是个位数、全表扫描读页是几千 —— 这就是索引的全部秘密。\n";
    std::cout << "  " << (rc == 0 ? "全部校验通过 ✓" : "有校验失败 ✗") << "\n";
    return rc;
}

}   // namespace

int main() {
    SetConsoleOutputCP(65001);
    const int rc = run();
    return rc;
}
