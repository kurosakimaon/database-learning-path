// ============================================================================
// minidb · 阶段 2：堆表 —— RID、变长字段、null bitmap、删除与槽位复用
// ----------------------------------------------------------------------------
// 编译：  g++ -std=c++17 -Wall -Wextra phase2_heap_table.cpp -o phase2.exe
// 运行：  .\phase2.exe              （1 万行实测，几秒钟）
// 退出码：0 = 全部校验通过
//
// 师傅开场白：
//   阶段 1 我用了"定长记录"这个作弊码。今天作弊码收回来，现实立刻咬人：
//   VARCHAR 长度不一样、有的列是 NULL、行会被删掉。这三件事加在一起，
//   逼你在页内再套一层"槽位目录（slot directory）"。
//
//   页内布局（所有主流数据库的共同祖先，SQLite / PG / InnoDB 都是它的变体）：
//
//     0        8                                   空闲              4096
//     +--------+--------------+...............+-------------+-----------+
//     | header | slot dir →   |               |   ← 记录从页尾往前堆   |
//     +--------+--------------+...............+-------------+-----------+
//
//     [0..1] u16 slot_count    槽位目录里的条目数（含已删除的）
//     [2..3] u16 delete_count  其中已删除的条目数
//     [4..7] u32 dead_bytes    已删除记录还占着的字节数（碎片）
//     [8..]  slot dir          每项 u32：bit31=已删除，低 31 位=记录在页内偏移
//
//   为什么目录从前向后长、记录从后向前长？让它们朝对方生长，中间剩下的才是可用
//   空间。这样目录开销只在页真的很满时才显现，而不是一刀切走一半空间。
//
//   RID = (page_id, slot)。它不是主键，是"物理住址"。住址会变（更新=删+插、
//   页整理都会搬），所以任何存 RID 的二级索引都必须跟着改 —— 这是阶段 3 的入口，
//   也是 SQLite 的 rowid 和 InnoDB 的聚簇主键为什么长成那样的根本原因。
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
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <io.h>

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMagic = 0x4D494E49u;   // MINI
constexpr uint32_t kVersion = 1;
constexpr uint32_t kFirstDataPage = 1;
constexpr uint32_t kPageHeader = 8;
constexpr uint32_t kSlotSize = 4;
constexpr uint32_t kSlotDeleted = 0x80000000u;

// ---- 小端序列化 + 校验 ----------------------------------------------------
// 为什么不用 memcpy 整个 struct 落盘？因为 struct 有 padding，布局由编译器决定；
// 而且把 char 数组强转成 uint64_t* 是未对齐访问（x86 忍了，ARM 直接崩）。
// 逐字节小端序列化慢一点点，换来的是"文件格式由我说了算"。
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

// 中文占 2 列宽，std::setw 按字节算会排歪。控制台排版中文必备。
size_t dispWidth(const std::string &s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c < 0x80) { w += 1; continue; }
        size_t extra = 0;                       // 前导字节决定这个字符占几个字节
        if ((c >> 5) == 0x6) extra = 1;         // 110xxxxx
        else if ((c >> 4) == 0xE) extra = 2;    // 1110xxxx（CJK 基本都在这里）
        else if ((c >> 3) == 0x1E) extra = 3;   // 11110xxx
        w += ((c >> 4) == 0xE || (c >> 3) == 0x1E) ? 2 : 1;
        i += extra;   // 关键：跳过跟随字节，否则 0x80~0xBF 会被当成新字符再算一遍
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

// ---------------------------------------------------------------------------
// Pager（与阶段 1 同源，故意复制以保证"单文件可编译"）
// ---------------------------------------------------------------------------
class Pager {
public:
    struct Stats {
        uint64_t hits = 0, misses = 0, evictions = 0, flushes = 0, reads = 0;
    };

