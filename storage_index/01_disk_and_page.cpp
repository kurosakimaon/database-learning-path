// ================================================================
//  01_disk_and_page.cpp —— 页（Page）：数据库最小的读写单位
//  把"页"这个抽象，亲手做成一个能写进文件里的实体
// ================================================================
//
//  徒弟，先记住师傅这句话，它是整个第 2 关的总纲：
//
//      数据库的一切设计，都源于这对矛盾：
//      磁盘按"页"读写，内存断电即失。
//
//  为什么要有 Buffer Pool？因为内存断电即失，得先把热数据
//    攥在手里，还得记住哪些页"改了但没落盘"。
//  为什么要有 WAL 日志？因为内存断电即失，改的东西必须先
//    变成磁盘上的一条顺序记录，才算数。
//  为什么要有 B+ 树？因为磁盘按页读写，一次只能捞 4KB 回来，
//    那就让每个节点恰好塞满一页，一次页读取换一批分叉。
//  为什么页内用"偏移"而不用"指针"？因为内存里的地址下次开机
//    就不是那个地址了，断电即失嘛。
//
//  看见没，全是这对矛盾逼出来的。今天这一课，先把"磁盘按页
//  读写"这半边摸透：亲手写一个极简 Pager，把定长页写进 .db 文件。
//
//  编译： g++ -std=c++17 -O2 01_disk_and_page.cpp -o 01_disk_and_page.exe
//

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

// Windows 控制台默认不是 UTF-8，不开这行你看到的是一堆乱码方块
static void setup_console_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

// ================================================================
//  一、页的物理布局
// ================================================================
//
//  真实数据库（MySQL InnoDB 默认 16KB、SQLite 默认 4KB）的页，
//  内部都是"槽式页"（slotted page）这套布局：
//
//    偏移 0  ┌──────────────────────────┐
//            │  页头 header（12 字节）   │  页号 / 槽数 / 空闲区边界 / 校验和
//    偏移 12 ├──────────────────────────┤
//            │  槽数组 slot[]（每项 4B） │  ← 从前往后长
//            │    slot[i] = {偏移, 长度} │
//            ├──────────────────────────┤
//            │      空闲空间 free        │
//            ├──────────────────────────┤
//            │  记录数据 rows            │  ← 从后往前长
//    偏移 4095└──────────────────────────┘
//
//  为什么要"两头往中间长"？因为页定长（4096 字节），
//  固定切一刀的话，要么槽表浪费、要么记录区浪费，
//  两头长则把空闲空间全部留给"槽数 + 记录总长"，一点不浪费。
//
//  注意：槽里存的是【页内偏移】（uint16_t，2 字节），
//  不是指针（Windows/Linux 64 位下是指针 8 字节）。
//  这是 C++ 开发者最容易忽略的一层转换：
//    - 内存里你想怎么指都行，指针爽；
//    - 一旦要落盘，地址就不能用了（下次加载这块内存换地方了），
//      只能存"相对于页首的第几个字节"。
//  你把 std::map、std::list 直接 memcpy 到文件里再读回来试试？
//  一堆野指针当场爆炸。持久化格式里没有指针，只有偏移。
//

static constexpr uint32_t PAGE_SIZE   = 4096;               // 一页 4KB，和 SQLite 默认一致
static constexpr uint32_t HEADER_SIZE = 12;                 // 页头字节数
static constexpr uint32_t SLOT_SIZE   = 4;                  // 一个槽：偏移(2) + 长度(2)
static constexpr int32_t  META_PAGE   = 0;                  // 第 0 页固定当元数据页

