# 数据库修炼路 —— 写给 C++ 工程师的数据库七关

> 老师傅带徒弟的路子：每关一份教程（`tutorial.md`）+ 能亲手编译运行的 C++ 示例 + 打开即看的交互动画。
> 不讲空话，每一层都落到"磁盘上那几个字节到底怎么摆"。

**马上开始**：用浏览器打开根目录的 [`数据库学习路线图.html`](数据库学习路线图.html)，那是全程地图兼总目录。

## 这门课适合谁

- 会 C++（类、模板、智能指针、多线程 basics），想搞懂数据库内部是怎么回事
- 会写 SQL 但只停在"能查"，想知道 B+ 树、WAL、MVCC、查询计划器是怎么回事
- 想动手搓一个玩具数据库练手的

不需要任何数据库内部知识，从第一关开始铺。

## 七关总览

| 关卡 | 目录 | 主题 | 你会得到 |
|------|------|------|----------|
| 第 1 关 · 筑基期 | [`fundamentals/`](fundamentals/) | 关系模型与 SQL | 表/关系代数、JOIN 可视化、SQL 练习沙盒 |
| 第 2 关 · 炼气期 | [`storage_index/`](storage_index/) | 存储与索引 | 页与磁盘 I/O、B+ 树交互动画、缓冲池动画、索引实测 |
| 心法期 | [`transactions/`](transactions/) | 事务与 ACID | 隔离级别异常实验台、MVCC 动画、锁与死锁、ACID 速查表 |
| 第 4 关 · 兵器期 | [`cpp_integration/`](cpp_integration/) | C++ 接入数据库 | SQLite C API、RAII 封装、线程池+连接池、MySQL/ORM |
| 第 5 关 · 结丹期 | [`query_internals/`](query_internals/) | 查询内幕 | JOIN 三算法实测、代价模型、EXPLAIN 流水线动画、WAL 崩溃恢复 |
| 第 6 关 · 轻剑期 | [`nosql_distributed/`](nosql_distributed/) | NoSQL 与分布式 | 数据模型对比、LSM vs B+ 树动画、CAP 动画、并发原语 |
| 第 7 关 · 手搓期 | [`minidb/`](minidb/) | 手搓 miniDB | 六个阶段从零造一个迷你数据库：Pager → 堆表 → B+ 树 → WAL → SQL 前端 → 压测 |

每关目录内的 `tutorial.md` 是主课本，`index.html` 是该关的导航页（浏览器直接打开即可，无需服务器）。

## 环境要求

- **编译器**：任意支持 C++17 的编译器（g++ / clang++ / MSVC）。示例中给出的是 g++ 命令：

  ```powershell
  g++ -std=c++17 -O2 -Wall -Wextra 01_disk_and_page.cpp -o demo.exe
  ```

- **部分示例需要 SQLite3 开发库**（文件名里带 sqlite 的大部分如此），链接时加 `-lsqlite3`：

  ```powershell
  g++ -std=c++17 -O2 cpp_integration\01_sqlite_c_api.cpp -o demo.exe -lsqlite3
  ```

  Windows 下可用 [MSYS2](https://www.msys2.org/) 的 `pacman -S mingw-w64-ucrt-x86_64-sqlite` 或从 [sqlite.org](https://www.sqlite.org/download.html) 下载合并版。

- **交互动画与导航页**：纯离线单文件 HTML，双击即开，不联网、无外部依赖。
- **SQL 练习**：`.sql` 文件用任意 SQLite 客户端执行（`sqlite3 xxx.sql`）即可。

## 建议路线

1. 打开总路线图，扫一眼全程地图
2. 按关卡顺序：读 `tutorial.md` → 编译运行当关 `.cpp` 示例 → 打开动画看内部过程 → 做 `.sql` 练习
3. 最后一关 `minidb/` 六个 phase 顺序推进，跑完压测对比第 2 关的数字，全程闭环

## 仓库结构

```
database-learning-path/
├── 数据库学习路线图.html   # 总地图（入口）
├── fundamentals/           # 第 1 关 筑基期 · 关系模型与 SQL
├── storage_index/          # 第 2 关 炼气期 · 存储与索引
├── transactions/           # 心法期 · 事务与 ACID
├── cpp_integration/        # 第 4 关 兵器期 · C++ 接入数据库
├── query_internals/        # 第 5 关 结丹期 · 查询内幕
├── nosql_distributed/      # 第 6 关 轻剑期 · NoSQL 与分布式
└── minidb/                 # 第 7 关 手搓期 · 迷你数据库
```

统计：**22 个可编译 C++ 示例 · 18 个离线交互 HTML · 7 份教程 · 5 份 SQL 练习**。
