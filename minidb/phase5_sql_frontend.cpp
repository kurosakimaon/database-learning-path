// ============================================================================
// phase5_sql_frontend.cpp —— 手写 SQL 前端：分词 → 递归下降解析 → 逻辑计划 → 执行
//
// 师傅的话：
//   前四个阶段你把地基（页）、房子（堆表）、书架（B+ 树）、保险柜（WAL）都
//   亲手砌过了。这一阶段换换口味：不碰磁盘，专攻"用户说的话怎么变成机器动作"。
//   一条 SELECT 进来，要过四道关卡：
//     1. 分词器（Tokenizer）：把字符流剁成 token（单词/数字/字符串/运算符）；
//     2. 解析器（Parser）：递归下降，按文法把 token 串拼成语法树（AST）；
//     3. 逻辑计划（Planner）：决定"先扫什么后滤什么"——谓词能走 B+ 树就走
//        索引，走不了才全表扫。这一步的取舍就叫"选择律下推"；
//     4. 执行器（Executor）：扫描 → 过滤 → 排序 → 截断 → 投影，流水线吐行。
//   本阶段后端刻意用内存页容器（PageStore），接口和阶段 1 的 Pager 同名同型：
//   落盘、恢复那些事前面已经玩透了，这里把镜头对准前端。
//
// 支持的文法（就这么多，麻雀虽小五脏俱全）：
//   CREATE TABLE t(a INT, b TEXT);
//   INSERT INTO t VALUES (1, '张三'), (2, NULL);        -- 支持多行
//   SELECT 列[, 列]* | * FROM t
//     [ WHERE 列 (=|!=|<|>|<=|>=) 值 [AND ...]* ]
//     [ ORDER BY 列 [DESC] ] [ LIMIT n ];
//   EXPLAIN 前缀可以挂在 SELECT 前面，只打印计划不执行。
//
// 编译：g++ -std=c++17 -O2 -Wall -Wextra phase5_sql_frontend.cpp -o phase5_sql_frontend.exe
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 通用工具（与阶段 1~4 同源，小端序列化 + FNV-1a + 中文宽度对齐）
// ---------------------------------------------------------------------------
constexpr uint32_t kPageSize = 4096;

inline int uchar(char c) { return static_cast<unsigned char>(c); }   // isalpha 等的安全入参

void putU16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v & 0xFF); p[1] = uint8_t(v >> 8); }
void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void putU64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint16_t getU16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t getU32(const uint8_t *p) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[i]) << (8 * i); return v; }
uint64_t getU64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i); return v; }
int64_t getI64(const uint8_t *p) { return int64_t(getU64(p)); }

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
    std::cout << "\n==== " << title << " " << std::string(used < 64 ? 64 - used : 1, '=') << "\n";
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

// ---------------------------------------------------------------------------
// 值类型：这个迷你 SQL 只有两种类型加 NULL —— INT(int64) 和 TEXT。
// 比较规则（简化）：NULL 当最小；类型不同一律谓词为假。
// 师傅点评：真 SQL 是三值逻辑（UNKNOWN），这里先立规矩，课后练习再补。
// ---------------------------------------------------------------------------
struct Value {
    enum Kind : uint8_t { kNull = 0, kInt = 1, kText = 2 };
    Kind kind = kNull;
    int64_t i = 0;
    std::string s;

    static Value makeInt(int64_t v) { Value x; x.kind = kInt; x.i = v; return x; }
    static Value makeText(std::string t) { Value x; x.kind = kText; x.s = std::move(t); return x; }
    static Value makeNull() { return Value(); }

    std::string toString() const {
        if (kind == kNull) return "NULL";
        if (kind == kInt) return std::to_string(i);
        return s;
    }
};

// 返回 -1/0/1；NULL 视为最小
int cmpValue(const Value &a, const Value &b) {
    if (a.kind == Value::kNull || b.kind == Value::kNull)
        return (a.kind == b.kind) ? 0 : (a.kind == Value::kNull ? -1 : 1);
    if (a.kind == Value::kInt && b.kind == Value::kInt)
        return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    if (a.kind == Value::kText && b.kind == Value::kText)
        return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
    return -2;   // 类型不匹配：调用方按"谓词为假"处理
}

// ---------------------------------------------------------------------------
// PageStore：内存页容器。接口刻意与阶段 1 的 Pager 同名（readPage/writePage/
// allocPage），换成落盘实现就是一台真引擎。reads 计数供 EXPLAIN 和基准用。
// ---------------------------------------------------------------------------
struct RID { uint32_t page; uint32_t slot; };

class PageStore {
public:
    uint32_t allocPage() {
        pages_.emplace_back(kPageSize, uint8_t(0));
        return uint32_t(pages_.size()) - 1;
    }
    const std::vector<uint8_t> &readPage(uint32_t pid) const { ++reads; return pages_[pid]; }
    std::vector<uint8_t> &writePage(uint32_t pid) { ++writes; return pages_[pid]; }
    size_t pageCount() const { return pages_.size(); }
    mutable uint64_t reads = 0;    // 读页计数：基准前清零，查询后读差值
    uint64_t writes = 0;

private:
    // 用 deque 而不是 vector：B+ 树分裂时会持有已有页的引用再 allocPage，
    // vector 扩容会搬走内层 vector 对象、引用当场悬空；deque 的 push_back
    // 不动既有元素的地址，引用才稳。这是容器选型里值得记住的一课。
    std::deque<std::vector<uint8_t>> pages_;
};

// ---------------------------------------------------------------------------
// 堆表（动态 schema 版）：页头 8B（u16 slotCount @0 | u16 数据下界 @2，初值 4096）
// + slot 目录 + 数据从页尾向前堆。
// 行格式：u16 总长 | u16 列数 | null 位图 | u16 偏移表[ncols+1] | 数据区
//   非 NULL 列的字节：INT → 8B 小端；TEXT → 原文（长度 = off[i+1]-off[i]）
//   NULL 列零字节，靠位图标记。和阶段 2 一脉相承，只是列数不再写死。
// ---------------------------------------------------------------------------
class Heap {
public:
    explicit Heap(PageStore &st) : st_(st) { firstPage(); }