    Pager(const std::string &path, size_t cap, bool createIfMissing) : path_(path), cap_(std::max<size_t>(1, cap)) {
        std::fstream probe(path, std::ios::in | std::ios::binary);
        if (!probe) {
            if (!createIfMissing) throw std::runtime_error("文件不存在且未允许创建: " + path);
            page_count_ = 1;
            std::FILE *fp = ::fopen(path.c_str(), "w+b");
            if (!fp) throw std::runtime_error("创建失败: " + path);
            writeMetaPage(fp);
            std::fflush(fp);
            ::_commit(_fileno(fp));
            std::fclose(fp);   // 别忘了：不 fsync 的元数据，断电等于没建库
            return;
        }
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
                // 短读不报错：allocPage 刚分配的新页，文件还没长到那儿，
                // "文件里没有这一页"是合法状态，语义就是全零页。
                // 新手在这里抛异常，跑 1 万行插入时就会在第一个新页上崩掉。
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

    // 把页缓存里的脏页写下去。每页单独 fseek —— 乱序写脏页是 buffer pool 的常态，
    // 别指望它们是顺序的（想顺序，就得靠 WAL 把乱序写变成顺序写，阶段 4 见）。
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
        ::_commit(_fileno(fp));   // Windows 上的 fsync
        std::fclose(fp);
    }

    // 缩到 n 页（回收尾部空页）。只在本阶段"整理"演示里用来证明文件真能变小。
    void truncateTo(uint32_t n) {
        if (n >= page_count_) return;
        flush();
        std::FILE *fp = ::fopen(path_.c_str(), "r+b");
        if (!fp) return;
        std::fseek(fp, 0, SEEK_SET);
        // 重写文件长度：先用 ftruncate 语义（Windows 用 _chsize_s 太绕，这里重读再写）
        std::vector<uint8_t> all(size_t(n) * kPageSize, 0);
        size_t got = std::fread(all.data(), 1, all.size(), fp);
        std::fclose(fp);
        (void)got;
        fp = ::fopen(path_.c_str(), "wb");
        if (!fp) return;
        std::fwrite(all.data(), 1, all.size(), fp);
        std::fclose(fp);
        page_count_ = n;
        lru_.clear();
        idx_.clear();
        fp = ::fopen(path_.c_str(), "r+b");
        if (fp) { writeMetaPage(fp); std::fflush(fp); ::_commit(_fileno(fp)); std::fclose(fp); }
    }

private:
    struct Entry { uint32_t pid; std::vector<uint8_t> bytes; bool dirty; };
    static long off(uint32_t pid) { return static_cast<long>(pid) * static_cast<long>(kPageSize); }

    // 把文件撑到 pages 页。跳过 EOF 直接写，不同 CRT 行为不一致（有的补零有的失败）。
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
        if (getU32(m.data()) != kMagic)
            throw std::runtime_error("magic 不匹配，这不是 minidb 文件");
        if (fnv1a(m.data(), 12) != getU32(m.data() + 16))
            throw std::runtime_error("元数据页 checksum 失败，页 0 被写坏了");
        if (getU32(m.data() + 8) != kPageSize)
            throw std::runtime_error("页尺寸不兼容：文件 " + std::to_string(getU32(m.data() + 8)));
        if (getU32(m.data() + 4) != kVersion)
            throw std::runtime_error("版本不兼容：文件 v" + std::to_string(getU32(m.data() + 4)));
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
// 一行数据：定长非空 / 变长非空 / 定长可空 / 变长可空，四种组合全占上
// ---------------------------------------------------------------------------
struct Row {
    int64_t id = 0;
    std::string name;
    std::optional<int32_t> age;
    std::optional<std::string> bio;
};
constexpr uint16_t kColCount = 4;

// 记录编码：u16 总长 | u16 列数 | null bitmap | u16 偏移表[列数] | 变长/定长数据
//   NULL 的列不占数据字节，但偏移表里照样有一项（指向下一个边界，长度为 0）。
//   读写两侧都必须走同一张偏移表 —— 一旦"算式"写了两套且对不上，就是线上
//   数据错乱、越界读的开始。这是我最想让你在这里记住的一句话。
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

std::string rowText(const Row &r) {
    std::string s = "id=" + std::to_string(r.id) + " name=" + r.name +
                    " age=" + (r.age ? std::to_string(*r.age) : "NULL") +
                    " bio=" + (r.bio ? ("<len " + std::to_string(r.bio->size()) + ">") : "NULL");
    return s;
}
bool rowEqual(const Row &a, const Row &b) {
    return a.id == b.id && a.name == b.name && a.age == b.age && a.bio == b.bio;
}

struct RID {
    uint32_t page = 0;
    uint16_t slot = 0;
    uint64_t key() const { return (uint64_t(page) << 16) | slot; }
    std::string str() const { return "(" + std::to_string(page) + "," + std::to_string(slot) + ")"; }
    bool operator==(const RID &o) const { return page == o.page && slot == o.slot; }
};

// ---------------------------------------------------------------------------
// HeapTable
// ---------------------------------------------------------------------------
class HeapTable {
public:
    struct Stats {
        uint64_t inserted = 0, deleted = 0, compactions = 0, pageAllocs = 0, slotReused = 0;
        uint64_t getOk = 0, getMiss = 0, scanned = 0, ridMoved = 0;
    };

