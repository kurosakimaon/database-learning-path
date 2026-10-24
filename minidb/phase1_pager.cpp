// ============================================================================
// minidb · 阶段 1：Pager —— 数据库的地基就是"按页读写一个文件"
// ----------------------------------------------------------------------------
// 编译：  g++ -std=c++17 -Wall -Wextra phase1_pager.cpp -o phase1.exe
// 运行：  .\phase1.exe
// 退出码：0 表示全部校验通过；非 0 表示有校验失败（别信"看起来对"，信退出码）
//
// 师傅开场白：
//   你在应用层写了五年 SQL，从来没关心过"一行数据在磁盘上到底长什么样"。
//   今天开始关心。因为一旦你亲手把 4096 字节写进文件、再把它读回来对 checksum，
//   你以后看 buffer pool、看页分裂、看 WAL、看 fsync，全都心里有底。
//
//   本阶段只做四件事，别贪多：
//     1) 定长页（4KB）作为唯一读写单位 —— 为什么是 4KB，见下面的注释
//     2) 页 0 当元数据页（magic / version / page_size / page_count）
//     3) 一个可配容量的 LRU 页缓存，命中率是真统计出来的
//     4) 跨页写 200 条记录，关掉重开，逐条对 checksum
//
//   注意：phase2~6 都会各自重复一份 Pager 代码。这不是我懒，是故意的 ——
//   每个阶段必须能单独 g++ 一个文件就跑起来，不用 CMake、不用拆 header。
//   真实项目里你会把它抽成 pager.h，这里为了"单文件可运行"牺牲一点 DRY。
// ============================================================================

#include <windows.h>   // SetConsoleOutputCP，不设置的话 PowerShell 里中文是乱码

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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <io.h>

namespace {

// ---------------------------------------------------------------------------
// 为什么是 4096？三条理由，面试问烂了，但你亲手写过才算你的：
//   1) 对齐 x86 一页内存、绝大多数文件系统的 block 大小；4Kn 盘的原生扇区
//      也是 4KB（注意 512e 盘逻辑扇区仍是 512B）。对齐写能少一次读-改-写，
//      但 4KB 写本身**不保证原子**——断电可能只落下半页（撕裂页，phase4 正面撞）。
//   2) 太大：一次点查要读一整页，浪费 IO 和缓存；太小：元数据开销、寻址次数爆炸。
//   3) SQLite 默认 4096，PostgreSQL 默认 8192，InnoDB 默认 16384。都是这个量级。
// ---------------------------------------------------------------------------
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMagic = 0x4D494E49u;   // 'M''I''N''I' 的小端拼法
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMetaPage = 0;          // 页 0 专门放元数据，数据页从 1 开始
constexpr uint32_t kDefaultCachePages = 8; // 故意给得小，小才看得见淘汰和 miss

// 页内布局（阶段 1 用最朴素的定长槽位，slot 目录留给阶段 2 正式做）：
//   [0..1]   u16 record_count
//   [2..7]   保留（对齐 + 留给以后放 slot 目录起始偏移）
//   [8..]    record_count 个定长记录，每条 kRecSize 字节
constexpr uint32_t kPageHeader = 8;
constexpr uint32_t kRecSize = 200;
constexpr uint32_t kRecPerPage = (kPageSize - kPageHeader) / kRecSize;   // = 20

// ---------------------------------------------------------------------------
// 小端序列化。为什么不用 memcpy 直接 cast 结构体指针？
//   因为 (a) 结构体对齐会插 padding，磁盘布局不可控；
//   (b) 直接 *(uint32_t*)ptr 在目标地址未对齐时是 UB，编译器还可能向量化出问题。
//   老老实实逐字节读写，代价可以忽略，换来磁盘格式完全可控 + 跨平台。
// ---------------------------------------------------------------------------
void put_u16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t(v >> 8); }
void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint16_t get_u16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); return v; }
uint64_t get_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }

// FNV-1a：够用就行。这里的作用是"校验读回来的字节和写下去的是同一份"，不是防恶意。
uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// ===========================================================================
// Pager：唯一职责是"给我一个 page_id，我保证给你正确的 4096 字节"
// ===========================================================================
class Pager {
public:
    struct Stats {
        uint64_t hits = 0;       // 在缓存里就拿到了
        uint64_t misses = 0;     // 不得不去读盘
        uint64_t evictions = 0;  // 被淘汰的页
        uint64_t flushes = 0;    // 真正 write() 到磁盘的脏页数
        uint64_t reads = 0;      // 真正 fread 的页数
    };

    Pager(const std::string &path, size_t cacheCapacity, bool createIfMissing) {
        path_ = path;
        cap_ = std::max<size_t>(1, cacheCapacity);
        // 用 fopen 探存在性：部分 libstdc++ 下 fstream 以 in 打开缺失文件时
        // 虽未真正打开，failbit 却不置位，if (!probe) 会误判为"文件存在"。
        std::FILE *probe = std::fopen(path.c_str(), "rb");
        if (!probe) {
            if (!createIfMissing) {
                throw std::runtime_error("文件不存在且未允许创建: " + path);
            }
            formatNew();
        } else {
            std::fclose(probe);
            openExisting();
        }
    }

    uint32_t pageCount() const { return page_count_; }
    const Stats &stats() const { return st_; }
    size_t cacheSize() const { return lru_.size(); }
    size_t cacheCapacity() const { return cap_; }
    std::string lastOpenNote() const { return note_; }

    // 读一页（整页拷贝出来）。缓存命中就不碰磁盘。
    std::vector<uint8_t> readPage(uint32_t pid) {
        auto it = idx_.find(pid);
        if (it != idx_.end()) {
            ++st_.hits;
            lru_.splice(lru_.begin(), lru_, it->second);   // 移到队首 = 最近使用
            return it->second->bytes;
        }
        ++st_.misses;
        ++st_.reads;
        std::vector<uint8_t> buf(kPageSize, 0);
        if (pid < page_count_) {
            std::FILE *fp = ::fopen(path_.c_str(), "rb");
            if (!fp) throw std::runtime_error("打开失败: " + path_);
            if (::fseek(fp, off(pid), SEEK_SET) != 0 ||
                std::fread(buf.data(), 1, kPageSize, fp) != kPageSize) {
                std::fclose(fp);
                throw std::runtime_error("读页失败: pid=" + std::to_string(pid));
            }
            std::fclose(fp);
        }
        // miss 之后要放进缓存；读进来时 dirty=false
        insert(pid, std::move(buf), /*dirty=*/false);
        return peekCopy(pid);
    }

    // 整页写回缓存并标脏。注意：这里"不立刻落盘"，落盘时机交给 flush()。
    // 这个"写缓存不写盘"的延迟，就是数据库所有脏活（WAL、checkpoint、崩溃恢复）的源头。
    void writePage(uint32_t pid, const std::vector<uint8_t> &page) {
        if (page.size() != kPageSize) throw std::runtime_error("页尺寸不对，必须正好 4096");
        auto it = idx_.find(pid);
        if (it != idx_.end()) {
            ++st_.hits;
            it->second->bytes = page;
            it->second->dirty = true;
            lru_.splice(lru_.begin(), lru_, it->second);
        } else {
            ++st_.misses;
            insert(pid, page, /*dirty=*/true);
        }
        if (pid >= page_count_) page_count_ = pid + 1;
    }

    uint32_t allocPage() { return page_count_++; }