    RID insert(const std::vector<Value> &row) {
        std::vector<uint8_t> enc = encode(row);
        if (lastPageFull(enc.size())) allocDataPage();
        auto &pg = st_.writePage(dataPid_.back());
        const uint16_t slots = getU16(pg.data());
        const uint16_t lo = getU16(pg.data() + 2);   // 数据区下界
        // 新行写在"下界之下"。要是图省事直接写 kPageSize - len，同一页里
        // 后写的行会盖掉先写的行 —— 槽位读出来全是杂交垃圾，这是真实踩过的坑。
        const uint32_t off = uint32_t(lo) - uint32_t(enc.size());
        std::memcpy(pg.data() + off, enc.data(), enc.size());
        putU16(pg.data(), uint16_t(slots + 1));
        putU16(pg.data() + 2, uint16_t(off));        // 下界下移
        putU32(pg.data() + 8 + size_t(slots) * 4, off);
        ++rows_;
        return RID{dataPid_.back(), slots};
    }

    // 读一行；返回 false = 槽位是空的
    bool get(const RID &rid, std::vector<Value> *out) const {
        if (rid.slot >= slotCount(rid.page)) return false;
        const auto &pg = st_.readPage(rid.page);
        const uint32_t item = getU32(pg.data() + 8 + size_t(rid.slot) * 4);
        if (item & 0x80000000u) return false;             // 已删除
        decode(pg.data() + item, out);
        return true;
    }

    // 全表扫描：逐页逐槽回调
    void scan(const std::function<void (const RID &, const std::vector<Value> &)> &cb) const {
        for (uint32_t pid : dataPid_) {
            const uint16_t slots = slotCount(pid);
            for (uint16_t s = 0; s < slots; ++s) {
                RID rid{pid, s};
                std::vector<Value> row;
                if (get(rid, &row)) cb(rid, row);
            }
        }
    }

    size_t rows() const { return rows_; }
    size_t pages() const { return dataPid_.size(); }

private:
    void firstPage() { allocDataPage(); }
    void allocDataPage() {
        const uint32_t pid = st_.allocPage();
        // 新页数据下界 = 页尾。不写这个的话新页下界是 0，free 空间算出负数。
        putU16(st_.writePage(pid).data() + 2, uint16_t(kPageSize));
        dataPid_.push_back(pid);
    }

    uint16_t slotCount(uint32_t pid) const {
        return getU16(st_.readPage(pid).data());
    }
    // 空闲 = 数据下界 - slot 目录末尾。只减目录不记得减数据区已用字节，
    // 大表写到中途目录和数据就会在页中间对撞 —— 堆页空间账必须两头一起算。
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
        for (size_t c = 0; c < n; ++c)
            if (row[c].kind == Value::kNull) p[c / 8] |= uint8_t(1u << (c % 8));
        p += bmp;
        for (size_t c = 0; c <= n; ++c) { putU16(p, off[c]); p += 2; }
        std::memcpy(p, data.data(), data.size());
        return enc;
    }

    static void decode(const uint8_t *p, std::vector<Value> *out) {
        const uint16_t n = getU16(p + 2);
        const size_t bmp = (n + 7) / 8;
        const uint8_t *bits = p + 4;
        const uint8_t *offs = p + 4 + bmp;
        const uint8_t *base = offs + size_t(n + 1) * 2;
        out->assign(n, Value::makeNull());
        for (size_t c = 0; c < n; ++c) {
            if (bits[c / 8] & (1u << (c % 8))) continue;          // NULL
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
// MiniBTree：int64 → RID 的二级索引（值可重复）。页格式与下降规则同阶段 3：
//   叶子页：kind(1B) pad n(2B) right(4B) left(4B) | [key i64][ridPage][ridSlot]...
//   内部页：kind(1B) pad n(2B) child0(4B)          | [key i64][child]...
//   两套下降规则各管各的 ——
//     插入下降"等于往右"：同 key 永远追加到最右合法叶，批量同值不来回分裂；
//     搜索下降"等于往左"：落到最左合法叶，再沿右兄弟链表向右收集，
//                         这样跨叶的同 key 串一条都不会漏（阶段 3 踩过的坑）。
// 分隔符语义：key[j] = 右子树的最小 key（下界）。
// ---------------------------------------------------------------------------
class MiniBTree {
public:
    static constexpr size_t kLEntry = 16, kIEntry = 12;
    static constexpr size_t kLmax = (kPageSize - 12) / kLEntry;   // 255
    static constexpr size_t kImax = (kPageSize - 8) / kIEntry;    // 340

    explicit MiniBTree(PageStore &st) : st_(st) {
        root_ = st.allocPage();
        height_ = 1;
        st_.writePage(root_)[0] = 2;    // 根页必须标成叶子；全 0 页没标记会被当内部页走
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
        uint32_t pid = descendLE(key);                    // 第一个 >= key 的叶
        while (pid != kNil) {
            const auto &pg = st_.readPage(pid);
            const uint16_t n = getU16(pg.data() + 2);
            size_t pos = 0;
            while (pos < n && getI64(pg.data() + 12 + pos * kLEntry) < key) ++pos;
            for (; pos < n; ++pos) {
                const int64_t k = getI64(pg.data() + 12 + pos * kLEntry);
                if (k > key) return out;
                const uint32_t rp = getU32(pg.data() + 12 + pos * kLEntry + 8);
                const uint32_t rs = getU32(pg.data() + 12 + pos * kLEntry + 12);
                out.push_back(RID{rp, rs});
            }
            pid = getU32(pg.data() + 4);                  // right sibling
        }
        return out;
    }

    // 闭区间 [lo, hi]
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
                const uint32_t rp = getU32(pg.data() + 12 + pos * kLEntry + 8);
                const uint32_t rs = getU32(pg.data() + 12 + pos * kLEntry + 12);
                cb(k, RID{rp, rs});
            }
            pid = getU32(pg.data() + 4);
            lo = INT64_MIN;                               // 后续叶从头收
        }
    }

    uint32_t height() const { return height_; }
    uint64_t count() const { return count_; }
    uint32_t root() const { return root_; }
    static constexpr uint32_t kNil = 0;                   // 0 号页是堆表的，树页 >=1

    // 诊断遍历：从最左叶沿右兄弟链表走完全树叶子
    void dumpAll(const std::function<void (int64_t, RID)> &cb) const {
        uint32_t pid = descendLE(INT64_MIN);
        while (pid != kNil) {
            const auto &pg = st_.readPage(pid);
            const uint16_t n = getU16(pg.data() + 2);
            for (uint16_t t = 0; t < n; ++t)
                cb(getI64(pg.data() + 12 + t * kLEntry),
                   RID{getU32(pg.data() + 12 + t * kLEntry + 8),
                       getU32(pg.data() + 12 + t * kLEntry + 12)});
            pid = getU32(pg.data() + 4);
        }
    }