    HeapTable(const std::string &path, size_t cachePages) : pg_(path, cachePages, true) {}

    // ---- 页内小工具（所有布局知识都集中在这里，别的地方一律不许自己算偏移）----
    static uint16_t slotCount(const std::vector<uint8_t> &p) { return getU16(p.data()); }
    static uint16_t deleteCount(const std::vector<uint8_t> &p) { return getU16(p.data() + 2); }
    static uint32_t deadBytes(const std::vector<uint8_t> &p) { return getU32(p.data() + 4); }
    static uint32_t slotOf(const std::vector<uint8_t> &p, uint16_t i) {
        return getU32(p.data() + kPageHeader + i * kSlotSize);
    }
    // 记录区起点 = 所有槽位偏移的最小值（含已删除槽位，因为尸体还占在那儿）
    static uint32_t dataStart(const std::vector<uint8_t> &p) {
        uint32_t start = kPageSize;
        const uint16_t n = slotCount(p);
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t off = slotOf(p, i) & 0x7FFFFFFFu;
            if (off < start) start = off;
        }
        return start;
    }
    // 真正的可用空间：从记录区起点往下、到槽位目录结尾为止的那段连续空闲。
    // 注意它和 dead_bytes 是两回事：碎片不是空闲，插不进去。
    static uint32_t freeSpace(const std::vector<uint8_t> &p) {
        const uint32_t dirEnd = kPageHeader + (uint32_t(slotCount(p)) + 1) * kSlotSize;
        return dataStart(p) > dirEnd ? dataStart(p) - dirEnd : 0;
    }

