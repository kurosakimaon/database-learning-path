// ============================================================================
// minidb · 阶段 4：预写日志（WAL）与崩溃恢复 —— 让数据库在断电后还能站起来
// ----------------------------------------------------------------------------
// 编译：  g++ -std=c++17 -O2 -Wall -Wextra phase4_wal.cpp -o phase4_wal.exe
// 运行：  .\phase4_wal.exe        （会自动拉起子进程模拟崩溃，看日志别慌）
// 退出码：0 = 三种崩溃场景全部恢复成功
//
// 师傅开场白：
//   前面三个阶段你的数据都"裸奔"：writePage 只进内存，flush 才落盘。进程半夜被
//   kill，或者笔记本被合上，内存里的脏页就没了 —— 更糟的是"一半新一半旧"的页：
//   页写到一半断电，前半截是新的、后半截是老的，这种杂交页比全丢还难查。
//
//   WAL 的纪律就三条，数据库四十年没变过：
//     ① 改页之前，先把 (LSN, page_id, 前像, 后像) 追加进 .wal 文件并 fsync；
//     ② commit 只 fsync WAL —— 数据页可以慢慢来，日志必须先落地；
//     ③ 恢复时：带校验和地扫 WAL，撕裂尾直接截掉；只 REDO 已提交事务的后像，
//        未提交的直接丢弃（本阶段是 no-steal：脏页从不提前落盘，所以无需 UNDO）。
//
//   checkpoint（检查点）= 把脏页全部落盘 + 把 checkpoint LSN 写进元数据 + 截断日志。
//   恢复从 checkpoint 边界以上开始 REDO，边界以下的记录对应的页已经在盘上，重放
//   它们既浪费又没必要（REDO 虽幂等，但能不做就不做）。
//
//   "崩溃"怎么模拟才算真？本程序用 std::system 拉起自己的子进程，子进程干到一半
//   直接 exit(9) —— 不跑析构、不 flush 数据文件，和真断电对进程来说没区别。
//   三种场景：
//     A 写页中途崩：checkpoint 写到一半，页被撕裂成"半新半旧"；
//     B 提交前崩：改动已进 WAL 并 fsync，但没有 COMMIT 记录 —— 恢复必须丢弃；
//     C 提交后崩：COMMIT 已 fsync，脏页还在内存 —— 恢复必须 REDO 找回。
// ============================================================================

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <io.h>

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kMagic = 0x4D494E49u;      // MINI：数据文件
constexpr uint32_t kWalMagic = 0x57414C31u;   // WAL1：日志记录
constexpr uint32_t kVersion = 1;

constexpr uint8_t kRecBegin = 1, kRecUpdate = 2, kRecCommit = 3;
constexpr uint32_t kWalHdr = 28;   // magic4+len4+lsn8+txn4+type1+pad3+pid4
constexpr size_t kRecCrcTail = 4;  // 记录尾部的校验和

// ---- 小端序列化与 FNV-1a（与阶段 1~3 同源）--------------------------------
void putU32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void putU64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
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
void hr(const std::string &title) {
    size_t used = 5 + dispWidth(title);
    std::cout << "\n==== " << title << " " << std::string(used < 64 ? 64 - used : 1, '=') << "\n";
}
std::string ms(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f ms", v);
    return b;
}
double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

const std::string kDb = "p4.db";
const std::string kWal = "p4.wal";

// ---------------------------------------------------------------------------
// WalLog：.wal 文件的追加与解析
//
// 记录格式（整条记录一次 fwrite + _commit，要么整条在，要么压根没有/是撕裂尾）：
//   [0..3]   u32 magic=WAL1        [4..7]   u32 total_len（含头含校验）
//   [8..15]  u64 lsn               [16..19] u32 txn_id
//   [20]     u8  type（1=BEGIN 2=UPDATE 3=COMMIT）
//   [21..23] pad                   [24..27] u32 page_id（UPDATE 才有意义）
//   [28..]   UPDATE：4096 前像 + 4096 后像；BEGIN/COMMIT：无
//   尾部 4B  u32 FNV-1a（覆盖 [0, total_len-4)）—— 撕裂尾/位翻转都逃不过它
// ---------------------------------------------------------------------------
struct WalRec {
    uint64_t lsn = 0;
    uint32_t txn = 0;
    uint8_t type = 0;
    uint32_t pid = 0;
    std::vector<uint8_t> before, after;   // UPDATE 的整页前后像
};