private:
    static void putI64(uint8_t *p, int64_t v) { putU64(p, uint64_t(v)); }

    // 搜索下降：等于往左（第一个 >= key 的分隔符处停）
    uint32_t descendLE(int64_t key) const {
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
        if (kind == 2) {                                   // ---- 叶子 ----
            if (size_t(n) + 1 <= kLmax) {
                size_t t = 0;                              // 插入下降：等于往右
                while (t < n && getI64(pg.data() + 12 + t * kLEntry) <= key) ++t;
                uint8_t *dst = pg.data() + 12 + t * kLEntry;
                std::memmove(dst + kLEntry, dst, (n - t) * kLEntry);
                putI64(dst, key);
                putU32(dst + 8, rid.page);
                putU32(dst + 12, rid.slot);
                putU16(pg.data() + 2, uint16_t(n + 1));
                return false;
            }
            // 满了：kLmax+1 条先进临时数组，一分为二
            struct E { int64_t k; uint32_t p; uint32_t s; };
            std::vector<E> tmp;
            tmp.reserve(kLmax + 1);
            for (uint16_t t = 0; t < n; ++t)
                tmp.push_back({getI64(pg.data() + 12 + t * kLEntry),
                               getU32(pg.data() + 12 + t * kLEntry + 8),
                               getU32(pg.data() + 12 + t * kLEntry + 12)});
            size_t pos = tmp.size();
            while (pos > 0 && tmp[pos - 1].k > key) --pos; // 等于往右：插在相等者之后
            tmp.insert(tmp.begin() + long(pos), E{key, rid.page, rid.slot});
            const size_t mid = (kLmax + 1) / 2;
            const uint32_t np = st_.allocPage();
            auto &right = st_.writePage(np);
            right[0] = 2;
            putU32(right.data() + 4, getU32(pg.data() + 4));   // right = 旧 right
            putU32(right.data() + 8, pid);                      // left = 我
            putU16(right.data() + 2, uint16_t(tmp.size() - mid));
            for (size_t t = mid; t < tmp.size(); ++t) {
                uint8_t *q = right.data() + 12 + (t - mid) * kLEntry;
                putI64(q, tmp[t].k); putU32(q + 8, tmp[t].p); putU32(q + 12, tmp[t].s);
            }
            // 左页也必须照 tmp 重写一遍！只改条数不搬字节，那么当新条目的
            // 插入位 pos < mid 时（重复 key 一多就会撞上），左页留的是原字节的
            // 前 mid 条、右页拿到的却是右移过一位的那批：边界那条被复制两份，
            // 新条目当场蒸发。唯一 key 顺序插入永远 pos = n 掩盖了这个坑。
            for (size_t q = 0; q < mid; ++q) {
                uint8_t *q2 = pg.data() + 12 + q * kLEntry;
                putI64(q2, tmp[q].k); putU32(q2 + 8, tmp[q].p); putU32(q2 + 12, tmp[q].s);
            }
            putU16(pg.data() + 2, uint16_t(mid));               // 我只留前 mid 条
            putU32(pg.data() + 4, np);                          // right = 新兄弟
            if (getU32(right.data() + 4) != kNil)
                putU32(st_.writePage(getU32(right.data() + 4)).data() + 8, np);
            *splitKey = tmp[mid].k;
            *newPid = np;
            return true;
        }
        // ---- 内部页 ----
        size_t t = 0;                                        // 插入下降：等于往右
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
        // 内部页分裂：先把 splitKey/newPid 按位插入（原元素右移），
        // 中间 key 上移不进任何一页。血泪教训：这里图省事直接
        // keys[t]=splitKey 覆盖写入，会把原分隔符和子指针当场碾死，
        // 每次内部页分裂都丢一棵子树 —— 非唯一 key 的索引最先暴露。
        std::vector<int64_t> keys(n + 1);
        std::vector<uint32_t> kids(n + 2);
        kids[0] = getU32(pg.data() + 4);
        for (uint16_t q = 0; q < n; ++q) {
            keys[q] = getI64(pg.data() + 8 + q * kIEntry);
            kids[q + 1] = getU32(pg.data() + 8 + q * kIEntry + 8);
        }
        for (size_t q = n; q > t; --q) keys[q] = keys[q - 1];         // [t..n-1] 右移
        for (size_t q = n + 1; q > t + 1; --q) kids[q] = kids[q - 1]; // [t+1..n] 右移
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
// 分词器：字符流 → token 流。token 四类：Word（标识符/关键字）、Number、
// String（单引号，'' 转义）、Op（( ) , * = < > <= >= != - ;）。大小写不敏感。
// ---------------------------------------------------------------------------
struct Token {
    enum Kind { kWord, kNumber, kString, kOp, kEnd };
    Kind kind = kEnd;
    std::string text;              // Word 原文 / String 内容 / Op 符号
    int64_t num = 0;               // Number 的值
    size_t pos = 0;                // 出错时告诉用户第几个字符
};

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string &m) : std::runtime_error(m) {}
};

