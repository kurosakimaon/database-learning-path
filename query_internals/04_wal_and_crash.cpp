// =====================================================================
//  04_wal_and_crash.cpp —— 内存断电即失：WAL 怎么写账，崩溃怎么还账
//
//  编译：g++ -std=c++17 -O2 -Wall -Wextra 04_wal_and_crash.cpp -o t04.exe
//  运行：.\t04.exe            （全程在当前目录建 engine.db / engine.wal，
//                              跑完自动删除，不留垃圾）
//
//  第 2 关对付的是「磁盘按页读写」；这一关对付矛盾的另一半：
//  「内存断电即失」。MiniEngine 只有 16 个页，每页存一个余额（int64）。
//  更新余额不能直接改数据页就完事 —— 改到一半断电，钱就花了但货没发。
//  WAL（Write-Ahead Log）的规矩：
//    1. 想改哪页，先往 engine.wal 追加一条记录（LSN + 页号 + 新值 + 校验和）
//    2. 事务提交 = 往 WAL 追加一条 COMMIT 记录，然后 fsync —— 这一步做完
//       才对客户端说「提交成功」。数据页可以慢慢刷，甚至不刷。
//    3. 断电重启：扫 WAL，有 COMMIT 的事务重放（REDO），没有的丢弃。
//    4. checkpoint：把脏页刷进 engine.db，然后清空 WAL。
//
//  C++ 类比：WAL 就像你给关键状态变更写的「只追加、带校验的审计日志」，
//  崩溃恢复 = 从日志重放状态机。事件溯源（event sourcing)是同一个思想。
// =====================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>  // offsetof（算校验和覆盖范围）
#include <io.h>     // Windows 的 _commit（fsync）、_fileno、_chsize
#include <chrono>
#include <string>
#include <vector>
#include <unordered_set>

// ------------------------- 公共小工具 --------------------------------

// FNV-1a：一行搞定的非加密校验和。这里防的是「写了一半的烂尾记录」
// （torn write），不是防黑客 —— 防篡改要用 CRC32C 硬件指令或更强算法。
static uint32_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// 可复现随机数（跟 01 用同一套思路，别用 rand() 看机器心情）
struct Lcg {
    uint64_t s;
    uint32_t next(uint32_t mod) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return (uint32_t)((s >> 33) % mod);
    }
};

// ------------------------- WAL 记录格式 ------------------------------
// 定长记录，落盘靠 fwrite 原样写。定长 + 每条自带校验和，
// 恢复时才能从头一条条读、读到哪算哪。
enum RecType : uint32_t { REC_WRITE = 1, REC_COMMIT = 2 };

struct WalRecord {
    uint64_t lsn;       // 日志序列号：全局单调递增，就是「账本行号」
    uint32_t txid;      // 事务号
    uint32_t pageId;    // 改的是哪页（REC_COMMIT 时无意义，填 0）
    int64_t  value;     // 新值（REC_COMMIT 时无意义）
    uint32_t type;      // REC_WRITE / REC_COMMIT
    uint32_t pad;       // 对齐占位，显式写出来免得各编译器猜
    uint32_t checksum;  // 前面所有字节的 FNV-1a（不含本字段）
};
static_assert(sizeof(WalRecord) == 40, "WalRecord 必须是 40 字节，格式就完蛋了");

static uint32_t recChecksum(const WalRecord &r) {
    // 覆盖范围 = checksum 字段之前的所有字节。有个真实踩过的坑：
    // 「sizeof(记录) - sizeof(checksum)」看着对，其实被尾部填充(padding)
    // 阴了 —— 本结构有 8 字节对齐要求，字段净占 36 字节，sizeof 凑到 40，
    // 尾部 4 字节 padding 排在 checksum【后面】，checksum 不在最后，
    // 用减法就把 checksum 自己算进了校验 → 恢复时永远校验失败。
    // offsetof 才是该算的边界。真实引擎的日志格式都显式定长、无 padding。
    return fnv1a(&r, offsetof(WalRecord, checksum));
}