class WalLog {
public:
    uint64_t appends = 0, fsyncs = 0;

    explicit WalLog(const std::string &path) : path_(path) {
        wal_ = ::fopen(path.c_str(), "a+b");   // 不存在就创建，存在就追加
        if (!wal_) throw std::runtime_error("无法打开 WAL: " + path);
        std::fseek(wal_, 0, SEEK_END);
    }
    ~WalLog() { if (wal_) std::fclose(wal_); }
    WalLog(const WalLog &) = delete;
    WalLog &operator=(const WalLog &) = delete;

    size_t size() const {
        long cur = std::ftell(wal_);
        return size_t(cur < 0 ? 0 : cur);
    }

    void append(uint64_t lsn, uint32_t txn, uint8_t type, uint32_t pid,
                const uint8_t *before, const uint8_t *after) {
        const size_t payload = (type == kRecUpdate) ? size_t(kPageSize) * 2 : 0;
        std::vector<uint8_t> buf(size_t(kWalHdr) + payload + kRecCrcTail, 0);
        putU32(buf.data() + 0, kWalMagic);
        putU32(buf.data() + 4, uint32_t(buf.size()));
        putU64(buf.data() + 8, lsn);
        putU32(buf.data() + 16, txn);
        buf[20] = type;
        putU32(buf.data() + 24, pid);
        if (payload) {
            std::memcpy(buf.data() + kWalHdr, before, kPageSize);
            std::memcpy(buf.data() + kWalHdr + kPageSize, after, kPageSize);
        }
        putU32(buf.data() + buf.size() - kRecCrcTail, fnv1a(buf.data(), buf.size() - kRecCrcTail));
        std::fseek(wal_, 0, SEEK_END);
        if (std::fwrite(buf.data(), 1, buf.size(), wal_) != buf.size())
            throw std::runtime_error("WAL 追加失败");
        std::fflush(wal_);
        ::_commit(_fileno(wal_));            // 纪律①②：日志必须先于一切落地
        ++appends;
        ++fsyncs;
    }

    // 场景 B 专用：故意留一条"半截记录"在尾部，模拟 fsync 之前断电的撕裂尾
    void appendTornTailForDemo() {
        std::vector<uint8_t> buf(kWalHdr + kPageSize + kPageSize + kRecCrcTail, 0xAB);
        putU32(buf.data() + 0, kWalMagic);
        putU32(buf.data() + 4, uint32_t(buf.size()));   // 声称完整，实际只写一半
        std::fseek(wal_, 0, SEEK_END);
        std::fwrite(buf.data(), 1, kWalHdr + 1000, wal_);
        std::fflush(wal_);
        ::_commit(_fileno(wal_));
    }

    // 顺序扫全文件；返回最后一条"校验通过"记录的末尾位置。
    // 读不满 / magic 或长度离谱 / 校验和不符 → 从那里开始就是撕裂尾。
    void scan(std::vector<WalRec> *recs, size_t *goodEnd) const {
        recs->clear();
        *goodEnd = 0;
        std::FILE *fp = ::fopen(path_.c_str(), "rb");
        if (!fp) return;
        std::vector<uint8_t> hdr(kWalHdr);
        size_t off = 0;
        for (;;) {
            size_t got = std::fread(hdr.data(), 1, kWalHdr, fp);
            if (got == 0) break;                         // 正常 EOF
            if (got < kWalHdr) break;                    // 半截头：撕裂尾
            if (getU32(hdr.data()) != kWalMagic) break;
            const uint32_t len = getU32(hdr.data() + 4);
            if (len < kWalHdr + kRecCrcTail || len > kWalHdr + kPageSize * 2 + kRecCrcTail)
                break;                                   // 长度离谱：撕裂尾
            std::vector<uint8_t> buf((size_t(len)));   // 多一层括号：防"最令人头疼的解析"
            std::memcpy(buf.data(), hdr.data(), kWalHdr);
            if (std::fread(buf.data() + kWalHdr, 1, size_t(len) - kWalHdr, fp) != size_t(len) - kWalHdr)
                break;                                   // 半截体：撕裂尾
            const uint32_t crc = getU32(buf.data() + len - kRecCrcTail);
            if (fnv1a(buf.data(), size_t(len) - kRecCrcTail) != crc)
                break;                                   // 校验和不对：撕裂尾/位翻转
            WalRec r;
            r.lsn = getU64(buf.data() + 8);
            r.txn = getU32(buf.data() + 16);
            r.type = buf[20];
            r.pid = getU32(buf.data() + 24);
            if (r.type == kRecUpdate) {
                r.before.assign(buf.begin() + kWalHdr, buf.begin() + kWalHdr + kPageSize);
                r.after.assign(buf.begin() + kWalHdr + kPageSize, buf.begin() + kWalHdr + kPageSize * 2);
            }
            recs->push_back(std::move(r));
            off += size_t(len);
            *goodEnd = off;
        }
        std::fclose(fp);
    }