std::string upper(std::string s) {
    for (char &c : s) c = char(std::toupper(uchar(c)));
    return s;
}

std::vector<Token> tokenize(const std::string &sql) {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = sql.size();
    while (i < n) {
        char c = sql[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {          // -- 行注释
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }
        Token tk;
        tk.pos = i;
        if (std::isalpha(uchar(c)) || c == '_') {                  // 标识符/关键字
            size_t j = i;
            while (j < n && (std::isalnum(uchar(sql[j])) || sql[j] == '_')) ++j;
            tk.kind = Token::kWord;
            tk.text = sql.substr(i, j - i);
            i = j;
        } else if (std::isdigit(uchar(c))) {                       // 整数
            size_t j = i;
            uint64_t v = 0;
            while (j < n && std::isdigit(uchar(sql[j]))) { v = v * 10 + uint64_t(sql[j] - '0'); ++j; }
            tk.kind = Token::kNumber;
            tk.num = int64_t(v);
            tk.text = sql.substr(i, j - i);
            i = j;
        } else if (c == '\'') {                                    // 字符串，'' → '
            size_t j = i + 1;
            std::string s;
            for (;;) {
                if (j >= n) throw ParseError("字符串没闭合（位置 " + std::to_string(i) + "）");
                if (sql[j] == '\'') {
                    if (j + 1 < n && sql[j + 1] == '\'') { s += '\''; j += 2; continue; }
                    ++j;
                    break;
                }
                s += sql[j++];
            }
            tk.kind = Token::kString;
            tk.text = s;
            i = j;
        } else {                                                   // 运算符
            std::string op(1, c);
            if (i + 1 < n) {
                std::string two = sql.substr(i, 2);
                if (two == "<=" || two == ">=" || two == "!=") op = two;
            }
            if (op != "(" && op != ")" && op != "," && op != "*" &&
                op != "=" && op != "<" && op != ">" && op != "<=" &&
                op != ">=" && op != "!=" && op != "-" && op != ";")
                throw ParseError(std::string("不认识的字符 '") + c + "'（位置 " + std::to_string(i) + "）");
            tk.kind = Token::kOp;
            tk.text = op;
            i += op.size();
        }
        out.push_back(tk);
    }
    Token end;
    end.kind = Token::kEnd;
    end.pos = n;
    out.push_back(end);
    return out;
}

// ---------------------------------------------------------------------------
// AST：一条语句解析完的样子
// ---------------------------------------------------------------------------
struct ColDef { std::string name; bool isInt = true; };          // TEXT 用 false

struct Pred {                                                    // WHERE 的一个合取项
    std::string col;
    std::string op;                                              // = != < > <= >=
    Value val;
};

struct SelectStmt {
    bool explain = false;
    bool star = false;
    std::vector<std::string> cols;
    std::string table;
    std::vector<Pred> preds;                                     // AND 链
    bool hasOrder = false;
    std::string orderCol;
    bool desc = false;
    bool hasLimit = false;
    int64_t limit = 0;
};

struct InsertStmt {
    std::string table;
    std::vector<std::vector<Value>> rows;
};

struct CreateTableStmt {
    std::string name;
    std::vector<ColDef> cols;
};

enum class StmtKind { kCreate, kInsert, kSelect };
struct Stmt {
    StmtKind kind = StmtKind::kSelect;
    CreateTableStmt create;
    InsertStmt insert;
    SelectStmt select;
};

// ---------------------------------------------------------------------------
// 递归下降解析器。文法见文件头。每个 parseXxx 对应一条产生式，
// 遇到对不上的 token 就抛 ParseError —— 这就是手写解析器的好处：
// 报错信息是你自己写的，想多贴心就多贴心。
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Stmt parseStmt() {
        Stmt st;
        if (matchKw("CREATE")) { parseCreate(&st.create); st.kind = StmtKind::kCreate; }
        else if (matchKw("INSERT")) { parseInsert(&st.insert); st.kind = StmtKind::kInsert; }
        else if (peekKw("EXPLAIN")) {
            advance();
            if (!matchKw("SELECT")) err("EXPLAIN 目前只支持 SELECT");
            parseSelect(&st.select);
            st.select.explain = true;
            st.kind = StmtKind::kSelect;
        }
        else if (matchKw("SELECT")) { parseSelect(&st.select); st.kind = StmtKind::kSelect; }
        else err("不认识的操作，语句应以 CREATE / INSERT / SELECT / EXPLAIN 开头");
        matchOp(";");                                            // 分号可省
        expect(Token::kEnd, "语句尾有多余内容");
        return st;
    }

private:
    const Token &cur() const { return toks_[p_]; }
    const Token &advance() { return toks_[p_++]; }
    bool peekKw(const char *kw) const {
        return cur().kind == Token::kWord && upper(cur().text) == kw;
    }
    bool matchKw(const char *kw) {
        if (peekKw(kw)) { advance(); return true; }
        return false;
    }
    bool matchOp(const char *op) {
        if (cur().kind == Token::kOp && cur().text == op) { advance(); return true; }
        return false;
    }
    void expectOp(const char *op, const char *ctx) {
        if (!matchOp(op))
            err(std::string(ctx) + "：期望 '" + op + "'，实际是 '" + cur().text + "'");
    }
    void expect(Token::Kind k, const char *msg) {
        if (cur().kind != k) err(msg);
    }
    [[noreturn]] void err(const std::string &m) {
        throw ParseError("解析错误 @ " + std::to_string(cur().pos) + "：" + m);
    }
    std::string ident(const char *ctx) {
        if (cur().kind != Token::kWord)
            err(std::string(ctx) + "：期望标识符");
        return advance().text;
    }

    void parseCreate(CreateTableStmt *c) {
        if (!matchKw("TABLE")) err("CREATE 后面跟的得是 TABLE");
        c->name = ident("表名");
        expectOp("(", "CREATE TABLE 列定义");
        for (;;) {
            ColDef cd;
            cd.name = ident("列名");
            if (matchKw("INT") || matchKw("INTEGER")) cd.isInt = true;
            else if (matchKw("TEXT")) cd.isInt = false;
            else if (matchKw("VARCHAR")) { expectOp("(", "VARCHAR"); advance(); expectOp(")", "VARCHAR"); cd.isInt = false; }
            else err("列类型只支持 INT / TEXT / VARCHAR(n)");
            c->cols.push_back(cd);
            if (matchOp(",")) continue;
            break;
        }
        expectOp(")", "CREATE TABLE 列定义");
        if (c->cols.empty()) err("表至少要有一列");
    }

    void parseInsert(InsertStmt *in) {
        if (!matchKw("INTO")) err("INSERT 后面跟的得是 INTO");
        in->table = ident("表名");
        if (!matchKw("VALUES")) err("INSERT INTO t 后面得是 VALUES");
        for (;;) {
            expectOp("(", "VALUES 行");
            std::vector<Value> row;
            for (;;) {
                row.push_back(parseLiteral());
                if (matchOp(",")) continue;
                break;
            }
            expectOp(")", "VALUES 行");
            in->rows.push_back(std::move(row));
            if (matchOp(",")) continue;
            break;
        }
        if (in->rows.empty()) err("VALUES 里一行都没有");
    }

    Value parseLiteral() {
        if (matchKw("NULL")) return Value::makeNull();
        if (cur().kind == Token::kNumber) return Value::makeInt(advance().num);
        if (matchOp("-")) {
            if (cur().kind != Token::kNumber) err("负号后面得是数字");
            return Value::makeInt(-advance().num);
        }
        if (cur().kind == Token::kString) return Value::makeText(advance().text);
        err("值只支持 整数 / '字符串' / NULL");
    }

    void parseSelect(SelectStmt *s) {
        if (matchOp("*")) { s->star = true; }
        else {
            for (;;) {
                s->cols.push_back(ident("投影列"));
                if (matchOp(",")) continue;
                break;
            }
        }
        if (!matchKw("FROM")) err("SELECT 列表后面得是 FROM");
        s->table = ident("表名");
        if (matchKw("WHERE")) {
            for (;;) {
                Pred pd;
                pd.col = ident("谓词左边的列名");
                if (cur().kind != Token::kOp ||
                    (cur().text != "=" && cur().text != "!=" && cur().text != "<" &&
                     cur().text != ">" && cur().text != "<=" && cur().text != ">="))
                    err("谓词运算符只支持 = != < > <= >=");
                pd.op = advance().text;
                pd.val = parseLiteral();
                s->preds.push_back(std::move(pd));
                if (matchKw("AND")) continue;
                break;
            }
        }
        if (matchKw("ORDER")) {
            if (!matchKw("BY")) err("ORDER 后面得是 BY");
            s->hasOrder = true;
            s->orderCol = ident("ORDER BY 列名");
            s->desc = matchKw("DESC");
            matchKw("ASC");
        }
        if (matchKw("LIMIT")) {
            if (cur().kind != Token::kNumber) err("LIMIT 后面得是非负整数");
            s->hasLimit = true;
            s->limit = advance().num;
        }
    }

    std::vector<Token> toks_;   // token 流
    size_t p_ = 0;              // 当前读到的位置
};