// ------------------------- MiniEngine --------------------------------
// 数据文件 engine.db：16 个固定大小的「页」，每页 4096 字节，
// 页头 8 字节是余额。为什么要整页读写？—— 第 2 关的老朋友：
// 磁盘按页搬，改 8 个字节也得把 4096 字节的页写回去。
static const int   N_PAGES   = 16;
static const int   PAGE_SIZE = 4096;
static const char *DB_PATH   = "engine.db";
static const char *WAL_PATH  = "engine.wal";

class MiniEngine {
public:
    // ---------- 打开（或初始化）引擎 ----------
    // pool_ 是「Buffer Pool」：内存里的页副本。改数据只改它，
    // 落不落盘、什么时候落，由 WAL 规则说了算。
    static MiniEngine openOrInit(bool freshInit, std::vector<std::string> &log,
                                 uint64_t *outNextLsn = nullptr) {
        MiniEngine e;
        if (freshInit) {
            FILE *f = fopen(DB_PATH, "wb");
            std::vector<char> page(PAGE_SIZE, 0);
            for (int p = 0; p < N_PAGES; ++p) {
                int64_t v = 1000; // 初始余额都是 1000
                memcpy(page.data(), &v, sizeof(v));
                fwrite(page.data(), 1, PAGE_SIZE, f);
            }
            fclose(f);
            FILE *w = fopen(WAL_PATH, "wb"); // 立刻关：清空 WAL
            fclose(w);
            e.loadPoolFromDb(); // 文件写好了才有得读
            log.push_back("  初始化：engine.db 16 页，每页余额 1000；engine.wal 清空");
        } else {
            e.loadPoolFromDb(); // pool_ 的容量在这里确定
            // 真实引擎的必修课：LSN 序列不能从 1 重来，否则新记录和
            // 旧记录撞号，恢复时张冠李戴。打开时扫一遍 WAL 取尾。
            FILE *r = fopen(WAL_PATH, "rb");
            WalRecord rec;
            while (fread(&rec, 1, sizeof(rec), r) == sizeof(rec)) {
                if (rec.checksum != recChecksum(rec)) break; // 烂尾：停在最后一条完整的
                if (rec.lsn >= e.nextLsn_) e.nextLsn_ = rec.lsn + 1;
            }
            fclose(r);
        }
        e.wal_ = fopen(WAL_PATH, "ab"); // 只追加
        if (outNextLsn) *outNextLsn = e.nextLsn_;
        return e;
    }

    ~MiniEngine() { closeWal(); }

    MiniEngine() = default;
    // 禁拷贝（wal_ 是裸 FILE*，默认拷贝 = 双重 fclose）；允许移动
    MiniEngine(const MiniEngine &) = delete;
    MiniEngine &operator=(const MiniEngine &) = delete;
    MiniEngine(MiniEngine &&o) noexcept
        : wal_(o.wal_), pool_(std::move(o.pool_)), nextLsn_(o.nextLsn_),
          curTxid_(o.curTxid_), poolDirtyHint_(o.poolDirtyHint_) {
        o.wal_ = nullptr; // 偷走句柄，原主析构时不会 double fclose
    }

    // ---------- 改一页：先写 WAL 记录，再改内存池 ----------
    void update(int pageId, int64_t newVal) {
        WalRecord r;
        r.lsn = nextLsn_++;
        r.txid = curTxid_;
        r.pageId = (uint32_t)pageId;
        r.value = newVal;
        r.type = REC_WRITE;
        r.pad = 0;
        r.checksum = recChecksum(r);
        fwrite(&r, 1, sizeof(r), wal_);       // 注意：此刻只进了 OS 缓冲
        pool_[pageId] = newVal;               // 内存页变脏，先不落盘
    }

    // ---------- 提交：追加 COMMIT 记录 + fsync WAL ----------
    // fsync 返回 = 数据已钉在磁盘上。这一步之前断电，事务当没发生；
    // 这一步之后断电，恢复时必须重放 —— 这就是「提交点」。
    void commit(std::vector<std::string> &log) {
        WalRecord r;
        r.lsn = nextLsn_++;
        r.txid = curTxid_;
        r.pageId = 0;
        r.value = 0;
        r.type = REC_COMMIT;
        r.pad = 0;
        r.checksum = recChecksum(r);
        fwrite(&r, 1, sizeof(r), wal_);
        fflush(wal_);
        _commit(_fileno(wal_)); // Windows 的 fsync
        log.push_back("  tx" + std::to_string(curTxid_) + " 提交：COMMIT 记录 + fsync WAL ——  durability 在这一刻兑现");
        curTxid_ = 0; // 事务结束，等 begin()
    }