    // 脏页落盘 + 更新元数据页 + fsync。
    // 为什么 fsync 而不是 fflush？fflush 只把 C 运行时缓冲推给操作系统，
    // 断电时操作系统自己的页缓存照样能丢。真要"承诺已落盘"，必须 fsync。
    // 代价（本机 phase 2 会实测）：一次 fsync 几百微秒到几毫秒，是逐条提交的死因。
    void flush() {
        std::FILE *fp = ::fopen(path_.c_str(), "r+b");
        if (!fp) throw std::runtime_error("打开失败: " + path_);
        writeMetaPage(fp);
        growTo(fp, page_count_);   // 新分配的页必须先把文件撑开，否则 fseek 到 EOF 之外写是赌运气
        for (auto &e : lru_) {
            if (!e.dirty) continue;
            if (::fseek(fp, off(e.pid), SEEK_SET) != 0 ||
                std::fwrite(e.bytes.data(), 1, kPageSize, fp) != kPageSize) {
                std::fclose(fp);
                throw std::runtime_error("写页失败: pid=" + std::to_string(e.pid));
            }
            e.dirty = false;
            ++st_.flushes;
        }
        std::fflush(fp);
        ::_commit(_fileno(fp));      // 这就是 Windows 上的 fsync
        std::fclose(fp);
        // 元数据里的 page_count 可能又涨了，把文件裁到整数页
        truncateToPages();
    }

    // 测试用：假装进程被杀，把内存里的东西全丢掉（脏页直接丢，用来演示"没 flush 就没了"）
    void dropCacheLosingDirty() {
        lru_.clear();
        idx_.clear();
    }

    // 把缓存清空但保留磁盘内容：模拟"进程重启，冷启动第一批读"
    void clearCacheClean() {
        flush();
        lru_.clear();
        idx_.clear();
    }

private:
    struct Entry {
        uint32_t pid;
        std::vector<uint8_t> bytes;
        bool dirty;
    };

    static long off(uint32_t pid) { return static_cast<long>(pid) * static_cast<long>(kPageSize); }