// ---------------------------------------------------------------------------
// 目录 + 执行器
// ---------------------------------------------------------------------------
struct Table {
    std::string name;
    std::vector<ColDef> cols;
    std::unique_ptr<Heap> heap;
    // 二级索引：每个 INT 列一棵 B+ 树（自动建，注释见 execCreate）。
    // key = 列值，value = RID。TEXT 列没索引 → WHERE 只能全表扫，正好做对照。
    std::map<size_t, std::unique_ptr<MiniBTree>> idx;

    int colIdx(const std::string &col) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i].name == col) return int(i);
        return -1;
    }
};

struct QueryResult {
    std::vector<std::string> header;
    std::vector<std::vector<Value>> rows;
    std::vector<std::string> plan;      // EXPLAIN 文本（非 EXPLAIN 时也有，供内部检查）
};

class Engine {
public:
    // 返回 true 表示表存在
    bool exec(const Stmt &st, QueryResult *res, std::string *errOut) {
        try {
            switch (st.kind) {
                case StmtKind::kCreate: execCreate(st.create); return true;
                case StmtKind::kInsert: execInsert(st.insert); return true;
                case StmtKind::kSelect: execSelect(st.select, res); return true;
            }
        } catch (const ParseError &e) { *errOut = e.what(); return false; }
        return false;
    }

    PageStore store;

private:
    Table *need(const std::string &name) {
        auto it = tables_.find(name);
        if (it == tables_.end())
            throw ParseError("表不存在：" + name);
        return it->second.get();
    }

    void execCreate(const CreateTableStmt &c) {
        if (tables_.count(c.name))
            throw ParseError("表已存在：" + c.name);
        auto t = std::make_unique<Table>();
        t->name = c.name;
        t->cols = c.cols;
        t->heap = std::make_unique<Heap>(store);
        for (size_t i = 0; i < t->cols.size(); ++i)
            if (t->cols[i].isInt)
                t->idx[i] = std::make_unique<MiniBTree>(store);
        // 师傅点评：真数据库要 CREATE INDEX 显式建，因为索引有写放大代价。
        // 这里图省事给每个 INT 列自动建，"选择性 vs 写放大"的账留在 tutorial。
        tables_[c.name] = std::move(t);
    }