    void truncateTo(size_t goodEnd) {
        if (goodEnd < size()) {
            ::_chsize_s(_fileno(wal_), long(goodEnd));
            std::fseek(wal_, 0, SEEK_END);
        }
    }
    void truncateAll() { truncateTo(0); }

private:
    std::string path_;
    std::FILE *wal_ = nullptr;
};

// ---------------------------------------------------------------------------
// WalDB：带 WAL 的最小页式存储。
//
//   元数据页（页 0）：magic/version/page_size/page_count/checkpoint_lsn/next_lsn+crc
//   数据页（页 1..4）：4096 字节 × 64 槽 × 64B 的"账户格子"（u64 余额 + 填充），
//                     够我们演示转账与撕裂，不必再背堆表那套行格式。
//
//   no-steal 纪律：脏页只活在内存，只有 checkpoint 才写数据文件。所以未提交事务
//   的改动压根没上过盘，恢复时"丢弃"就是什么都不做 —— 这是最简单也最安全的起点。
//   （真正的数据库允许 steal + UNDO，那是为了吞吐，以后再学。）
// ---------------------------------------------------------------------------
class WalDB {
public:
    struct RecoveryReport {
        uint64_t scanned = 0;        // WAL 里校验通过的记录数
        uint64_t skippedByCkpt = 0;  // LSN <= checkpoint 边界，跳过
        uint64_t redone = 0;         // 实际重放的 UPDATE 数
        std::vector<uint32_t> committed, discarded;
        size_t walBytesBefore = 0, tornBytes = 0;
    };
    struct Stats { uint64_t walAppends = 0, pagesWritten = 0, ckpts = 0; };

    WalDB(const std::string &dbPath, const std::string &walPath)
        : dbPath_(dbPath), wal_(walPath) {
        db_ = ::fopen(dbPath.c_str(), "r+b");
        if (!db_) {                       // 新建数据库：只写元数据页
            db_ = ::fopen(dbPath.c_str(), "w+b");
            if (!db_) throw std::runtime_error("无法创建数据库: " + dbPath);
            page_count_ = 1;
            flushMeta();
        } else {
            readMeta();
        }
    }
    ~WalDB() { if (db_) std::fclose(db_); }
    WalDB(const WalDB &) = delete;
    WalDB &operator=(const WalDB &) = delete;

    uint32_t allocPage() { ++page_count_; markMetaDirty(); return page_count_ - 1; }
    uint32_t pageCount() const { return page_count_; }
    uint64_t nextLsn() const { return next_lsn_; }
    uint64_t ckptLsn() const { return ckpt_lsn_; }
    const Stats &stats() const { return st_; }

    std::vector<uint8_t> copyPage(uint32_t pid) { return loadPage(pid); }

    uint64_t cell(uint32_t pid, size_t row) {
        auto p = loadPage(pid);
        return getU64(p.data() + row * 64);
    }

    uint32_t begin() {
        cur_txn_ = ++last_txn_;
        wal_.append(next_lsn_++, cur_txn_, kRecBegin, 0, nullptr, nullptr);
        return cur_txn_;
    }

    // 改页前先把 (lsn, pid, 前像, 后像) 塞进 WAL 并 fsync —— 这就是预写日志
    void setCell(uint32_t pid, size_t row, uint64_t value) {
        auto before = loadPage(pid);
        auto after = before;
        putU64(after.data() + row * 64, value);
        wal_.append(next_lsn_++, cur_txn_, kRecUpdate, pid, before.data(), after.data());
        pages_[pid] = std::move(after);
        dirty_.insert(pid);
        ++st_.walAppends;
    }