    void begin(int txid) { curTxid_ = txid; }

    // ---------- 崩溃模拟 ----------
    // 「进程死掉」= 关掉文件句柄（OS 缓存里的字节还在，这就是现实：
    // 断电丢的是进程内存，OS 页缓存往往还活着），然后扔掉内存池。
    // 场景里「数据页没来得及刷盘」本来就是事实 —— 我们从不提前刷 pool_。
    void crash(std::vector<std::string> &log) {
        closeWal();
        log.push_back("  ⚡ 崩溃：内存池里 " + std::to_string(poolDirtyHint_) +
                      " 个脏页随着进程蒸发；engine.db 保持旧值");
        pool_.assign(N_PAGES, -777); // 打毒值：证明恢复只认磁盘，不认记忆
    }

    // ---------- checkpoint：脏页刷进 db，WAL 清空 ----------
    void checkpoint(std::vector<std::string> &log) {
        flushPoolToDb();
        fclose(wal_);
        wal_ = fopen(WAL_PATH, "wb");
        log.push_back("  checkpoint：脏页刷入 engine.db，WAL 清空（重放历史清零）");
    }

    int64_t page(int i) const { return pool_[i]; }

    // ---------- 崩溃恢复：扫 WAL → 定 COMMIT → REDO → 截尾 ----------
    struct RecoveryReport {
        int totalRecords = 0;      // 读到的完整记录数（含未提交的）
        int validBytes = 0;        // 校验通过的字节数
        int tornBytes = 0;         // 烂尾/坏校验被截掉的字节数
        std::vector<uint32_t> committedTx;
        int redone = 0;            // 重放的 WRITE 记录数
        int dropped = 0;           // 丢弃的未提交 WRITE 记录数
    };

    static RecoveryReport recover(std::vector<std::string> &log) {
        RecoveryReport rep;
        // 第一遍：从头扫，校验和当门卫；顺便收集有 COMMIT 的 txid
        std::unordered_set<uint32_t> hasCommit;
        std::vector<WalRecord> valid;
        FILE *f = fopen(WAL_PATH, "rb");
        WalRecord r;
        while (fread(&r, 1, sizeof(r), f) == sizeof(r)) {
            if (r.checksum != recChecksum(r)) {
                rep.tornBytes = (int)sizeof(r); // 整条坏掉：当烂尾截
                break;
            }
            rep.totalRecords++;
            valid.push_back(r);
            if (r.type == REC_COMMIT) hasCommit.insert(r.txid);
        }
        // 循环退出时，valid 前缀之后的所有字节都不可信：可能是坏校验的
        // 整条记录（含其后内容），也可能是不足一条记录的零头（torn write）。
        // 两种情况统一：烂尾 = 文件大小 − 有效前缀。
        fseek(f, 0, SEEK_END);
        long fileSize = ftell(f);
        rep.tornBytes = (int)(fileSize - (long)valid.size() * (long)sizeof(WalRecord));
        fclose(f);

        if (rep.tornBytes > 0) {
            // 注意：遇到坏记录就停，后面即使有完好的 COMMIT 也不认 ——
            // ARIES 同款纪律「前缀连续有效」，日志是追加的，截尾不伤历史
            log.push_back("  校验和拦下烂尾记录：截掉尾部 " +
                          std::to_string(rep.tornBytes) + " 字节，扫描停止");
            FILE *g = fopen(WAL_PATH, "r+b");
            _chsize(_fileno(g), (long)valid.size() * (long)sizeof(WalRecord));
            fclose(g);
        }

        rep.validBytes = (int)valid.size() * (int)sizeof(WalRecord);
        for (const auto &tx : hasCommit) rep.committedTx.push_back(tx);

        // 第二遍：按 LSN 顺序重放「有 COMMIT 的事务」的 WRITE
        MiniEngine e = openOrInit(false, log);
        for (const auto &rec : valid) {
            if (rec.type != REC_WRITE) continue;
            if (hasCommit.count(rec.txid)) {
                e.pool_[rec.pageId] = rec.value;
                rep.redone++;
            } else {
                rep.dropped++;
            }
        }
        log.push_back("  REDO 重放 " + std::to_string(rep.redone) + " 条（已提交）；丢弃 " +
                      std::to_string(rep.dropped) + " 条（无 COMMIT，当作没发生）");
        // 恢复完直接把结果落定，简化后续场景
        e.flushPoolToDb();
        fclose(e.wal_);
        e.wal_ = fopen(WAL_PATH, "wb");
        return rep;
    }