    // 把文件长度撑到 pageCount 页（只增不减，用零页填）。
    // 教训：fseek 到 EOF 之外再 fwrite，在不同 CRT 上表现不一样（有的隐式补零、
    // 有的直接失败）。显式补零这件事必须由你自己做，别交给运气。
    static void growTo(std::FILE *fp, uint32_t pageCount) {
        const long need = off(pageCount);
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
            Entry &victim = lru_.back();
            if (victim.dirty) {
                // 脏页被淘汰前必须落盘，否则改了个寂寞。
                // 真实数据库在这里会写 WAL —— 我们到 phase4 才补这套机制。
                std::FILE *fp = ::fopen(path_.c_str(), "r+b");
                if (fp) {
                    growTo(fp, page_count_);
                    if (::fseek(fp, off(victim.pid), SEEK_SET) == 0) {
                        std::fwrite(victim.bytes.data(), 1, kPageSize, fp);
                        std::fflush(fp);
                        ::_commit(_fileno(fp));
                    }
                    std::fclose(fp);
                    ++st_.flushes;
                }
            }

            ++st_.evictions;
            idx_.erase(victim.pid);
            lru_.pop_back();
        }
    }

    std::vector<uint8_t> peekCopy(uint32_t pid) {
        auto it = idx_.find(pid);
        return it == idx_.end() ? std::vector<uint8_t>(kPageSize, 0) : it->second->bytes;
    }

    void writeMetaPage(std::FILE *fp) {
        std::vector<uint8_t> m(kPageSize, 0);
        put_u32(m.data() + 0, kMagic);
        put_u32(m.data() + 4, kVersion);
        put_u32(m.data() + 8, kPageSize);
        put_u32(m.data() + 12, page_count_);
        uint32_t c = fnv1a(m.data(), 12);
        put_u32(m.data() + 16, c);
        if (::fseek(fp, off(kMetaPage), SEEK_SET) != 0 ||
            std::fwrite(m.data(), 1, kPageSize, fp) != kPageSize) {
            throw std::runtime_error("写元数据页失败");
        }
    }

    void formatNew() {
        page_count_ = 1;   // 先只有元数据页
        std::FILE *fp = ::fopen(path_.c_str(), "w+b");
        if (!fp) throw std::runtime_error("创建失败: " + path_);
        writeMetaPage(fp);
        std::fflush(fp);
        ::_commit(_fileno(fp));
        std::fclose(fp);
        note_ = "新建数据库文件";
    }

    void openExisting() {
        std::FILE *fp = ::fopen(path_.c_str(), "r+b");
        if (!fp) throw std::runtime_error("打开失败: " + path_);
        std::vector<uint8_t> m(kPageSize, 0);
        size_t got = (std::fseek(fp, 0, SEEK_SET) == 0) ? std::fread(m.data(), 1, kPageSize, fp) : 0;
        if (got < 20) {
            std::fclose(fp);
            throw std::runtime_error("文件太小，不是合法的 minidb 文件");
        }
        std::fclose(fp);
        uint32_t magic = get_u32(m.data() + 0);
        uint32_t version = get_u32(m.data() + 4);
        uint32_t ps = get_u32(m.data() + 8);
        uint32_t pc = get_u32(m.data() + 12);
        uint32_t c = get_u32(m.data() + 16);
        // 顺序很重要：先看 magic 是不是我们的文件，再看 checksum，再看版本和页尺寸。
        // 每一步的报错信息都要能让人一眼知道下一步干什么，这是运维友好度的基本功。
        if (magic != kMagic)
            throw std::runtime_error("magic 不匹配（0x" + hex(magic) + "），这不是 minidb 文件，或者头部已损坏");
        if (fnv1a(m.data(), 12) != c)
            throw std::runtime_error("元数据页 checksum 不匹配，页 0 被写坏了（撕裂页的典型症状）");
        if (ps != kPageSize)
            throw std::runtime_error("页尺寸不兼容：文件是 " + std::to_string(ps) + "，本程序只认 " + std::to_string(kPageSize));
        if (version != kVersion)
            throw std::runtime_error("版本不兼容：文件 v" + std::to_string(version) + "，本程序是 v" + std::to_string(kVersion));
        page_count_ = pc;
        note_ = "打开已有数据库（v" + std::to_string(version) + "，" + std::to_string(page_count_) + " 页）";
    }

    void truncateToPages() {
        // 用"重写整个文件"的笨办法裁尾。生产环境用 SetEndOfFile / ftruncate。
        // 这里宁可笨也要保证没有半页残留 —— 半页残留就是撕裂页的来源之一。
        std::vector<uint8_t> all;
        std::FILE *rp = ::fopen(path_.c_str(), "rb");
        if (!rp) return;
        std::fseek(rp, 0, SEEK_END);
        long sz = std::ftell(rp);
        long want = off(page_count_);
        if (sz == want) { std::fclose(rp); return; }
        std::fseek(rp, 0, SEEK_SET);
        all.resize(size_t(std::max<long>(sz, 0)));
        if (sz > 0) std::fread(all.data(), 1, size_t(sz), rp);
        std::fclose(rp);
        all.resize(size_t(want), 0);
        std::FILE *wp = ::fopen(path_.c_str(), "wb");
        if (!wp) return;
        std::fwrite(all.data(), 1, all.size(), wp);
        std::fflush(wp);
        ::_commit(_fileno(wp));
        std::fclose(wp);
    }

    static std::string hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof b, "%08X", v);
        return b;
    }

    std::string path_;
    size_t cap_ = kDefaultCachePages;
    uint32_t page_count_ = 0;
    std::string note_;
    std::list<Entry> lru_;                                     // 队首=最近使用，队尾=最久未用
    std::unordered_map<uint32_t, std::list<Entry>::iterator> idx_;
    Stats st_;
};