    void execInsert(const InsertStmt &in) {
        Table *t = need(in.table);
        for (const auto &rowIn : in.rows) {
            if (rowIn.size() != t->cols.size())
                throw ParseError("列数对不上：表 " + t->name + " 有 " +
                                 std::to_string(t->cols.size()) + " 列，VALUES 给了 " +
                                 std::to_string(rowIn.size()) + " 个值");
            std::vector<Value> row = rowIn;
            for (size_t i = 0; i < row.size(); ++i) {          // 类型粗校验并归一
                if (row[i].kind == Value::kNull) continue;
                if (t->cols[i].isInt && row[i].kind != Value::kInt)
                    throw ParseError("列 " + t->cols[i].name + " 是 INT，塞不进字符串 '" + row[i].s + "'");
                if (!t->cols[i].isInt && row[i].kind != Value::kText)
                    throw ParseError("列 " + t->cols[i].name + " 是 TEXT，塞不进整数");
            }
            const RID rid = t->heap->insert(row);
            for (auto &kv : t->idx)                            // 索引同步更新
                if (row[kv.first].kind == Value::kInt)
                    kv.second->insert(row[kv.first].i, rid);   // NULL 不进索引
        }
    }

    void execSelect(const SelectStmt &s, QueryResult *res) {
        Table *t = need(s.table);
        for (const auto &pd : s.preds)
            if (t->colIdx(pd.col) < 0)
                throw ParseError("WHERE 里的列不存在：" + pd.col);
        if (s.hasOrder && t->colIdx(s.orderCol) < 0)
            throw ParseError("ORDER BY 里的列不存在：" + s.orderCol);

        // ---- 选择律：找第一个能下推到索引的谓词 ----
        int idxPred = -1;
        for (size_t i = 0; i < s.preds.size(); ++i) {
            const int ci = t->colIdx(s.preds[i].col);
            if (ci >= 0 && t->idx.count(size_t(ci)) && s.preds[i].val.kind == Value::kInt &&
                s.preds[i].op != "!=") { idxPred = int(i); break; }
        }

        // ---- 计划文本（EXPLAIN 看的就是这个）----
        std::string scanDesc;
        if (idxPred >= 0) {
            const Pred &pd = s.preds[size_t(idxPred)];
            scanDesc = "IndexScan[" + t->name + "." + pd.col + " : " + pd.col + " " +
                       pd.op + " " + pd.val.toString() + "]，树高 " +
                       std::to_string(t->idx[size_t(t->colIdx(pd.col))]->height()) +
                       " 层 ≈ 读 " + std::to_string(t->idx[size_t(t->colIdx(pd.col))]->height()) + " 页";
        } else {
            scanDesc = "TableScan[" + t->name + "]，" + std::to_string(t->heap->rows()) +
                       " 行 / " + std::to_string(t->heap->pages()) + " 页全读";
        }
        res->plan.push_back("Project[" + (s.star ? "*" : join(s.cols, ", ")) + "]");
        if (s.hasLimit) res->plan.push_back("  └─ Limit[" + std::to_string(s.limit) + "]");
        if (s.hasOrder) res->plan.push_back("    └─ Sort[" + s.orderCol + (s.desc ? " DESC" : " ASC") + "]");
        std::string filterDesc = "Filter[";
        for (size_t i = 0; i < s.preds.size(); ++i) {
            if (i) filterDesc += " AND ";
            filterDesc += s.preds[i].col + " " + s.preds[i].op + " " + s.preds[i].val.toString();
        }
        filterDesc += s.preds.empty() ? "]" : "]";
        if (!s.preds.empty()) res->plan.push_back("      └─ " + filterDesc);
        res->plan.push_back(std::string(s.preds.empty() ? "    " : "        ") + "└─ " + scanDesc);

        if (s.explain) return;                                 // EXPLAIN 只出计划

        // ---- 执行：扫描 ----
        std::vector<std::vector<Value>> rows;
        const uint64_t reads0 = store.reads;
        if (idxPred >= 0) {
            const Pred &pd = s.preds[size_t(idxPred)];
            MiniBTree *bt = t->idx[size_t(t->colIdx(pd.col))].get();
            auto fetch = [&](RID rid) {
                std::vector<Value> row;
                if (t->heap->get(rid, &row)) rows.push_back(std::move(row));
            };
            if (pd.op == "=") {
                for (RID rid : bt->lookup(pd.val.i)) fetch(rid);
            } else {                                           // 范围：折算成闭区间
                int64_t lo = INT64_MIN, hi = INT64_MAX;
                if (pd.op == "<") hi = pd.val.i - 1;
                else if (pd.op == "<=") hi = pd.val.i;
                else if (pd.op == ">") lo = pd.val.i + 1;
                else lo = pd.val.i;                            // >=
                bt->range(lo, hi, [&](int64_t, RID rid) { fetch(rid); });
            }
        } else {
            t->heap->scan([&](const RID &, const std::vector<Value> &row) {
                rows.push_back(row);
            });
        }

        // ---- 过滤：全部谓词再验一遍（索引那条也重验，图个稳当）----
        std::vector<std::vector<Value>> kept;
        for (const auto &row : rows) {
            bool ok = true;
            for (const auto &pd : s.preds) {
                const int ci = t->colIdx(pd.col);
                const Value &v = row[size_t(ci)];
                if (v.kind == Value::kNull || pd.val.kind != v.kind) { ok = false; break; }
                const int c = cmpValue(v, pd.val);
                if (c == -2) { ok = false; break; }
                bool pass = false;
                if (pd.op == "=") pass = (c == 0);
                else if (pd.op == "!=") pass = (c != 0);
                else if (pd.op == "<") pass = (c < 0);
                else if (pd.op == "<=") pass = (c <= 0);
                else if (pd.op == ">") pass = (c > 0);
                else pass = (c > 0);                           // >=
                if (!pass) { ok = false; break; }
            }
            if (ok) kept.push_back(std::move(row));
        }
        rows = std::move(kept);

        // ---- 排序 ----
        if (s.hasOrder) {
            const int ci = t->colIdx(s.orderCol);
            std::stable_sort(rows.begin(), rows.end(), [&](const std::vector<Value> &a,
                                                            const std::vector<Value> &b) {
                int c = cmpValue(a[size_t(ci)], b[size_t(ci)]);
                return s.desc ? (c > 0) : (c < 0);
            });
        }
        // ---- 截断 ----
        if (s.hasLimit && size_t(s.limit) < rows.size()) rows.resize(size_t(s.limit));

        // ---- 投影 ----
        if (s.star) {
            res->header.clear();
            for (const auto &c : t->cols) res->header.push_back(c.name);
            res->rows = std::move(rows);
        } else {
            res->header.clear();
            std::vector<size_t> pick;
            for (const auto &name : s.cols) {
                const int ci = t->colIdx(name);
                if (ci < 0) throw ParseError("投影的列不存在：" + name);
                res->header.push_back(name);
                pick.push_back(size_t(ci));
            }
            std::vector<std::vector<Value>> out;
            out.reserve(rows.size());
            for (const auto &row : rows) {
                std::vector<Value> one;
                one.reserve(pick.size());
                for (size_t c : pick) one.push_back(row[c]);
                out.push_back(std::move(one));
            }
            res->rows = std::move(out);
        }
        lastScanPages_ = store.reads - reads0;                 // 供基准打印
    }