    void markDirtyHint(int n) { poolDirtyHint_ = n; }

private:
    void loadPoolFromDb() {
        pool_.assign(N_PAGES, 0);
        FILE *f = fopen(DB_PATH, "rb");
        for (int p = 0; p < N_PAGES; ++p) {
            fseek(f, (long)p * PAGE_SIZE, SEEK_SET);
            int64_t v = 0;
            fread(&v, 1, sizeof(v), f);
            pool_[p] = v;
        }
        fclose(f);
    }
    void flushPoolToDb() {
        FILE *f = fopen(DB_PATH, "r+b");
        std::vector<char> page(PAGE_SIZE, 0);
        for (int p = 0; p < N_PAGES; ++p) {
            memcpy(page.data(), &pool_[p], sizeof(int64_t));
            fseek(f, (long)p * PAGE_SIZE, SEEK_SET);
            fwrite(page.data(), 1, PAGE_SIZE, f);
        }
        fflush(f);
        _commit(_fileno(f));
        fclose(f);
    }
    void closeWal() {
        if (wal_) { fclose(wal_); wal_ = nullptr; }
    }

    FILE *wal_ = nullptr;
    std::vector<int64_t> pool_;       // Buffer Pool（每页只用 8 字节余额）
    uint64_t nextLsn_ = 1;            // 真实引擎：从 WAL 尾记录推出起点
    int curTxid_ = 0;
    int poolDirtyHint_ = 0;
};

// ------------------------- 打印小抄 ----------------------------------
static void showDbPages(const char *title, const std::vector<int> &pages) {
    FILE *f = fopen(DB_PATH, "rb");
    printf("  %s", title);
    for (int p : pages) {
        fseek(f, (long)p * PAGE_SIZE, SEEK_SET);
        int64_t v = 0;
        fread(&v, 1, sizeof(v), f);
        printf("页%d=%lld ", p, (long long)v);
    }
    fclose(f);
    printf("\n");
}

// ------------------------- I/O 计时：随机写 vs 顺序写 ----------------
// 这是 WAL 的另一半论据：如果每次提交都直接改数据页并 fsync，
// 那就是「每次一个随机 4KB 写 + 等盘」；WAL 把它变成顺序追加。
// 每笔都 fsync，才测得出真实代价（不然 OS 页缓存会把两者都糊过去）。
static void ioBenchmark() {
    const int N = 300;                 // 300 笔「事务」
    std::vector<char> blob(4096, 'D'); // 一笔一页
    Lcg rng{777};

    // 随机写：300 个随机页位，每次写完 fsync（模拟「不用 WAL，直写数据页」）
    FILE *f = fopen("rnd.dat", "wb");
    std::vector<char> pad((size_t)256 * PAGE_SIZE, 0);
    fwrite(pad.data(), 1, pad.size(), f); // 先铺 256 页
    fclose(f);
    f = fopen("rnd.dat", "r+b");
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        long pos = (long)rng.next(256) * PAGE_SIZE; // 随机页号
        fseek(f, pos, SEEK_SET);
        fwrite(blob.data(), 1, blob.size(), f);
        fflush(f);
        _commit(_fileno(f));          // 每笔都等到磁盘，才是提交语义
    }
    auto t1 = std::chrono::steady_clock::now();
    fclose(f);

    // 顺序写：同样 300 笔，全部追加到文件尾 + 每笔 fsync（模拟 WAL）
    f = fopen("seq.dat", "wb");
    auto t2 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        fwrite(blob.data(), 1, blob.size(), f);
        fflush(f);
        _commit(_fileno(f));
    }
    auto t3 = std::chrono::steady_clock::now();
    fclose(f);

    double rndMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double seqMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
    printf("  随机写 %d 笔（每笔 4KB + fsync）：%8.1f ms（平均 %.3f ms/笔）\n",
           N, rndMs, rndMs / N);
    printf("  顺序写 %d 笔（每笔 4KB + fsync）：%8.1f ms（平均 %.3f ms/笔）\n",
           N, seqMs, seqMs / N);
    if (seqMs > 0)
        printf("  随机/顺序 ≈ %.1f 倍（本机数字只参考量级，换块盘就差很多）：\n", rndMs / seqMs);
    printf("  · NVMe SSD 上随机 ≈ 顺序，很正常；机械盘上这个比值能到 50~100 倍。\n");
    printf("  · 但注意两者的平均耗时都在 1 ms 上下 —— 大头根本不是「随机 vs 顺序」，\n");
    printf("    而是每笔一次 fsync 的往返。WAL 省第一笔账（随机→顺序），\n");
    printf("    group commit 省第二笔账（N 笔凑一车共用一次 fsync），两笔都要算。\n");
    remove("rnd.dat");
    remove("seq.dat");
}