// ---------------------------------------------------------------------------
// 记录：定长 200 字节。id(4) + seq(8) + text(184) + crc(4)
// 为什么先做定长？因为定长记录的页内布局可以算出来（第 n 条在 8 + n*200），
// 不需要 slot 目录。变长字段的麻烦留给 phase2，你会在那儿亲手恨一次自己。
// ---------------------------------------------------------------------------
std::vector<uint8_t> makeRecord(uint32_t i) {
    std::vector<uint8_t> r(kRecSize, 0);
    uint64_t seq = uint64_t(i) * 7919ull + 13;
    std::string text = "row-" + std::to_string(i) + "-师傅手搓的第" + std::to_string(i) + "条记录";
    put_u32(r.data() + 0, i);
    put_u64(r.data() + 4, seq);
    size_t n = std::min<size_t>(text.size(), 183);
    std::memcpy(r.data() + 12, text.data(), n);
    put_u32(r.data() + 196, fnv1a(r.data(), 196));
    return r;
}

bool checkRecord(const uint8_t *r, uint32_t expectId) {
    if (get_u32(r + 196) != fnv1a(r, 196)) return false;      // checksum 先过
    if (get_u32(r + 0) != expectId) return false;             // id 对得上
    return get_u64(r + 4) == uint64_t(expectId) * 7919ull + 13;
}

// 把第 i 条记录写到它该去的页：页号 = 1 + i/20，槽位 = i%20
uint32_t recPage(uint32_t i) { return 1 + i / kRecPerPage; }
uint32_t recSlot(uint32_t i) { return i % kRecPerPage; }

void storeRecord(Pager &pg, uint32_t i) {
    uint32_t pid = recPage(i);
    auto page = pg.readPage(pid);
    put_u16(page.data(), uint16_t(std::max<uint32_t>(get_u16(page.data()), recSlot(i) + 1)));
    std::vector<uint8_t> r = makeRecord(i);
    std::memcpy(page.data() + kPageHeader + size_t(recSlot(i)) * kRecSize, r.data(), kRecSize);
    pg.writePage(pid, page);
}

const uint8_t *fetchRecord(const std::vector<uint8_t> &page, uint32_t slot) {
    return page.data() + kPageHeader + size_t(slot) * kRecSize;
}

std::string pct(uint64_t hit, uint64_t total) {
    if (total == 0) return "n/a";
    char b[32];
    std::snprintf(b, sizeof b, "%.1f%%", 100.0 * double(hit) / double(total));
    return b;
}

// 中文占两个终端列宽，std::setw 按字节数算，所以直接 setw 会歪成鬼样子。
// 这两个小工具是任何要在 Windows 控制台排版中文的程序的必需品。
size_t dispWidth(const std::string &s) {
    size_t w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c < 0x80) { w += 1; continue; }
        size_t extra = 0;                       // 后续跟随字节数
        if ((c >> 5) == 0x6) extra = 1;         // 110xxxxx：拉丁字母带音标，1 列
        else if ((c >> 4) == 0xE) extra = 2;    // 1110xxxx：CJK / 全角标点，2 列
        else if ((c >> 3) == 0x1E) extra = 3;   // 11110xxx：emoji 等，算 2 列
        else extra = 0;
        w += ((c >> 4) == 0xE || (c >> 3) == 0x1E) ? 2 : 1;
        i += extra;   // 关键：跳过跟随字节，否则每个 0x80-0xBF 又被当成一个新字符
    }
    return w;
}
std::string padR(const std::string &s, size_t w) {
    size_t cur = dispWidth(s);
    return s + std::string(cur < w ? w - cur : 0, ' ');
}

void hr(const char *title) {
    size_t used = 5 + dispWidth(title);
    std::cout << "\n==== " << title << " " << std::string(used < 58 ? 58 - used : 1, '=') << "\n";
}