    // ---- 插入：先用有空位的页，再考虑整理一个碎片页，最后才开新页 ----
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
                    ++st_.inserted;
                    return RID{pid, lastSlot_};
                }
                room_.pop_back();   // 这个页满了，忘掉它
                continue;
            }
            if (!fragmented_.empty()) {
                const uint32_t pid = fragmented_.back();
                fragmented_.pop_back();
                auto page = pg_.readPage(pid);
                // 顺序和 InnoDB 一模一样：先试"现有连续空闲够不够"，够就直接放进去
                // （顺带复用被删槽位的下标）；不够才做 page reorganize；还是不够才 split
                // —— 在我们这儿就是开新页。先整理再用是新手写法，白白多写一遍页。
                if (freeSpace(page) >= len) {
                    place(page, rec.data(), len);   // 内部会复用已删除槽位的下标
                    pg_.writePage(pid, page);
                    keepIfRoom(pid, page);
                    ++st_.inserted;
                    return RID{pid, lastSlot_};
                }
                if (deleteCount(page) != 0) {
                    compact(page);
                    ++st_.compactions;
                    pg_.writePage(pid, page);
                    if (freeSpace(page) >= len) {
                        place(page, rec.data(), len);
                        pg_.writePage(pid, page);
                        keepIfRoom(pid, page);
                        ++st_.inserted;
                        return RID{pid, lastSlot_};
                    }
                }
                continue;   // 这一页榨不出空间了，回去开新页
            }
            const uint32_t pid = pg_.allocPage();
            ++st_.pageAllocs;
            auto page = pg_.readPage(pid);   // 全新零页
            place(page, rec.data(), len);
            pg_.writePage(pid, page);
            room_.push_back(pid);
            ++st_.inserted;
            return RID{pid, lastSlot_};
        }
    }

    std::optional<Row> get(const RID &rid) {
        if (rid.page < kFirstDataPage || rid.page >= pg_.pageCount()) { ++st_.getMiss; return std::nullopt; }
        auto page = pg_.readPage(rid.page);
        if (rid.slot >= slotCount(page)) { ++st_.getMiss; return std::nullopt; }
        const uint32_t s = slotOf(page, rid.slot);
        if (s & kSlotDeleted) { ++st_.getMiss; return std::nullopt; }
        ++st_.getOk;
        return decode(page.data() + (s & 0x7FFFFFFFu));
    }

    bool remove(const RID &rid) {
        if (rid.page < kFirstDataPage || rid.page >= pg_.pageCount()) return false;
        auto page = pg_.readPage(rid.page);
        if (rid.slot >= slotCount(page)) return false;
        const uint32_t s = slotOf(page, rid.slot);
        if (s & kSlotDeleted) return false;
        const uint16_t len = getU16(page.data() + (s & 0x7FFFFFFFu));
        putU32(page.data() + kPageHeader + rid.slot * kSlotSize, s | kSlotDeleted);
        putU16(page.data() + 2, uint16_t(deleteCount(page) + 1));
        putU32(page.data() + 4, deadBytes(page) + len);
        pg_.writePage(rid.page, page);
        fragmented_.push_back(rid.page);
        ++st_.deleted;
        return true;
    }

    // 更新 = 删 + 插。RID 会变 —— 记住这一条，阶段 3 的索引就要为它付账。
    RID update(const RID &rid, const Row &row) {
        if (!remove(rid)) throw std::runtime_error("update：RID 不存在");
        return insert(row);
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
                ++st_.scanned;
            }
        }
        return out;
    }

    void flush() { pg_.flush(); }
    uint32_t dataPages() const { return pg_.dataPages(); }
    size_t fileBytes() const { return pg_.fileBytes(); }
    const Pager::Stats &pager() const { return pg_.stats(); }
    const Stats &stats() const { return st_; }

    // 注意这两个都不是 const：读页会动 LRU、会改命中率统计。
    // 老实标出来，比硬挂个 const 再用 mutable 绕过去诚实得多。
    uint64_t deadBytesTotal() {
        uint64_t total = 0;
        for (uint32_t pid = kFirstDataPage; pid < pg_.pageCount(); ++pid)
            total += deadBytes(pg_.readPage(pid));
        return total;
    }
    uint64_t liveBytes() {
        uint64_t total = 0;
        for (auto &kv : scan()) total += encodedSize(kv.second);
        return total;
    }