    // 纪律②：commit 只 fsync WAL。数据页一个字节都不碰。
    void commit() {
        if (!cur_txn_) return;
        wal_.append(next_lsn_++, cur_txn_, kRecCommit, 0, nullptr, nullptr);
        cur_txn_ = 0;
    }

    // 重启后的恢复：两遍扫描。第一遍收集 COMMIT 集合（COMMIT 记录可能在后面，
    // 不先收齐就不知道每条 UPDATE 的 txn 提没提交），第二遍按 LSN 序 REDO。
    void recover(RecoveryReport *rp) {
        rp->walBytesBefore = wal_.size();
        std::vector<WalRec> recs;
        size_t goodEnd = 0;
        wal_.scan(&recs, &goodEnd);
        rp->scanned = recs.size();
        rp->tornBytes = rp->walBytesBefore - goodEnd;
        if (rp->tornBytes) wal_.truncateTo(goodEnd);   // 截掉撕裂尾

        std::unordered_set<uint32_t> committed;
        std::unordered_set<uint32_t> touched;
        for (const auto &r : recs) {
            touched.insert(r.txn);
            if (r.type == kRecCommit) committed.insert(r.txn);
        }
        for (uint32_t t : touched)
            (committed.count(t) ? rp->committed : rp->discarded).push_back(t);

        for (const auto &r : recs) {
            if (r.lsn <= ckpt_lsn_) { ++rp->skippedByCkpt; continue; }   // 边界以下
            if (!committed.count(r.txn)) continue;                        // 未提交：丢弃
            if (r.type == kRecUpdate) {                                   // REDO 后像
                pages_[r.pid] = r.after;
                dirty_.insert(r.pid);
                if (r.pid >= page_count_) { page_count_ = r.pid + 1; markMetaDirty(); }
                ++rp->redone;
            }
        }
    }

    // 检查点：脏页落盘 + fsync 数据文件 → 元数据记 checkpoint LSN → 截断 WAL。
    // 顺序不能反：页没落盘就截日志，等于把唯一的救命绳剪了。
    double checkpoint(bool truncateWal) {
        const double t0 = nowMs();
        std::fseek(db_, 0, SEEK_END);
        long have = std::ftell(db_);
        growFile(have);
        for (uint32_t pid : dirty_) {
            const auto &p = pages_[pid];
            if (::fseek(db_, long(pid) * long(kPageSize), SEEK_SET) != 0 ||
                std::fwrite(p.data(), 1, kPageSize, db_) != kPageSize)
                throw std::runtime_error("checkpoint 写页失败");
            ++st_.pagesWritten;
        }
        dirty_.clear();
        ckpt_lsn_ = next_lsn_ - 1;      // 边界 = 当前最后一条已持久化的记录
        flushMeta();                     // 元数据页也落盘（fsync 在同一次 commit 里）
        if (truncateWal) wal_.truncateAll();
        ++st_.ckpts;
        return nowMs() - t0;
    }

private:
    std::vector<uint8_t> loadPage(uint32_t pid) {
        auto it = pages_.find(pid);
        if (it != pages_.end()) return it->second;
        std::vector<uint8_t> buf(kPageSize, 0);
        if (pid < page_count_) {
            if (::fseek(db_, long(pid) * long(kPageSize), SEEK_SET) == 0)
                if (std::fread(buf.data(), 1, kPageSize, db_) != kPageSize)
                    std::fill(buf.begin(), buf.end(), uint8_t(0));
        }
        pages_[pid] = buf;
        return buf;
    }
    void markMetaDirty() { /* 元数据在 checkpoint/flushMeta 时统一落盘 */ }

    void growFile(long have) {
        const long need = long(page_count_) * long(kPageSize);
        static const std::vector<uint8_t> zeros(kPageSize, 0);
        for (; have < need; have += long(kPageSize))
            std::fwrite(zeros.data(), 1, kPageSize, db_);
    }

    void flushMeta() {
        std::vector<uint8_t> m(kPageSize, 0);
        putU32(m.data() + 0, kMagic);
        putU32(m.data() + 4, kVersion);
        putU32(m.data() + 8, kPageSize);
        putU32(m.data() + 12, page_count_);
        putU64(m.data() + 16, ckpt_lsn_);
        putU64(m.data() + 24, next_lsn_);
        putU32(m.data() + 32, fnv1a(m.data(), 32));
        std::fseek(db_, 0, SEEK_SET);
        std::fwrite(m.data(), 1, kPageSize, db_);
        std::fflush(db_);
        ::_commit(_fileno(db_));
    }