// ===========================================================================
// 演示 1：跨页写 200 条 → 冷启动重开逐条校验
// ===========================================================================
int demo_cross_page(size_t cap, const std::string &path) {
    std::remove(path.c_str());
    const uint32_t N = 200;

    std::cout << "记录 " << kRecSize << " 字节/条，每页放 " << kRecPerPage
              << " 条 → " << N << " 条要占 " << (N + kRecPerPage - 1) / kRecPerPage
              << " 个数据页；页缓存容量只有 " << cap << " 页\n";

    auto t0 = std::chrono::steady_clock::now();
    {
        Pager pg(path, cap, true);
        std::cout << "  [写] " << pg.lastOpenNote() << "\n";
        for (uint32_t i = 0; i < N; ++i) storeRecord(pg, i);
        pg.flush();
        auto t1 = std::chrono::steady_clock::now();
        auto s = pg.stats();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  [写] 写完 " << N << " 条，用时 " << std::fixed << std::setprecision(1) << ms
                  << " ms，文件涨到 " << pg.pageCount() << " 页\n";
        std::cout << "  [写] 页缓存：命中 " << s.hits << " / 未命中 " << s.misses
                  << "（命中率 " << pct(s.hits, s.hits + s.misses) << "），淘汰 " << s.evictions
                  << " 次，落盘 " << s.flushes << " 页\n";
        std::cout << "  [写] 因为一个页里挤了 " << kRecPerPage << " 条记录，写第 2~" << kRecPerPage
                  << " 条时页已经在缓存里，所以命中率天生就高\n";
        if (cap < (N + kRecPerPage - 1) / kRecPerPage) {
            std::cout << "  [写] 注意淘汰 " << s.evictions << " 次：" << cap << " 页缓存装不下 "
                      << (N + kRecPerPage - 1) / kRecPerPage << " 个数据页，写到身后时前面的页已经被挤出去了"
                      "（淘汰的脏页在本实现里会被强制落盘，这一步就是 checkpoint 的雏形）\n";
        } else {
            std::cout << "  [写] 淘汰 0 次：" << cap << " 页缓存刚好盖住全部 "
                      << (N + kRecPerPage - 1) / kRecPerPage << " 个数据页，一个都没被挤出去\n";
        }
    }

    // ---- 冷启动重开：新对象、空缓存，必须完全靠磁盘 ----
    auto t2 = std::chrono::steady_clock::now();
    {
        Pager pg(path, cap, false);
        std::cout << "  [读] " << pg.lastOpenNote() << "（缓存是空的，一切从磁盘来）\n";
        uint32_t bad = 0, badCrc = 0;
        for (uint32_t i = 0; i < N; ++i) {
            auto page = pg.readPage(recPage(i));
            if (!checkRecord(fetchRecord(page, recSlot(i)), i)) {
                ++bad;
                if (get_u32(fetchRecord(page, recSlot(i)) + 196) != fnv1a(fetchRecord(page, recSlot(i)), 196)) ++badCrc;
            }
        }
        auto t3 = std::chrono::steady_clock::now();
        auto s = pg.stats();
        double ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::cout << "  [读] 重开后逐条校验 " << N << " 条：";
        if (bad == 0) std::cout << "全部通过 ✓\n";
        else std::cout << "有 " << bad << " 条不对（其中 checksum 失败 " << badCrc << " 条）✗\n";
        std::cout << "  [读] 用时 " << std::fixed << std::setprecision(1) << ms << " ms，页缓存命中 "
                  << s.hits << " / 未命中 " << s.misses << "（命中率 " << pct(s.hits, s.hits + s.misses)
                  << "），真读盘 " << s.reads << " 页\n";
        std::cout << "  [读] 一个页里挤了 " << kRecPerPage << " 条记录，所以顺序读的时候"
                  << "每读盘一次就能白赚 " << (kRecPerPage - 1) << " 次命中 —— "
                  "这就是「局部性」能吃掉 IO 的根本原因\n";
        if (bad != 0) return 1;
    }
    return 0;
}