    static std::string join(const std::vector<std::string> &v, const char *sep) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += sep; s += v[i]; }
        return s;
    }

public:
    uint64_t lastScanPages() const { return lastScanPages_; }
    size_t tableCount() const { return tables_.size(); }

private:
    std::map<std::string, std::unique_ptr<Table>> tables_;
    uint64_t lastScanPages_ = 0;
};

// ---------------------------------------------------------------------------
// 表格化输出：按显示宽度对齐（中文占 2 列）
// ---------------------------------------------------------------------------
void printTable(const QueryResult &qr) {
    const size_t nc = qr.header.size();
    std::vector<size_t> w(nc);
    for (size_t c = 0; c < nc; ++c) w[c] = dispWidth(qr.header[c]);
    std::vector<std::vector<std::string>> txt;
    txt.reserve(qr.rows.size());
    for (const auto &row : qr.rows) {
        std::vector<std::string> one(nc);
        for (size_t c = 0; c < nc; ++c) {
            one[c] = row[c].toString();
            w[c] = std::max(w[c], dispWidth(one[c]));
        }
        txt.push_back(std::move(one));
    }
    std::string line = "+";
    for (size_t c = 0; c < nc; ++c) line += std::string(w[c] + 2, '-') + "+";
    std::cout << line << "\n";
    std::cout << "|";
    for (size_t c = 0; c < nc; ++c) std::cout << " " << padR(qr.header[c], w[c]) << " |";
    std::cout << "\n" << line << "\n";
    for (const auto &row : txt) {
        std::cout << "|";
        for (size_t c = 0; c < nc; ++c) std::cout << " " << padR(row[c], w[c]) << " |";
        std::cout << "\n";
    }
    std::cout << line << "\n";
    std::cout << "共 " << txt.size() << " 行\n";
}

// ---------------------------------------------------------------------------
// 主演示：一段脚本化的 SQL 会话
// ---------------------------------------------------------------------------
Engine g;

// 跑一条 SQL；expectRows >= 0 时校验行数，返回校验位（0=过）
int run(const std::string &sql, int64_t expectRows = -1) {
    std::cout << "\nmysql> " << sql << "\n";
    try {
        Parser p(tokenize(sql));
        Stmt st = p.parseStmt();
        QueryResult qr;
        std::string err;
        if (!g.exec(st, &qr, &err)) {
            std::cout << "  ✗ " << err << "\n";
            return (expectRows == -2) ? 0 : 1;   // -2 = 这条本来就该报错
        }
        if (st.kind == StmtKind::kSelect) {
            if (qr.plan.empty()) { std::cout << "  ✗ 计划是空的\n"; return 1; }
            if (st.select.explain) {
                std::cout << "  计划：\n";
                for (const auto &l : qr.plan) std::cout << "    " << l << "\n";
                return 0;
            }
            printTable(qr);
            if (expectRows >= 0 && int64_t(qr.rows.size()) != expectRows) {
                std::cout << "  ✗ 期望 " << expectRows << " 行，实际 " << qr.rows.size() << "\n";
                return 1;
            }
        } else {
            std::cout << "  ok\n";
        }
        return 0;
    } catch (const ParseError &e) {
        std::cout << "  ✗ " << e.what() << "\n";
        return (expectRows == -2) ? 0 : 1;      // expectRows=-2 表示"这条就该报错"
    }
}