    void readMeta() {
        std::vector<uint8_t> m(kPageSize, 0);
        if (std::fread(m.data(), 1, kPageSize, db_) != kPageSize)
            throw std::runtime_error("数据文件太短");
        if (getU32(m.data()) != kMagic) throw std::runtime_error("magic 不匹配");
        if (fnv1a(m.data(), 32) != getU32(m.data() + 32))
            throw std::runtime_error("元数据页 checksum 失败");
        page_count_ = getU32(m.data() + 12);
        ckpt_lsn_ = getU64(m.data() + 16);
        next_lsn_ = getU64(m.data() + 24);
        last_txn_ = uint32_t(next_lsn_);   // 事务号续在 LSN 后面，保证不回绕撞车
    }

    std::string dbPath_;
    std::FILE *db_ = nullptr;
    WalLog wal_;
    uint32_t page_count_ = 1;
    uint64_t ckpt_lsn_ = 0, next_lsn_ = 1;
    uint32_t last_txn_ = 0, cur_txn_ = 0;
    std::unordered_map<uint32_t, std::vector<uint8_t>> pages_;
    std::unordered_set<uint32_t> dirty_;
    Stats st_;
};

// ---------------------------------------------------------------------------
// 演示用的"账户格子"：页 1..4，每页 64 行，行 r 全局编号 = (pid-1)*64 + r
// ---------------------------------------------------------------------------
uint64_t rawCell(const std::string &path, uint32_t pid, size_t row) {
    std::FILE *fp = ::fopen(path.c_str(), "rb");
    if (!fp) return ~0ull;
    uint8_t b[8];
    size_t got = 0;
    if (::fseek(fp, long(pid) * long(kPageSize) + long(row * 64), SEEK_SET) == 0)
        got = std::fread(b, 1, 8, fp);
    std::fclose(fp);
    return got == 8 ? getU64(b) : ~0ull;
}
// 崩溃场景里的同一笔"事务"：r0 1000→900；r64 1000→1150；r96 1000→950（总和不变）
void applyDemoTxn(WalDB &db) {
    db.setCell(1, 0, 900);
    db.setCell(2, 0, 1150);
    db.setCell(2, 32, 950);     // 页 2 行 32 = 全局 r96 = 页内偏移 2048，撕裂线的正下方
}
bool checkDemo(const std::string &tag, uint64_t a, uint64_t b, uint64_t c,
               uint64_t wa, uint64_t wb, uint64_t wc) {
    const bool ok = (a == wa && b == wb && c == wc);
    std::cout << "  " << tag << "：r0=" << a << "  r64=" << b << "  r96=" << c
              << "  → " << (ok ? "✓" : "✗");
    if (!ok) std::cout << "（期望 " << wa << "/" << wb << "/" << wc << "）";
    std::cout << "\n";
    return ok;
}