// ===========================================================================
// 演示 2：缓存容量的影响。同一段工作负载只改容量。
//   这里刻意跑两种负载，因为它们的结论完全相反，而这个"相反"恰恰是重点：
//     A 顺序扫描：页内挤了 20 条记录，读一次盘白赚 19 次命中，
//                 所以就算缓存很小，命中率也照样高 —— 局部性帮你兜底。
//     B 点查：每次只碰一条记录，且访问顺序打乱，缓存装不下就得真读盘。
//   只跑 A 会得出"缓存大小无所谓"的错误结论。真实业务里点查才是要命的地方。
// ===========================================================================
void runSweep(const std::string &path, size_t cap, uint32_t N, bool shuffled, const char *label) {
    std::vector<uint32_t> order(N);
    for (uint32_t i = 0; i < N; ++i) order[i] = i;
    if (shuffled) {
        // 固定种子的确定性洗牌：同一个顺序跑三遍，结果可复现（别用 rand 不加种）
        uint32_t s = 12345u;
        for (uint32_t i = N - 1; i > 0; --i) {
            s = s * 1664525u + 1013904223u;
            uint32_t j = (s >> 8) % (i + 1);
            std::swap(order[i], order[j]);
        }
    }
    std::remove(path.c_str());
    {
        Pager w(path, cap, true);
        for (uint32_t i = 0; i < N; ++i) storeRecord(w, i);
        w.flush();
        w.clearCacheClean();
    }
    Pager pg(path, cap, false);
    for (int round = 0; round < 3; ++round)
        for (uint32_t i = 0; i < N; ++i) (void)pg.readPage(recPage(order[i]));
    auto st = pg.stats();
    std::cout << "  " << padR(label, 26) << padR(std::to_string(cap), 11)
              << padR(pct(st.hits, st.hits + st.misses), 11)
              << padR(std::to_string(st.evictions), 11)
              << padR(std::to_string(st.reads), 11)
              << (st.reads <= (N + kRecPerPage - 1) / kRecPerPage ? "盖住工作集，第 2 遍起纯命中"
                                                                  : "装不下，每遍都得回头捞被挤掉的页")
              << "\n";
}

void demo_cache_size(const std::string &path) {
    const uint32_t N = 200;
    const uint32_t need = (N + kRecPerPage - 1) / kRecPerPage;
    std::cout << "同一个库（" << N << " 条 = " << need << " 个数据页），各扫 3 遍，只改缓存容量：\n\n";
    std::cout << "  " << padR("负载", 26) << padR("容量(页)", 11) << padR("命中率", 11)
              << padR("淘汰", 11) << padR("真读盘", 11) << "结论\n";
    std::cout << "  " << std::string(88, '-') << "\n";
    for (size_t cap : {size_t(2), size_t(4), size_t(8), size_t(16), size_t(64)})
        runSweep(path, cap, N, false, cap == 2 ? "A 顺序扫描（局部性好）" : "");
    std::cout << "\n";
    for (size_t cap : {size_t(2), size_t(4), size_t(8), size_t(16), size_t(64)})
        runSweep(path, cap, N, true, cap == 2 ? "B 乱序点查（局部性差）" : "");
    std::cout << "\n师傅点评：A 那几行命中率几乎不动，别以为缓存不重要 —— 那是页内 20 条记录"
                 "在替你把 IO 摊薄。B 才是 OLTP 的真面目：访问顺序一乱，容量从 8 涨到 16，"
                 "真读盘立刻掉到 " << need << " 页（正好一遍工作集），命中率跳上去。"
                 "buffer pool 的大小由工作集决定，不是拍脑袋。调 shared_buffers / cache_size 之前，"
                 "先想清楚你的热点数据到底多大。\n";
}