// ---------------- 字节读写小工具（小端序） ----------------
// 为什么要手写？因为直接把结构体 fwrite 到文件，
// 字节序、对齐 padding 都跟编译器绑定，换台机器就废了。
// 真实数据库都是这样一个字段一个字段手动编解码的。
static void put_u16(array<uint8_t, PAGE_SIZE>& b, size_t off, uint16_t v) {
    b[off]     = static_cast<uint8_t>(v & 0xFF);
    b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
static uint16_t get_u16(const array<uint8_t, PAGE_SIZE>& b, size_t off) {
    return static_cast<uint16_t>(b[off] | (static_cast<uint16_t>(b[off + 1]) << 8));
}
static void put_i32(array<uint8_t, PAGE_SIZE>& b, size_t off, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
}
static int32_t get_i32(const array<uint8_t, PAGE_SIZE>& b, size_t off) {
    uint32_t u = 0;
    for (int i = 0; i < 4; ++i) u |= (static_cast<uint32_t>(b[off + i]) << (8 * i));
    return static_cast<int32_t>(u);
}

// ---------------- 一条记录 ----------------
struct Record {
    int32_t     id    = 0;
    int32_t     value = 0;
    string      name;

    // 序列化后的实际长度：id(4) + value(4) + name_len(1) + name
    uint32_t serialized_size() const { return 9 + static_cast<uint32_t>(name.size()); }
};

// ================================================================
//  二、Page：一个定长 4096 字节的缓冲区 + 槽式页的操作
// ================================================================
class Page {
public:
    int32_t                  id   = -1;                  // 内存里图方便，留个页号
    array<uint8_t, PAGE_SIZE> data{};                     // 真正的"页"，落盘的就是它

    // ---- 造一页空页 ----
    void init(int32_t page_id) {
        id = page_id;
        data.fill(0);
        put_i32(data, 0, page_id);       // 页号
        put_u16(data, 4, 0);             // 槽数 = 0
        put_u16(data, 6, 0xFFFF);        // 保留字段（演示用）
        put_u16(data, 8, PAGE_SIZE);     // 数据区尾部指针，从页尾往前长
        put_u16(data, 10, checksum());   // 校验和
    }

    // ---- 页头字段读取 ----
    int32_t  page_id()   const { return get_i32(data, 0); }
    uint16_t slot_count() const { return get_u16(data, 4); }
    uint16_t data_end()  const { return get_u16(data, 8); }

    void set_slot_count(uint16_t v) { put_u16(data, 4, v); }
    void set_data_end(uint16_t v)   { put_u16(data, 8, v); }

    // 槽区末尾在哪个偏移
    uint32_t slots_end() const { return HEADER_SIZE + uint32_t(slot_count()) * SLOT_SIZE; }

    // 还能塞多少字节（槽 + 记录都要地方）
    uint32_t free_space() const {
        uint32_t e = data_end();
        uint32_t s = slots_end();
        return (e > s) ? (e - s) : 0;
    }

    // ---- 在页内插一条记录，返回槽号；放不下返回 -1 ----
    int insert(const Record& rec) {
        uint32_t need = rec.serialized_size() + SLOT_SIZE;
        if (free_space() < need) return -1;               // 页满，真实数据库会在这里分裂新页

        uint16_t off = static_cast<uint16_t>(data_end() - rec.serialized_size());
        put_i32(data, off, rec.id);
        put_i32(data, off + 4, rec.value);
        data[off + 8] = static_cast<uint8_t>(rec.name.size());
        for (size_t i = 0; i < rec.name.size(); ++i)
            data[off + 9 + i] = static_cast<uint8_t>(rec.name[i]);

        uint16_t slot_idx = slot_count();
        uint32_t s = HEADER_SIZE + uint32_t(slot_idx) * SLOT_SIZE;
        put_u16(data, s, off);                            // 槽里存的是【页内偏移】
        put_u16(data, s + 2, static_cast<uint16_t>(rec.serialized_size()));

        set_slot_count(static_cast<uint16_t>(slot_idx + 1));
        set_data_end(off);
        refresh_checksum();
        return slot_idx;
    }

    // ---- 按槽号取记录 ----
    bool get(uint16_t slot_idx, Record& out) const {
        if (slot_idx >= slot_count()) return false;
        uint32_t s    = HEADER_SIZE + uint32_t(slot_idx) * SLOT_SIZE;
        uint16_t off  = get_u16(data, s);
        uint8_t  nlen = data[off + 8];
        out.id    = get_i32(data, off);
        out.value = get_i32(data, off + 4);
        out.name.assign(reinterpret_cast<const char*>(&data[off + 9]), nlen);
        return true;
    }

    // ---- 改页内一条记录：这里假设新记录不比原来长（真实数据库要处理挪位/溢出） ----
    bool update(uint16_t slot_idx, const Record& rec) {
        if (slot_idx >= slot_count()) return false;
        uint32_t s   = HEADER_SIZE + uint32_t(slot_idx) * SLOT_SIZE;
        uint16_t off = get_u16(data, s);
        put_i32(data, off, rec.id);
        put_i32(data, off + 4, rec.value);
        data[off + 8] = static_cast<uint8_t>(rec.name.size());
        for (size_t i = 0; i < rec.name.size(); ++i)
            data[off + 9 + i] = static_cast<uint8_t>(rec.name[i]);
        refresh_checksum();
        return true;
    }

    // ---- 简单校验和：模拟"页在磁盘上躺了三年，读回来怎么知道没坏" ----
    uint16_t checksum() const {
        uint32_t sum = 0;
        for (size_t i = 12; i < PAGE_SIZE; ++i) sum = (sum + data[i]) & 0xFFFF;
        return static_cast<uint16_t>(sum);
    }
    void refresh_checksum() { put_u16(data, 10, checksum()); }
    bool verify() const { return get_u16(data, 10) == checksum(); }

    void dump_head(int bytes = 48) const {
        for (int i = 0; i < bytes; i += 16) {
            printf("    %04X: ", i);
            for (int j = 0; j < 16; ++j) printf("%02X ", data[i + j]);
            printf("  |");
            for (int j = 0; j < 16; ++j) {
                uint8_t c = data[i + j];
                printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            printf("|\n");
        }
    }
};

// ================================================================
//  三、Pager：把定长页读写进 .db 文件
// ================================================================
//
//  这就是数据库存储引擎最内核的那一层，SQLite 里叫 osLayer，
//  InnoDB 里叫 fil 模块。核心只有两个动作：
//      read_page(id)  →  seekg(id * PAGE_SIZE) + 读 4096 字节
//      write_page(p)  →  seekp(id * PAGE_SIZE) + 写 4096 字节
//  注意"整页"两个字：你只要页里那 12 个字节，磁盘照样给你
//  搬 4096 个字节回来。I/O 放大就是从这儿开始的。
//
class Pager {
public:
    // 磁盘 I/O 计数器 —— 本节目的：让你亲眼看见每次读写
    uint64_t read_io  = 0;
    uint64_t write_io = 0;
    bool     verbose  = true;

    // fresh=true 表示建库（清空重建）；false 表示重新打开一个已存在的库。
    // 注意：这里只用 fstream 的 seekg/seekp + read/write，就这三个动作，
    // 真实数据库的存储层核心也就这三个动作。
    explicit Pager(const string& path, bool fresh = false) : path_(path) {
        const bool exists = std::filesystem::exists(path);
        if (fresh || !exists)
            file_.open(path, ios::binary | ios::in | ios::out | ios::trunc);
        else
            file_.open(path, ios::binary | ios::in | ios::out);
        if (!file_) {
            throw runtime_error("打不开数据库文件: " + path);
        }
        // 页号是从文件偏移算出来的，所以打开时顺手把已有页数恢复出来
        page_count_ = static_cast<uint32_t>(std::filesystem::file_size(path) / PAGE_SIZE);
    }
    ~Pager() { if (file_.is_open()) file_.close(); }

    Pager(const Pager&)            = delete;
    Pager& operator=(const Pager&) = delete;

    // 把文件撑到 page_count 页（相当于 fallocate 预分配）
    void extend_to(uint32_t page_count) {
        Page blank;
        for (uint32_t i = page_count_; i < page_count; ++i) {
            blank.init(static_cast<int32_t>(i));
            write_page(blank);
        }
        page_count_ = page_count;
    }

    void read_page(int32_t id, Page& out) {
        file_.seekg(static_cast<streamoff>(id) * PAGE_SIZE);   // 定位：seekg = 磁道寻址的替身
        file_.read(reinterpret_cast<char*>(out.data.data()), PAGE_SIZE);
        out.id = id;
        ++read_io;
        if (verbose)
            printf("    [磁盘I/O] READ  页 #%d  %u 字节   (累计 读=%llu 写=%llu)\n",
                   id, PAGE_SIZE, (unsigned long long)read_io, (unsigned long long)write_io);
    }

    void write_page(const Page& p) {
        file_.seekp(static_cast<streamoff>(p.id) * PAGE_SIZE);
        file_.write(reinterpret_cast<const char*>(p.data.data()), PAGE_SIZE);
        ++write_io;
        if (verbose)
            printf("    [磁盘I/O] WRITE 页 #%d  %u 字节   (累计 读=%llu 写=%llu)\n",
                   p.id, PAGE_SIZE, (unsigned long long)read_io, (unsigned long long)write_io);
    }

    // fsync 的替身：把 OS page cache 里的脏页真正刷到盘上。
    // 数据库提交前必须做这一步，否则"断电即失"就轮到你头上了。
    void flush() {
        file_.flush();
        file_.rdbuf()->pubsync();
        ++sync_io;
        if (verbose) printf("    [磁盘I/O] FSYNC 落盘 (累计 fsync=%llu)\n",
                            (unsigned long long)sync_io);
    }

    uint64_t total_io() const { return read_io + write_io; }
    uint64_t bytes_moved() const { return (read_io + write_io) * PAGE_SIZE; }
    uint32_t page_count() const { return page_count_; }

private:
    string   path_;
    fstream  file_;
    uint32_t page_count_ = 0;
    uint64_t sync_io     = 0;
};

// ================================================================
//  四、带淘汰策略的极简 Buffer Pool
// ================================================================
//
//  你写过带 LRU 的对象池，这就是同一个东西，只是"对象"换成了页。
//  三个新东西是数据库特有的：
//    1) pin（钉住）：正在用的页不许被淘汰，相当于引用计数 > 0
//    2) dirty（脏页）：内存改了但磁盘还没有，淘汰前必须先写回
//    3) 命中率：数据库性能的头号指标，比任何微优化都重要
//
class BufferPool {
public:
    BufferPool(Pager* pager, size_t frames) : pager_(pager), frames_(frames) {}

    Page& fetch(int32_t page_id) {
        ++requests;
        for (auto& f : pool_) {                       // 命中：池子里有这页
            if (f.valid && f.id == page_id) {
                ++hits;
                f.pin++;
                touch(f);
                cout << "      [BufferPool] 命中 页 #" << page_id
                     << "  → 不产生磁盘 I/O\n";
                return f.page;
            }
        }
        ++misses;
        Frame* slot = pick_victim();                  // 未命中：先腾地方
        if (slot->valid && slot->dirty) {             // 脏页必须先写回，否则数据永久丢失
            pager_->write_page(slot->page);
            cout << "      [BufferPool] 淘汰 页 #" << slot->page.id
                 << "（脏页，写回磁盘）\n";
        } else if (slot->valid) {
            cout << "      [BufferPool] 淘汰 页 #" << slot->page.id << "（干净页，直接丢）\n";
        }
        slot->valid  = true;
        slot->dirty  = false;
        slot->page   = Page{};
        pager_->read_page(page_id, slot->page);       // ← 一次真正的磁盘读
        slot->id     = page_id;
        slot->pin    = 1;
        touch(*slot);
        return slot->page;
    }

    void mark_dirty(Page& p) {
        for (auto& f : pool_) if (f.valid && f.page.data.data() == p.data.data()) f.dirty = true;
    }
    void unpin(Page& p) {
        for (auto& f : pool_) if (f.valid && f.page.data.data() == p.data.data() && f.pin > 0) f.pin--;
    }

    void flush_all_dirty() {
        for (auto& f : pool_)
            if (f.valid && f.dirty) { pager_->write_page(f.page); f.dirty = false; }
    }

    void report() const {
        cout << "\n  BufferPool 统计：请求 " << requests
             << " 次，命中 " << hits << " 次，未命中 " << misses << " 次，命中率 "
             << (requests ? 100.0 * hits / requests : 0.0) << "%\n";
    }

private:
    struct Frame {
        Page page;
        int32_t id    = -1;
        bool    valid = false;
        bool    dirty = false;
        int     pin   = 0;
        uint64_t seq  = 0;      // 越近访问越大
    };

    Frame* pick_victim() {
        Frame* victim = nullptr;
        for (auto& f : pool_) {
            if (!f.valid) return &f;                  // 有空帧直接用
            if (f.pin > 0) continue;                  // 钉住的不能踢
            if (!victim || f.seq < victim->seq) victim = &f;
        }
        if (!victim) throw runtime_error("所有帧都被钉住了（真实数据库会报 'all buffers pinned'）");
        return victim;
    }
    void touch(Frame& f) { f.seq = ++clock_; }

    Pager*      pager_;
    size_t      frames_;
    uint64_t    clock_    = 0;
    uint64_t    requests  = 0, hits = 0, misses = 0;
    vector<Frame> pool_{frames_};
};

// ================================================================
//  五、演示
// ================================================================
static void banner(const string& s) {
    cout << "\n──────────────────────────────────────────────────────────────\n"
         << "  " << s << "\n"
         << "──────────────────────────────────────────────────────────────\n";
}

int main() {
    setup_console_utf8();

    const string db_path = "demo_page.db";
    if (std::filesystem::exists(db_path)) std::filesystem::remove(db_path);

    cout << "================================================================\n"
         << "  页与 Pager：磁盘到底是怎么读写的\n"
         << "================================================================\n";

    // ---------------- 演示 1：建库 ----------------
    banner("演示 1：建一个 16 页的 .db 文件（每页固定 " + to_string(PAGE_SIZE) + " 字节）");
    {
        Pager pager(db_path, true);              // true = 清空重建
        pager.extend_to(16);
        pager.flush();
        cout << "\n  文件实际大小 = " << std::filesystem::file_size(db_path) << " 字节\n";
        cout << "  页数 = " << pager.page_count() << "，理论大小 = " << 16 * PAGE_SIZE << " 字节\n";
        cout << "  → 文件大小一定是页大小的整数倍，这就是'按页管理'的痕迹\n";
        cout << "  → 想清楚一件事：删空一张表，文件往往并不变小，因为页只是被\n"
             << "    标回'空闲页链表'，页本身还在文件里。空间回收是另一套机制。\n";
    }

    // ---------------- 演示 2：页内槽式布局 ----------------
    banner("演示 2：往第 1 页里塞记录，看清槽表与空闲区怎么两头长");
    {
        Pager pager(db_path);
        Page  p;
        pager.read_page(1, p);
        cout << "\n  刚建的空页：槽数=" << p.slot_count()
             << " 数据区尾=" << p.data_end()
             << " 空闲=" << p.free_space() << " 字节\n";

        vector<Record> recs = {
            {1001, 100, "zhang-san"},
            {1002, 200, "li-si"},
            {1003, 300, "wang-wu-the-third"},
            {1004, 400, "zhaoliu"},
        };
        for (const auto& r : recs) {
            int slot = p.insert(r);
            cout << "  插入 id=" << r.id << "（" << r.serialized_size()
                 << " 字节）→ 槽号 " << slot
                 << "，页内剩余空闲 " << p.free_space() << " 字节\n";
        }
        cout << "\n  第 1 页头部字节（前 32 字节 = 页头 + 4 个槽）：\n";
        p.dump_head(32);
        cout << "\n  解读：偏移 0~3 是页号；4~5 是槽数；8~9 是数据区尾指针；\n"
             << "        12 开始是槽数组，每 4 字节 = {页内偏移 2B, 记录长度 2B}。\n"
             << "        偏移全是 uint16_t，最大能指 65535 —— 所以 4KB 页最多塞\n"
             << "        65535 字节内容，这也是'大字段要放溢出页'的原因之一。\n";

        // 整页落盘：注意，为了这 4 条一共不到 60 字节的记录，写了 4096 字节
        pager.write_page(p);
        pager.flush();

        // ---------------- 演示 3：读一条记录要付多大代价 ----------------
        banner("演示 3：只要页里 26 个字节，磁盘给你 4096 个字节（I/O 放大）");
        Page q;
        pager.read_page(1, q);
        Record r;
        q.get(2, r);
        cout << "\n  取到记录：id=" << r.id << " value=" << r.value << " name=" << r.name << "\n";
        cout << "  记录本身 " << r.serialized_size() << " 字节，但为了它读了 " << PAGE_SIZE
             << " 字节 → 放大 " << (PAGE_SIZE / r.serialized_size()) << " 倍\n";
        cout << "  校验和验证：" << (q.verify() ? "通过" : "页损坏！") << "\n";

        // ---------------- 演示 4：改一条记录 = 读整页 + 写整页 ----------------
        banner("演示 4：把 value 从 300 改成 333，代价是多少？");
        uint64_t before = pager.total_io();
        Page     t;
        pager.read_page(1, t);              // 1 次读
        t.update(2, {1003, 333, "wang-wu-the-third"});
        pager.write_page(t);                // 1 次写
        pager.flush();
        uint64_t after = pager.total_io();
        cout << "\n  改了 4 个字节的数据 → 产生 " << (after - before)
             << " 次 I/O，搬运 " << (after - before) * PAGE_SIZE << " 字节\n";
        cout << "  放大倍数 = " << (double)((after - before) * PAGE_SIZE) / 4.0 << " : 1\n";
        cout << "  这就是为什么数据库怕'随机更新'，喜欢'攒一批顺序写'。\n";
        cout << "  也这就是为什么需要 WAL：把随机写变成日志上的顺序写（第 04 课讲）。\n";

        // ---------------- 演示 5：逐条落盘 vs 批量落盘 ----------------
        banner("演示 5：100 次修改，逐条落盘 vs 攒着批量落盘");
        uint64_t io0 = pager.total_io();
        for (int i = 0; i < 100; ++i) {     // 最笨的写法：每改一次就读写一遍
            Page tmp;
            pager.read_page(1, tmp);
            Record old;
            tmp.get(0, old);
            tmp.update(0, {old.id, old.value + 1, old.name});
            pager.write_page(tmp);
        }
        uint64_t dumb = pager.total_io() - io0;
        cout << "\n  写法 A（每次改都读写整页）： I/O = " << dumb
             << " 次，流量 " << dumb * PAGE_SIZE / 1024 << " KB\n";

        io0          = pager.total_io();
        pager.verbose = false;
        Page cached;                        // 写法 B：页留在内存里改，最后写一次
        pager.read_page(1, cached);
        for (int i = 0; i < 100; ++i) {
            Record old;
            cached.get(0, old);
            cached.update(0, {old.id, old.value + 1, old.name});
        }
        pager.verbose = true;
        pager.write_page(cached);
        cout << "  写法 B（内存里改 100 次，最后写一页）： I/O = "
             << (pager.total_io() - io0) << " 次\n";
        cout << "  → 差了 " << dumb << " 倍。Buffer Pool 的第一价值就在这：\n"
             << "    把'每次修改都落盘'变成'脏页攒着，淘汰或检查点时再落盘'。\n";
        pager.flush();
    }

    // ---------------- 演示 6：Buffer Pool ----------------
    banner("演示 6：4 帧 Buffer Pool —— 热数据在池里 vs 全表扫描把池冲干净");
    {
        // 场景 A：业务只反复访问 1~4 页（工作集装得下 4 帧）
        Pager      pager2(db_path);
        BufferPool pool(&pager2, 4);
        pager2.verbose = false;                    // 这一轮只看统计，别刷屏

        cout << "  场景 A：反复访问 1~4 页（工作集 = 4 页 = 池子大小）\n";
        for (int round = 0; round < 5; ++round)
            for (int32_t pid = 1; pid <= 4; ++pid) {
                Page& p = pool.fetch(pid);
                if (pid == 2 && round == 0) {      // 把 2 号页改脏，制造"脏页被淘汰必须写回"
                    p.insert({9001, 7, "dirty-here"});
                    pool.mark_dirty(p);
                    cout << "  （顺手改了 2 号页 → 它变成脏页，内存有、磁盘无）\n";
                }
                pool.unpin(p);
            }
        pool.report();
        cout << "  → 第一轮冷启动读 4 页，后面 16 次全命中。这就是 Buffer Pool 的理想状态。\n";

        // 场景 B：一条大表扫描，顺序路过 5~16 页，把热页全踢了
        pager2.verbose = true;
        cout << "\n  场景 B：同一个人插了一句全表扫描，顺序访问 5~16 页\n";
        for (int32_t pid = 5; pid <= 16; ++pid) {
            Page& p = pool.fetch(pid);
            pool.unpin(p);
        }
        cout << "\n  扫描完再回来访问热页 1~4：\n";
        for (int32_t pid = 1; pid <= 4; ++pid) {
            Page& p = pool.fetch(pid);
            pool.unpin(p);
        }
        pool.report();
        cout << "  → 看见没：一条大表扫描把四条热页全冲出去了，"
                "之后所有 OLTP 请求统统 miss。\n"
             << "    这就是著名的'缓存污染 / buffer pool 被冲干净'事故。"
             "真实数据库的解法：\n"
             << "    InnoDB 用 midpoint-insertion LRU（新页先插在 5/8 处，"
                "不是头），扫描流里的页还没热起来就被踢；"
                "老区页要待满 innodb_old_blocks_time（默认 1000ms）后再被访问才转正。\n"
             << "    另两个常被混着讲的参数解决的是别的麻烦："
                "innodb_buffer_pool_instances 把池子分片，摊薄 latch 竞争；"
                "MySQL 5.6+ 的 MRR（Multi-Range Read）先把二级索引攒下的主键排序，"
                "再用 handler::rnd_pos 批量顺序回表，把回表的随机 I/O 变成顺序 I/O。"
                "污染、锁竞争、随机回表是三笔账，别记成一笔。\n";
        cout << "\n  全程累计磁盘 I/O：" << pager2.total_io()
             << " 次（读 " << pager2.read_io << " + 写 " << pager2.write_io
             << "；写全是脏页被淘汰时被迫切回来的）\n";
        cout << "  把命中率 100% 想成理想、99% 想成优秀：OLTP 数据库 1% 的\n"
             << "  未命中就足以让 QPS 掉一个数量级，因为一次随机盘读约 100μs，\n"
             << "  而一次内存访问约 100ns，差着一千倍。\n"
             << "  （口径说明：这里的 100ns 是 DRAM 随机访问，含缓存行缺失；\n"
             << "    课本后面拿它和「页内一次比较 ≈ 1ns」比，那是 10 万倍。\n"
             << "    两个数都对，比的东西不同——记住「一次盘读 = 十万次比较」这个量级就行。）\n";
        pool.flush_all_dirty();
        pager2.flush();
    }

    // ---------------- 演示 7：指针 vs 偏移 ----------------
    banner("演示 7：为什么持久化格式里没有指针，只有偏移");
    {
        cout << "  sizeof(int*)   = " << sizeof(int*) << " 字节（64 位进程）\n";
        cout << "  sizeof(uint16_t) = " << sizeof(uint16_t) << " 字节（页内偏移够用）\n";
        cout << "  每个槽省 6 字节 × 一页 100 条记录 = 每页省 600 字节，\n"
             << "  100 万页就是 600MB。省内存是一方面，更关键的是：\n"
             << "  地址是进程私有的、重启就变的；偏移是页自己内部的相对位置，\n"
             << "  搬到任何机器、任何时刻都成立。断电即失嘛。\n";
        cout << "  数据库里所有'指向另一页'的关系，存的都是页号 + 槽号，\n"
             << "  相当于自己实现了一遍指针，只不过解引用要付一次 read_page。\n";
    }

    banner("收尾：删掉演示用的 .db 文件");
    // 想自己拿 hexdump 看这个文件，就把下面这行注释掉
    std::filesystem::remove(db_path);
    cout << "  已删除 " << db_path << "\n";

    // ================================================================
    cout << "\n================================================================\n"
         << "  老师傅的话\n"
         << "================================================================\n"
         << R"(
  徒弟，今天你只学会了一个词：页。但你得把这个词刻进骨头里。

  1) 你以前写 C++，脑子里的单位是"对象"。从今往后，凡是碰
     持久化的代码，脑子里的单位必须是"页"。你改一个字段，
     眼睛要能看见"一整页 4KB 被读上来又被写回去"。

  2) 别嫌 I/O 计数啰嗦。数据库 90% 的性能问题，最后都能翻译成
     一句话：多读了多少页。慢查询、索引、缓存、日志、检查点，
     全都在算"页"。你能算清页，就能算清数据库。

  3) 今天这个 Pager 故意没做并发。真加上线程，你马上要面对
     "读写同一页"的竞态 —— 那就是 latch（页闩锁）的地盘，
     第 3 关并发部分会专门讲。你现在只要知道：pin 计数就是
     引用计数的雏形，BufferPool::pick_victim 里跳过 pin>0 的帧，
     和你写 shared_ptr 时 use_count() > 0 不释放，是同一个道理。

  4) 最后再点一次题：磁盘按页读写 → 所以要 Buffer Pool、要 B+ 树、
     要把随机写改成顺序写；内存断电即失 → 所以要 WAL、要检查点、
     要恢复。下一课的手写 B+ 树，你会亲眼看到"因为一页只能放
     127 个 key，所以树高只有 4 层"这件事有多值钱。

  【课后动手】
   1. 把 PAGE_SIZE 改成 512（扇区级）和 65536，重跑演示 5，
      看"逐条落盘"的流量分别变成多少。你会长出"页大小是
      随机 I/O 延迟与内存浪费之间的折中"这个直觉。
   2. 给 Page 加一个 delete(slot)：把槽长度置 0 当墓碑，
      观察 free_space() 为什么不涨 —— 这就是"页碎片"，
      也是 VACUUM / OPTIMIZE TABLE 存在的理由。
   3. 把 BufferPool 的 LRU 换成 FIFO 或 CLOCK，比较命中率。
      下一份动画 bufferpool_animator.html 里就有 Clock，对照着看。
   4. 思考题：演示 4 里"读整页 + 改 + 写整页"，如果就在写完
      那一瞬间断电，数据会怎样？（提示：一半新一半旧，页烂了。
      这就是"部分写/撕裂页"，是 WAL 和双写缓冲要解决的东西，
      第 04 课亲手做。）
=== 本文件结束 ===
)";
    return 0;
}