// ---------------------------------------------------------------------------
// 子进程模式：干到一半 exit(9)，析构不跑、数据文件不 flush —— 模拟真崩溃
// ---------------------------------------------------------------------------
int childMain(const std::string &cmd) {
    if (cmd == "prep") {
        WalDB db(kDb, kWal);
        for (uint32_t p = 0; p < 4; ++p) db.allocPage();          // 页 1..4
        db.begin();
        for (uint32_t p = 1; p <= 4; ++p)
            for (size_t r = 0; r < 64; ++r) db.setCell(p, r, 1000);
        db.commit();
        // 基线 checkpoint：脏页落盘 + 记 ckpt_lsn，但保留 WAL（延迟截断），
        // 让后面的恢复演示"按 checkpoint 边界跳过旧记录"到底跳没跳。
        db.checkpoint(/*truncateWal=*/false);
        std::cout << "  [子] prep 完成：基线 " << db.cell(1, 0)
                  << " × 256 行已落盘，ckpt_lsn=" << db.ckptLsn() << "\n";
        return 0;
    }
    if (cmd == "crashA") {
        WalDB db(kDb, kWal);
        WalDB::RecoveryReport rp;
        db.recover(&rp);                       // 干净状态，啥也不用补
        db.begin();
        applyDemoTxn(db);
        db.commit();                           // COMMIT 已 fsync，改动是"别人的了"
        // —— 开始 checkpoint，写到一半"断电"：页 1 全写完，页 2 只写前 2048 字节
        auto p1 = db.copyPage(1), p2 = db.copyPage(2);
        std::FILE *fp = ::fopen(kDb.c_str(), "r+b");
        // 注意：fopen 之后文件位置在 0，必须先 seek 到页 1 的偏移，
        // 否则会把页 1 整个盖到元数据页上 —— magic 当场报废。
        // 写完一整页后位置自然落在页 2 开头，正好接着写半页。
        std::fseek(fp, long(kPageSize), SEEK_SET);
        std::fwrite(p1.data(), 1, kPageSize, fp);                    // 页 1 完整落盘
        std::fwrite(p2.data(), 1, kPageSize / 2, fp);                // 页 2 写半页
        std::fflush(fp);
        ::_commit(_fileno(fp));                // 保证"撕裂状态"真的上了盘
        std::fclose(fp);
        std::cout << "  [子] 页 1 写完、页 2 只写了一半 —— 断电！\n";
        std::cout.flush();
        std::exit(9);                          // 不跑析构，直接蒸发
    }
    if (cmd == "crashB") {
        WalDB db(kDb, kWal);
        WalDB::RecoveryReport rp;
        db.recover(&rp);
        db.begin();
        applyDemoTxn(db);                      // 三条 UPDATE 都已进 WAL 并 fsync
        WalLog w(kWal);
        w.appendTornTailForDemo();             // 再留一条半截记录当撕裂尾
        std::cout << "  [子] 事务没提交就断电，顺便留了条撕裂尾\n";
        std::cout.flush();
        std::exit(9);
    }
    if (cmd == "crashC") {
        WalDB db(kDb, kWal);
        WalDB::RecoveryReport rp;
        db.recover(&rp);
        db.begin();
        applyDemoTxn(db);
        db.commit();                           // COMMIT 已 fsync（纪律②），但没 checkpoint
        std::cout << "  [子] 已提交、没来得及 checkpoint 就断电\n";
        std::cout.flush();
        std::exit(9);
    }
    std::cerr << "未知子命令: " << cmd << "\n";
    return 2;
}

// ---------------------------------------------------------------------------
// 父进程：拉起子进程、检查盘上裸数据、跑恢复、对账
// ---------------------------------------------------------------------------
int spawn(const std::string &exe, const std::string &cmd) {
    return std::system(("\"" + exe + "\" --child " + cmd).c_str());
}

// 恢复 + 收尾 checkpoint + 重开对账。返回 0 = 场景通过。
int recoverAndVerify(uint64_t wa, uint64_t wb, uint64_t wc,
                     bool expectTornTruncated) {
    WalDB db(kDb, kWal);
    WalDB::RecoveryReport rp;
    const double t0 = nowMs();
    db.recover(&rp);
    const double tRec = nowMs() - t0;
    std::cout << "  [恢复] 扫过 " << rp.scanned << " 条记录：ckpt 边界(" << db.ckptLsn()
              << ")以下跳过 " << rp.skippedByCkpt << " 条，REDO " << rp.redone
              << " 条；撕裂尾截掉 " << rp.tornBytes << " 字节\n";
    std::cout << "  [恢复] 已提交并重放的事务：" << rp.committed.size()
              << " 个；未提交被丢弃：" << rp.discarded.size()
              << " 个；耗时 " << ms(tRec) << "\n";
    if (expectTornTruncated && rp.tornBytes == 0) {
        std::cout << "  ✗ 期望截掉撕裂尾，实际没截到\n";
        return 1;
    }
    if (!expectTornTruncated && rp.tornBytes != 0) {
        std::cout << "  ✗ 不该有撕裂尾，却截掉了 " << rp.tornBytes << " 字节\n";
        return 1;
    }
    const double tCk = db.checkpoint(/*truncateWal=*/true);
    std::cout << "  [恢复] 收尾 checkpoint：" << ms(tCk) << "，WAL 已截断至 "
              << (size_t)0 << "\n";

    WalDB db2(kDb, kWal);                       // 重开：盘上的数据必须原样能用
    WalDB::RecoveryReport rp2;
    db2.recover(&rp2);
    const bool ok = checkDemo("恢复并 checkpoint 后", db2.cell(1, 0), db2.cell(2, 0),
                              db2.cell(2, 32), wa, wb, wc);
    const uint64_t sum = [&]() {
        uint64_t s = 0;
        for (uint32_t p = 1; p <= 4; ++p)
            for (size_t r = 0; r < 64; ++r) s += db2.cell(p, r);
        return s;
    }();
    std::cout << "  全表总和守恒检查：" << sum << (sum == 256000 ? " ✓" : " ✗") << "\n";
    return (ok && sum == 256000) ? 0 : 1;
}