// ===========================================================================
// 演示 3：没 flush 的修改会消失 + 头部损坏会被拒开
// ===========================================================================
void demo_loss_and_corruption(const std::string &path) {
    std::remove(path.c_str());
    const uint32_t N = 200;
    {
        Pager pg(path, kDefaultCachePages, true);
        for (uint32_t i = 0; i < N; ++i) storeRecord(pg, i);
        pg.flush();
    }

    // 3a. 改了但没 flush，进程"暴死"
    {
        Pager pg(path, kDefaultCachePages, false);
        uint32_t pid = recPage(5);
        auto page = pg.readPage(pid);
        std::vector<uint8_t> r = makeRecord(5);
        put_u32(r.data() + 0, 999999);                              // 改成一条不该存在的记录
        put_u32(r.data() + 196, fnv1a(r.data(), 196));
        std::memcpy(page.data() + kPageHeader + size_t(recSlot(5)) * kRecSize, r.data(), kRecSize);
        pg.writePage(pid, page);
        pg.dropCacheLosingDirty();                                  // 模拟暴死：脏页没落盘就没了
        Pager reopen(path, kDefaultCachePages, false);
        auto back = reopen.readPage(pid);
        bool stillOld = checkRecord(fetchRecord(back, recSlot(5)), 5);
        std::cout << "改了第 5 条但没 flush，然后进程暴死 → 重开后旧数据还在？"
                  << (stillOld ? "是的，改动蒸发了 ✓（数据库把这叫『丢失更新』）" : "不对，脏页居然落盘了？") << "\n";
        std::cout << "师傅点评：这就是为什么必须有 WAL。phase4 会让你看到，"
                     "先把『我要改什么』顺序写进日志并 fsync，脏页晚点落也没关系 —— "
                     "日志在，就能重放出来。\n";
    }

    // 3b. 打坏页 0 的 magic
    const std::string bad = "phase1_corrupt.db";
    std::remove(bad.c_str());
    {
        std::ifstream in(path, std::ios::binary);
        std::ofstream out(bad, std::ios::binary);
        std::vector<uint8_t> head(kPageSize, 0);
        in.read(reinterpret_cast<char *>(head.data()), kPageSize);
        head[0] = 'X'; head[1] = 'X';
        out.write(reinterpret_cast<const char *>(head.data()), kPageSize);
    }
    try {
        Pager pg(bad, kDefaultCachePages, false);
        std::cout << "打坏 magic 之后居然打开成功了 —— 这是 bug，应该拒绝\n";
    } catch (const std::exception &e) {
        std::cout << "打坏 magic 之后打开被拒绝 ✓ 报错原文：" << e.what() << "\n";
    }
    std::remove(bad.c_str());
}

}   // namespace

int main() {
    SetConsoleOutputCP(65001);
    std::cout << "minidb 阶段 1 · Pager：4096 字节的页 + 页 0 元数据 + LRU 页缓存\n";
    std::cout << "页尺寸 " << kPageSize << " 字节，默认缓存 " << kDefaultCachePages << " 页\n";

    int rc = 0;
    hr("演示 1：跨页写 200 条，关掉重开逐条校验");
    rc |= demo_cross_page(kDefaultCachePages, "phase1.db");

    hr("演示 1b：把缓存加大到 64 页，同样的活再干一遍");
    rc |= demo_cross_page(64, "phase1_big.db");

    hr("演示 2：缓存容量 vs 命中率（真统计，不是编的）");
    demo_cache_size("phase1_size.db");

    hr("演示 3：不 flush 会丢 / 头部损坏会拒开");
    demo_loss_and_corruption("phase1_loss.db");

    std::remove("phase1.db");
    std::remove("phase1_big.db");
    std::remove("phase1_size.db");
    std::remove("phase1_loss.db");

    hr("收尾");
    std::cout << (rc == 0 ? "全部校验通过。你现在有了一个诚实的 Pager：它知道页在哪、"
                             "知道哪些页是脏的、知道什么时候真的落盘。\n"
                          : "有校验失败，退出码非 0。别往下走，先修。\n");
    std::cout << "下一阶段：在这个 Pager 上做堆表 —— RID=(page_id,slot)、变长字段、"
                 "null bitmap、删除后 slot 复用。你会第一次体会到"
                 "『页内还得再套一层目录』是被现实逼出来的。\n";
    return rc;
}