int runMain() {
    int rc = 0;

    hr("① 解析器自检：好语句收下来，坏语句踢回去");
    {
        const char *good[] = {
            "CREATE TABLE t(a INT, b TEXT)",
            "INSERT INTO t VALUES (1, 'hi'), (2, NULL)",
            "SELECT * FROM t WHERE a >= 1 AND b != 'x' ORDER BY a DESC LIMIT 3",
            "EXPLAIN SELECT a FROM t WHERE a = 5",
        };
        for (const char *s : good) {
            try { Parser p(tokenize(s)); p.parseStmt(); std::cout << "  ✓ 解析通过：" << s << "\n"; }
            catch (const ParseError &e) { std::cout << "  ✗ " << s << " → " << e.what() << "\n"; rc |= 1; }
        }
        const char *bad[] = {
            "SELECT * FROM",                 // 缺表名
            "SELECT * FROM t WHERE a = ",    // 缺值
            "INSERT INTO t VALUES (1",       // 括号没闭
            "DROP TABLE t",                  // 不认识的开头
        };
        for (const char *s : bad) {
            bool caught = false;
            try { Parser p(tokenize(s)); p.parseStmt(); }
            catch (const ParseError &e) { caught = true; std::cout << "  ✓ 正确踢回：" << s << " → " << e.what() << "\n"; }
            if (!caught) { std::cout << "  ✗ 坏语句居然过了：" << s << "\n"; rc |= 1; }
        }
    }

    hr("② 建表 + 插数据（含 NULL、重复值、中文名）");
    rc |= run("CREATE TABLE users(id INT, name TEXT, age INT)");
    rc |= run("INSERT INTO users VALUES (1, '张三', 28), (2, '李四', 35), (3, '王五', 28), "
              "(4, '赵六', NULL), (5, '孙七', 42), (6, '周八', 28), (7, '吴九', 35), (8, '郑十', 51)");

    hr("③ 查询全家桶");
    rc |= run("SELECT * FROM users", 8);
    rc |= run("SELECT name, age FROM users WHERE age >= 30 AND age <= 40", 2);   // 35,35（42 超界）
    rc |= run("SELECT * FROM users WHERE age = 28", 3);                          // 非唯一等值
    rc |= run("SELECT * FROM users WHERE age != 28", 4);                         // 4 个非 NULL 且不等
    rc |= run("SELECT name FROM users WHERE age > 30 ORDER BY age DESC LIMIT 3", 3);
    rc |= run("SELECT * FROM users WHERE name = '张三'", 1);                     // TEXT 谓词 → 全表扫

    hr("④ EXPLAIN：选择律看得见");
    rc |= run("EXPLAIN SELECT * FROM users WHERE age = 28");     // 走 IndexScan
    rc |= run("EXPLAIN SELECT * FROM users WHERE name = '张三'"); // 走 TableScan
    rc |= run("EXPLAIN SELECT * FROM users WHERE age > 30 AND name != '赵六' ORDER BY id LIMIT 2");

    hr("⑤ 规模实测：索引 vs 全表，数字不会说谎");
    rc |= run("CREATE TABLE big(id INT, name TEXT, score INT)");
    {
        // 20000 行，分批 INSERT 语句拼接交给引擎（分词/解析也一起受力）
        const size_t N = 20000;
        std::string sql = "INSERT INTO big VALUES ";
        size_t batch = 0;
        double t0 = nowMs();
        for (size_t i = 1; i <= N; ++i) {
            if (batch) sql += ", ";
            sql += "(" + std::to_string(i) + ", 'u" + std::to_string(i) + "', " +
                   std::to_string((i * 7) % 50) + ")";
            ++batch;
            if (batch == 500 || i == N) {
                Parser p(tokenize(sql));
                Stmt st = p.parseStmt();
                QueryResult qr;
                std::string err;
                if (!g.exec(st, &qr, &err)) { std::cout << "  ✗ 灌数失败：" << err << "\n"; return 1; }
                sql = "INSERT INTO big VALUES ";
                batch = 0;
            }
        }
        std::cout << "  灌入 " << N << " 行（走完整条 SQL 链路），耗时 " << ms(nowMs() - t0) << "\n";
    }
    {
        // 索引路径：score = 33（有 INT 索引）
        auto doQuery = [&](const std::string &sql, QueryResult *qr) -> double {
            Parser p(tokenize(sql));
            Stmt st = p.parseStmt();
            std::string err;
            const uint64_t r0 = g.store.reads;
            const double t0 = nowMs();
            if (!g.exec(st, qr, &err)) throw ParseError(err);
            const double dt = nowMs() - t0;
            qr->plan.clear();
            qr->plan.push_back("pages=" + std::to_string(g.store.reads - r0));
            return dt;
        };
        QueryResult qa, qb;
        double idxMs = doQuery("SELECT * FROM big WHERE score = 33", &qa);
        double heapMs = doQuery("SELECT * FROM big WHERE name = 'u12345'", &qb);
        // 手工对账：score=(i*7)%50==33 的 i 的个数
        size_t want = 0;
        for (size_t i = 1; i <= 20000; ++i) if ((i * 7) % 50 == 33) ++want;
        std::cout << "  索引查询 score=33 ：" << qa.rows.size() << " 行，" << ms(idxMs)
                  << "，读页 " << qa.plan[0] << "\n";
        std::cout << "  全表查 name=u12345：" << qb.rows.size() << " 行，" << ms(heapMs)
                  << "，读页 " << qb.plan[0] << "\n";
        std::cout << "  同一台引擎同一张表，换个谓词就是 "
                  << (heapMs / (idxMs > 0.0001 ? idxMs : 0.0001)) << " 倍差距\n";
        bool ok = (qa.rows.size() == want) && qb.rows.size() == 1 &&
                  (qa.rows.size() > 0 && qb.rows.size() > 0);
        std::cout << "  对账：索引行数与手算一致 " << (qa.rows.size() == want ? "✓" : "✗")
                  << "，全表命中 1 行 " << (qb.rows.size() == 1 ? "✓" : "✗") << "\n";
        rc |= ok ? 0 : 1;
        // 排序单调性校验：ORDER BY 结果必须真有序
        QueryResult qc;
        doQuery("SELECT score FROM big ORDER BY score LIMIT 50", &qc);
        bool sorted = true;
        for (size_t i = 1; i < qc.rows.size(); ++i)
            if (cmpValue(qc.rows[i - 1][0], qc.rows[i][0]) > 0) sorted = false;
        std::cout << "  ORDER BY score LIMIT 50 单调性检查：" << (sorted ? "✓" : "✗") << "\n";
        rc |= sorted ? 0 : 1;
    }

    hr("⑥ 错误处理：表不存在 / 列不存在 / 类型不搭");
    rc |= run("SELECT * FROM ghost", -2);
    rc |= run("SELECT nope FROM users", -2);
    rc |= run("INSERT INTO users VALUES (9, '钱多多', '三十八')", -2);

    hr("师傅收尾");
    std::cout << "  字符流 → token → AST → 逻辑计划 → 流水线执行，一条 SQL 的四道关卡全走通了。\n"
              << "  EXPLAIN 让你亲眼看到：INT 列的谓词走了 B+ 树，TEXT 列只能老老实实全表扫。\n"
              << "  " << (rc == 0 ? "全部通过 ✓" : "有问题没通过 ✗") << "\n";
    return rc;
}

}  // namespace

int main() {
    ::SetConsoleOutputCP(65001);   // 控制台切 UTF-8，中文表格才能对齐
    return runMain();
}