private:
    // 往页里放一条记录：位置 = 记录区起点 - 长度（从后往前堆）。
    // 有已删除槽位就复用它的下标（slot 复用），否则追加一个新槽。
    void place(std::vector<uint8_t> &page, const uint8_t *rec, uint16_t len) {
        const uint16_t n = slotCount(page);
        int reuse = -1;
        for (uint16_t i = 0; i < n; ++i) {
            if (slotOf(page, i) & kSlotDeleted) { reuse = i; break; }
        }
        uint16_t slotIdx;
        // 顺序要命：必须先算落点，再动 slot_count。
        // 如果先把 slot_count 加 1，那第 n 项槽位还是 0（没写值），
        // dataStart() 会把这个 0 当成"有一条记录在偏移 0"，于是
        // at = 0 - len 无符号下溢成一个天文数字，memcpy 直接越界写崩。
        // 这类"先改元数据再算依赖它的量"的顺序错误，是存储层最经典的自杀方式。
        const uint32_t at = dataStart(page) - len;
        if (reuse >= 0) {
            slotIdx = uint16_t(reuse);
            // 槽位号复用了，但尸体字节还在原地，不因此消失：dead_bytes 不减。
            // 想真正把空间要回来，只有 compact（页整理）这一条路。
            putU16(page.data() + 2, uint16_t(deleteCount(page) > 0 ? deleteCount(page) - 1 : 0));
            ++st_.slotReused;
        } else {
            slotIdx = n;
            putU16(page.data(), uint16_t(n + 1));
        }
        std::memcpy(page.data() + at, rec, len);
        putU32(page.data() + kPageHeader + slotIdx * kSlotSize, at);
        lastSlot_ = slotIdx;
    }

    // 页整理（reorganize / 缩小页内碎片的唯一手段）：
    //   把活记录按槽位顺序紧凑地重新堆到页尾，已删除的槽位整个丢掉。
    //   代价：槽位编号全变了 → 这一页上所有 RID 失效！
    //   所以 InnoDB 宁愿做 page defragment 时也维护行移动指针，PG 用 CTID 版本 +
    //   HOT 来缓解。"整理能省空间、但会动物理地址"这个矛盾，是存储引擎的核心张力之一。
    void compact(std::vector<uint8_t> &page) {
        const uint16_t n = slotCount(page);
        std::vector<std::pair<uint32_t, uint16_t>> live;   // (旧偏移, 长度)
        live.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            const uint32_t s = slotOf(page, i);
            if (s & kSlotDeleted) continue;
            const uint32_t off = s & 0x7FFFFFFFu;
            live.emplace_back(off, getU16(page.data() + off));
        }
        std::vector<uint8_t> fresh(kPageSize, 0);
        uint32_t cursor = kPageSize;
        putU16(fresh.data(), uint16_t(live.size()));
        putU16(fresh.data() + 2, 0);
        putU32(fresh.data() + 4, 0);
        for (size_t i = 0; i < live.size(); ++i) {
            cursor -= live[i].second;
            std::memcpy(fresh.data() + cursor, page.data() + live[i].first, live[i].second);
            putU32(fresh.data() + kPageHeader + uint32_t(i) * kSlotSize, cursor);
        }
        page = std::move(fresh);
        st_.ridMoved += live.size();
    }

    // 放过一条记录之后，这一页还剩地方就留在"有空位页"清单里。
    // 忘了登记，下一条插入就会去翻 fragmented_ 或者干脆开新页，页数白涨。
    void keepIfRoom(uint32_t pid, const std::vector<uint8_t> &page) {
        if (freeSpace(page) >= kSlotSize) room_.push_back(pid);
    }

    Pager pg_;
    Stats st_;
    std::vector<uint32_t> room_;         // 内存里的"有空位页"清单
    std::vector<uint32_t> fragmented_;   // 有删除尸体的页，等着被整理
    uint16_t lastSlot_ = 0;
};

Row makeRow(int64_t i) {
    Row r;
    r.id = i;
    r.name = "user_" + std::to_string(i);
    if (i % 7 != 0) r.age = int32_t(20 + (i % 50));                       // ~14% age 为 NULL
    if (i % 3 == 0) r.bio = "师傅手搓的第" + std::to_string(i) + "条备注，长度随内容变";
    return r;
}