// =====================================================================
int main() {
    std::vector<std::string> notes; // 攒着小笔记，统一按场景打印

    printf("\n============================================================\n");
    printf("  第 5 关：内存断电即失 —— WAL 记账法 + 崩溃恢复\n");
    printf("============================================================\n\n");

    // ---------------- 场景 0：初始化 ----------------
    {
        MiniEngine e = MiniEngine::openOrInit(true, notes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();

        // ---------------- 场景 1：正常提交 + checkpoint ----------------
        printf("\n---- 场景 1：一笔正常事务（tx1 转账：页0 -100，页1 +100）----\n");
        e.begin(1);
        e.update(0, 900);
        e.update(1, 1100);
        printf("  改完内存池还没提交 —— 此刻断电，tx1 必须人间蒸发\n");
        e.commit(notes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        printf("  提交完成，但 engine.db 还是旧的：");
        showDbPages("磁盘上 →", {0, 1});
        e.checkpoint(notes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        printf("  刷完页后        ：");
        showDbPages("磁盘上 →", {0, 1});
    }

    // ---------------- 场景 2：提交后、刷页前崩溃 ----------------
    printf("\n---- 场景 2：提交后、checkpoint 前崩溃（WAL 已 fsync）----\n");
    {
        MiniEngine e = MiniEngine::openOrInit(false, notes); notes.clear();
        e.begin(2);
        e.update(2, 1500);
        e.update(3, 800);
        e.commit(notes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        e.markDirtyHint(2);
        printf("  此刻 engine.db：");
        showDbPages("页2/页3 仍是旧值 →", {2, 3});
        e.crash(notes);
        printf("%s\n", notes[0].c_str()); notes.clear();
    }
    {
        printf("  —— 重启，开始恢复 ——\n");
        MiniEngine::RecoveryReport rep = MiniEngine::recover(notes);
        printf("  WAL 里读到 %d 条完整记录，其中已提交事务 %d 个（tx2 在列）\n",
               rep.totalRecords, (int)rep.committedTx.size());
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        printf("  恢复后 engine.db：");
        showDbPages("钱回来了 →", {2, 3});
    }

    // ---------------- 场景 3：提交前崩溃（没有 COMMIT）----------------
    printf("\n---- 场景 3：改完页、还没来得及 COMMIT 就崩（tx4）----\n");
    {
        MiniEngine e = MiniEngine::openOrInit(false, notes); notes.clear();
        e.begin(4);
        e.update(4, 999); // WRITE 记录进了 WAL 缓冲（fclose 会流到 OS）
        printf("  tx4 把页4 改成 999，但 COMMIT 还没写 —— 客户没收到「提交成功」\n");
        e.markDirtyHint(1);
        e.crash(notes);
        printf("%s\n", notes[0].c_str()); notes.clear();
    }
    {
        printf("  —— 重启，开始恢复 ——\n");
        MiniEngine::RecoveryReport rep = MiniEngine::recover(notes);
        printf("  WAL 里有 tx4 的 WRITE 记录，但没有它的 COMMIT → 丢弃 %d 条\n", rep.dropped);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        printf("  恢复后 engine.db：");
        showDbPages("页4 维持旧值（999 丢了，丢得对）→", {4});
        printf("  判据就是 COMMIT 记录：客户没听到「提交成功」，引擎就有权丢。\n");
    }

    // ---------------- 场景 4：日志尾写烂了（torn write）----------------
    printf("\n---- 场景 4：tx5 正常提交；tx6 的记录写到一半断电 ----\n");
    {
        MiniEngine e = MiniEngine::openOrInit(false, notes); notes.clear();
        e.begin(5);
        e.update(5, 1234);
        e.commit(notes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        // 手工伪造烂尾：往 WAL 追加半条 tx6 记录（20 字节，一半）
        FILE *w = fopen(WAL_PATH, "ab");
        WalRecord r;
        r.lsn = 999; r.txid = 6; r.pageId = 5; r.value = -1;
        r.type = REC_WRITE; r.pad = 0; r.checksum = recChecksum(r);
        fwrite(&r, 1, sizeof(r) / 2, w); // 只写一半，模拟断电瞬间
        fclose(w);
        printf("  已向日志尾部塞入半条 tx6 记录（%d 字节，模拟写一半断电）\n",
               (int)(sizeof(WalRecord) / 2));
        e.markDirtyHint(1);
        e.crash(notes); notes.clear();
    }
    {
        printf("  —— 重启，开始恢复 ——\n");
        MiniEngine::RecoveryReport rep = MiniEngine::recover(notes);
        printf("  完整记录 %d 条，有效 %d 字节；tx5 已提交 → 重放；烂尾已截掉\n",
               rep.totalRecords, rep.validBytes);
        for (auto &s : notes) printf("%s\n", s.c_str());
        notes.clear();
        printf("  恢复后 engine.db：");
        showDbPages("tx5 的 1234 保住了 →", {5});
        printf("  没有校验和，这半条记录会被当成「tx6 把页5 改成垃圾」——\n");
        printf("  一次断电演变成静默数据损坏。校验和是 WAL 的最后一道门。\n");
    }

    // ---------------- 场景 5：WAL 到底省了什么（计时）----------------
    printf("\n---- 场景 5：不用 WAL（随机写数据页+fsync）vs 用 WAL（顺序追加）----\n");
    ioBenchmark();

    // ---------------- 收尾 ----------------
    printf("\n============================================================\n");
    printf("  老师傅总结\n");
    printf("============================================================\n");
    printf("  ◆ 恢复只有三条规矩（本文件演了三遍）：\n");
    printf("      1) 有 COMMIT 记录的事务 → REDO（按 LSN 顺序重放到页上）\n");
    printf("      2) 没有 COMMIT 的事务 → 丢弃，对客户从未提交过\n");
    printf("      3) 校验和之后的烂尾 → 截掉。日志是追加的，截尾不伤历史\n");
    printf("  ◆ 提交点 = WAL fsync 返回，不是「改完内存」、更不是「刷完数据页」。\n");
    printf("    数据页什么时候落盘无所谓 —— 只要 WAL 在，账就能复原。\n");
    printf("  ◆ C++ 类比：这就是「不可变日志 + 状态快照 + 重放」。你给关键\n");
    printf("    数据结构做崩溃安全，招数一样：先落审计日志，再改内存，\n");
    printf("    后台定期快照，重启时快照 + 日志尾巴 = 最新状态。\n");
    printf("  ◆ 真实引擎的对应物：\n");
    printf("      InnoDB  redo log（WAL）+ checkpoint + undo log（回滚用，本关未演）\n");
    printf("      MySQL   binlog group commit：多个事务凑一车共用一次 fsync\n");
    printf("      PG      WAL + full_page_writes：整页写解决页内 torn write\n");
    printf("      SQLite  journal_mode=WAL：本文件的 engine.wal 就是它的玩具版\n");
    printf("  ◆ 遗留问题：恢复期间查询怎么办？checkpoint 期间能写吗？\n");
    printf("    —— 那是并发控制（锁/MVCC）的故事，第 6 关见。\n\n");

    remove(DB_PATH);
    remove(WAL_PATH);
    printf("  清理：engine.db / engine.wal 已删除，目录不留垃圾。\n");
    return 0;
}