void freshFiles() {
    std::remove(kDb.c_str());
    std::remove(kWal.c_str());
}

int parentMain(const std::string &exe) {
    int rc = 0;

    hr("场景 A：checkpoint 写页中途崩 —— 撕裂页被 REDO 修好");
    freshFiles();
    std::cout << "  [父] 拉子进程灌基线数据…\n";
    if (spawn(exe, "prep") != 0) return 1;
    std::cout << "  [父] 拉子进程跑事务后在 checkpoint 中途断电…\n";
    const int rcA = spawn(exe, "crashA");
    std::cout << "  [父] 子进程退出码 " << rcA << "（9 = 崩溃成功）\n";
    if (rcA != 9) return 1;
    // 恢复前先看裸文件：页 2 前一半是新影（r64=1150），后一半是旧身（r96=1000）
    std::cout << "  [父] 恢复前直接读盘："
              << "页1.r0=" << rawCell(kDb, 1, 0)
              << "，页2.r64=" << rawCell(kDb, 2, 0)
              << "，页2.r96=" << rawCell(kDb, 2, 32) << "\n";
    std::cout << "  ↑ 1150 和 1000 同时出现 = 半新半旧的杂交页，任何合法状态都不长这样\n";
    rc |= recoverAndVerify(900, 1150, 950, false);

    hr("场景 B：提交前崩 —— 未提交的改动必须人间蒸发");
    freshFiles();
    if (spawn(exe, "prep") != 0) return 1;
    const int rcB = spawn(exe, "crashB");
    std::cout << "  [父] 子进程退出码 " << rcB << "（9 = 崩溃成功）\n";
    if (rcB != 9) return 1;
    std::cout << "  [父] 恢复前直接读盘：r0=" << rawCell(kDb, 1, 0)
              << "，r64=" << rawCell(kDb, 2, 0) << "（脏页从没落过盘，no-steal）\n";
    WalLog wlBefore(kWal);
    std::cout << "  [父] WAL 现有 " << wlBefore.size() << " 字节，尾部埋着半截记录\n";
    rc |= recoverAndVerify(1000, 1000, 1000, true);

    hr("场景 C：提交后崩 —— COMMIT 已 fsync，数据必须靠 REDO 找回");
    freshFiles();
    if (spawn(exe, "prep") != 0) return 1;
    const int rcC = spawn(exe, "crashC");
    std::cout << "  [父] 子进程退出码 " << rcC << "（9 = 崩溃成功）\n";
    if (rcC != 9) return 1;
    std::cout << "  [父] 恢复前直接读盘：r0=" << rawCell(kDb, 1, 0)
              << "，r64=" << rawCell(kDb, 2, 0) << "（数据文件还是旧的！）\n";
    rc |= recoverAndVerify(900, 1150, 950, false);

    hr("师傅收尾");
    std::cout << "  A 证明：撕裂页 + 整页后像 = REDO 天然修页，幂等重放不怕写一半；\n"
              << "  B 证明：COMMIT 记录是唯一的准入证，没这张纸的改动一概不认；\n"
              << "  C 证明：commit 只 fsync WAL 敢这么狂，是因为恢复兜得住。\n"
              << "  " << (rc == 0 ? "三种场景全部恢复成功 ✓" : "有场景未通过 ✗") << "\n";
    return rc;
}

}   // namespace

int main(int argc, char *argv[]) {
    SetConsoleOutputCP(65001);
    if (argc >= 3 && std::strcmp(argv[1], "--child") == 0)
        return childMain(argv[2]);
    char self[MAX_PATH];
    ::GetModuleFileNameA(nullptr, self, MAX_PATH);
    return parentMain(self);
}