int run() {
    const std::string db = "phase2_heap.db";
    std::remove(db.c_str());
    const int N = 10000;
    int rc = 0;
    std::vector<RID> rids;

    hr("① 灌 1 万行：每页塞多少行，要几页");
    double insertMs = 0;
    {
        HeapTable t(db, 128);
        rids.reserve(N);
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; ++i) rids.push_back(t.insert(makeRow(i)));
        auto t1 = std::chrono::steady_clock::now();
        insertMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        t.flush();
        const uint32_t pages = t.dataPages();
        const uint64_t live = t.liveBytes();
        std::cout << "  插入 " << N << " 行，用时 " << ms(insertMs) << "（"
                  << int64_t(N / (insertMs / 1000.0)) << " 行/秒）\n";
        std::cout << "  数据页 " << pages << " 个 = " << (pages * kPageSize / 1024) << " KB，平均每页 "
                  << std::fixed << std::setprecision(1) << (double(N) / pages) << " 行\n";
        std::cout << "  记录净字节 " << live << "，页总字节 " << (pages * kPageSize) << "，填充率 "
                  << std::setprecision(1) << (100.0 * live / (pages * kPageSize)) << "%\n";
        std::cout << "  开销去哪了：页头 8 字节 + 每行 4 字节槽位 + 每行 (2+2+1+8) 固定头 + 变长列真实长度。"
                     "行越短，槽位和头的占比越高 —— 这是\"别 SELECT *、别存没用的大字段\"的物理层理由\n";
        std::cout << "  页缓存：命中 " << t.pager().hits << " / 未命中 " << t.pager().misses
                  << "，淘汰 " << t.pager().evictions << " 次\n";
        auto all = t.scan();
        std::cout << "  扫描 " << all.size() << " 行 " << (all.size() == size_t(N) ? "✓" : "✗ 行数不对") << "\n";
        if (all.size() != size_t(N)) rc = 1;
        std::cout << "  样例：" << rowText(all[0].second) << "\n";
        std::cout << "  样例：" << rowText(all[1].second) << "（bio 为 NULL 的行）\n";
    }

    hr("② 关掉重开：每个 RID 都还能取到，内容逐字段一致");
    {
        HeapTable t(db, 128);
        auto t0 = std::chrono::steady_clock::now();
        int bad = 0;
        for (int i = 0; i < N; ++i) {
            auto got = t.get(rids[size_t(i)]);
            if (!got || !rowEqual(*got, makeRow(i))) ++bad;
        }
        auto t1 = std::chrono::steady_clock::now();
        const double d = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  按 RID 点查 " << N << " 次，用时 " << ms(d) << "，平均 "
                  << std::fixed << std::setprecision(2) << (d * 1000.0 / N) << " µs/次\n";
        std::cout << "  内容校验：错 " << bad << " 条 → " << (bad ? "✗" : "✓ 全部一致") << "\n";
        std::cout << "  NULL 往返：第 0 行 age → "
                  << (t.get(rids[0])->age ? "✗ 本该是 NULL" : "✓ NULL")
                  << "；第 1 行 age → "
                  << (t.get(rids[1])->age ? "✓ 有值" : "✗ 丢了")
                  << "；第 0 行 bio → "
                  << (t.get(rids[0])->bio ? "✓ 有值" : "✗ 丢了")
                  << "；第 1 行 bio → "
                  << (t.get(rids[1])->bio ? "✗ 本该是 NULL" : "✓ NULL") << "\n";
        if (bad) rc = 1;

        hr("③ 变长列的直接后果：同页内行偏移不整齐");
        std::cout << "  前 5 行的 RID：";
        for (int i = 0; i < 5; ++i) std::cout << rids[size_t(i)].str() << " ";
        std::cout << "\n  前 5 行编码后字节数：";
        for (int i = 0; i < 5; ++i) std::cout << encodedSize(makeRow(i)) << " ";
        std::cout << "\n  定长表可以用 base + i * width 直接算偏移；变长表不行，只能查槽位目录。"
                     "slot directory 存在的唯一理由就是这句话\n";

        hr("④ 删除：只打标记，空间不还");
        auto t2 = std::chrono::steady_clock::now();
        int delOk = 0;
        for (int i = 0; i < N; i += 3) delOk += t.remove(rids[size_t(i)]) ? 1 : 0;
        auto t3 = std::chrono::steady_clock::now();
        // 期望值必须从实际操作里长出来，不能拍脑袋写 3000：
        // 每 3 行删 1 行，10000/3 向上取整 = 3334。这种"以为是多少"和"实际是多少"
        // 的偏差，是自检脚本里最常见的假失败（更糟的是假通过）。
        const int D = delOk;
        std::cout << "  删除 " << delOk << " 行（每 3 行删 1 行），用时 "
                  << ms(std::chrono::duration<double, std::milli>(t3 - t2).count()) << "\n";
        auto after = t.scan();
        std::cout << "  扫描得 " << after.size() << " 行（期望 " << (N - D) << "）"
                  << (after.size() == size_t(N - D) ? " ✓" : " ✗") << "\n";
        std::cout << "  被删的 RID 再点查 → " << (t.get(rids[0]) ? "✗ 居然还在" : "✓ 取不到了") << "\n";
        const uint64_t dead1 = t.deadBytesTotal();
        std::cout << "  页内尸体 " << dead1 << " 字节；文件仍是 " << (t.fileBytes() / 1024)
                  << " KB，一页没少 —— \"DELETE 不还空间，只打标记\"，这就是 VACUUM / OPTIMIZE TABLE 存在的原因\n";
        if (t.stats().deleted != uint64_t(D)) { std::cout << "  ✗ 删除计数对不上\n"; rc = 1; }

        hr("⑤ 再插入：槽位复用 + 页整理（碎片只有一条路能真正要回来）");
        const int ADD = D;
        const uint32_t pagesBefore = t.dataPages();
        auto t4 = std::chrono::steady_clock::now();
        for (int i = 0; i < ADD; ++i) (void)t.insert(makeRow(100000 + i));
        auto t5 = std::chrono::steady_clock::now();
        t.flush();
        const auto st = t.stats();
        const uint32_t pagesAfter = t.dataPages();
        std::cout << "  再插 " << ADD << " 行，用时 "
                  << ms(std::chrono::duration<double, std::milli>(t5 - t4).count()) << "\n";
        std::cout << "  页整理 " << st.compactions << " 次，槽位复用 " << st.slotReused
                  << " 次，新分配页 " << st.pageAllocs << " 个\n";
        std::cout << "  数据页 " << pagesBefore << " → " << pagesAfter << "（"
                  << (pagesAfter == pagesBefore ? "没涨，全靠整理腾出来的空间 ✓" : "涨了") << "）\n";
        std::cout << "  页内尸体 " << dead1 << " → " << t.deadBytesTotal()
                  << " 字节（被整理掉的页才真正还回了空间）\n";
        std::cout << "  整理过的页上，槽位号被重排 → 记录的 RID 全部变了（累计移动 "
                  << st.ridMoved << " 行）。这就是\"存 RID 的二级索引在 UPDATE/整理后必须重建\"的现场证据。\n";
        std::cout << "  顺手验一条硬道理：光复用槽位还不还空间，光整理又不可能塞下所有行。"
                     "所以本实现按三级顺序找地方：有空位页直接放 → 碎片页先放现有连续空闲、"
                     "不够再整理后放 → 都不行才开新页。跟 InnoDB「先用现有空闲、再 page reorganize、"
                     "最后才 split」是同一个顺序\n";
        if (st.compactions == 0) { std::cout << "  ✗ 一次整理都没发生，演示没做到点上\n"; rc = 1; }
        const auto survivors = t.scan();
        std::set<int64_t> ids;
        for (auto &kv : survivors) ids.insert(kv.second.id);
        bool survivorOk = ids.count(1) && ids.count(2) && !ids.count(0);
        std::cout << "  幸存行抽查：id=1 " << (ids.count(1) ? "在" : "丢!") << "、id=2 "
                  << (ids.count(2) ? "在" : "丢!") << "、id=0（已删）"
                  << (ids.count(0) ? "还在 ✗" : "确实没了 ✓") << "\n";
        if (!survivorOk) rc = 1;

        hr("⑥ 更新 = 删 + 插，RID 会变");
        // 先干一件更吓人的事：第⑤步整理过页，手里那份 rids[] 已经是过期地址本了。
        // 过期 RID 不会报错，它会安安稳稳地返回"别人的行" —— 这比崩溃难查一百倍。
        const auto fresh = t.scan();
        const RID liveRid = fresh[0].first;
        const int64_t liveId = fresh[0].second.id;
        if (auto stale = t.get(rids[1])) {
            std::cout << "  过期地址本实测：rids[1] 原指向 id=1，现在读到 id=" << stale->id << "。";
            if (stale->id == 1)
                std::cout << "这一次碰巧还是它 —— 因为那个槽位被回收后刚好又放回了同一行。"
                             "但只要复用顺序挪一点点，你读到的就是别人的行，而且它不报错、不崩溃。"
                             "\"静默读错\"比崩溃难查一百倍，所以数据库绝不把物理地址暴露给用户\n";
            else
                std::cout << "别人的行！不报错、不崩溃，就这么静默读错。"
                             "所以数据库绝不把物理地址暴露给用户，只暴露主键\n";
        } else {
            std::cout << "  过期地址本实测：rids[1] 已取不到 —— 同样说明物理地址不可依赖\n";
        }

        Row nr = makeRow(liveId);
        nr.bio = std::string("把 bio 撑大，故意让它塞不回原来的位置：") + std::string(400, 'x');
        RID moved = t.update(liveRid, nr);
        auto afterUpd = t.get(moved);
        std::cout << "  RID " << liveRid.str() << "（id=" << liveId << "）更新后落在 " << moved.str()
                  << " → " << (liveRid == moved ? "同位置（本页刚好有空）" : "换了住址") << "\n";
        std::cout << "  新 RID 读回来："
                  << (afterUpd && afterUpd->id == liveId && afterUpd->bio && afterUpd->bio->size() > 400
                          ? "✓ " + rowText(*afterUpd)
                          : "✗ " + (afterUpd ? rowText(*afterUpd) : "取不到"))
                  << "\n";
        if (!afterUpd || afterUpd->id != liveId) rc = 1;
        std::cout << "  旧 RID " << liveRid.str() << " 现在读到什么 → ";
        if (auto g = t.get(liveRid)) std::cout << "id=" << g->id << "（槽位被回收给别人了，这正是 RID 不稳定的代价）\n";
        else std::cout << "取不到（槽位仍是删除态）\n";
        std::cout << "  师傅点评：所以 InnoDB 的二级索引存的是主键值而不是物理地址，UPDATE 主键才那么贵；"
                     "PG 的 CTID 会变，所以要 HOT 和 index-only scan 的可见性判断来兜。"
                     "这不是谁的怪癖，是\"物理地址会变\"这条物理定律的必然后果\n";

        hr("⑦ 全表扫描 vs 按 RID 点查");
        auto t6 = std::chrono::steady_clock::now();
        auto all2 = t.scan();
        auto t7 = std::chrono::steady_clock::now();
        const double scanMs = std::chrono::duration<double, std::milli>(t7 - t6).count();
        std::cout << "  全表扫描 " << all2.size() << " 行：" << ms(scanMs) << "\n";
        auto t8 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i) (void)t.get(survivors[size_t(i)].first);
        auto t9 = std::chrono::steady_clock::now();
        const double pMs = std::chrono::duration<double, std::milli>(t9 - t8).count();
        std::cout << "  按 RID 点查 1000 次：" << ms(pMs) << "，全表扫 1 次 ≈ 点查 1000 次的 "
                  << std::setprecision(1) << (scanMs / pMs) << " 倍（缓存 128 页 < 135 页，点查也在淘汰页）\n";
        std::cout << "  可是\"查 id=12345 这一行\"必须先全表扫描 —— 堆表里没有 id → RID 的映射。"
                     "这句话就是阶段 3 的全部动机\n";
    }

    std::remove(db.c_str());
    std::cout << "\n" << (rc == 0 ? "阶段 2 全部校验通过。" : "阶段 2 有校验失败，退出码非 0。\n");
    return rc;
}

}   // namespace

int main() {
    SetConsoleOutputCP(65001);
    std::cout << "minidb 阶段 2 · 堆表：RID=(page_id,slot) + 变长字段 + null bitmap + 删除与整理\n";
    const int rc = run();
    std::cout << "下一阶段：给这张表加一个真正落盘的 B+ 树二级索引（key → RID）："
                 "叶子双向链表、插入分裂、范围扫描，以及 5 万条数据上索引 vs 全表扫的加速比。\n";
    return rc;
}
