# 心法期 · 事务与 ACID

老师傅带你把「事务」这两个字从嘴里说到手里

## 一、模块简介

徒弟啊，前两关你练的是招式：内存怎么摆、容器怎么用、
指针怎么指。这一关开始练心法。

事务这东西，接口就四个词——BEGIN、COMMIT、ROLLBACK、SAVEPOINT，
一个下午就能背完。可工地上一半的资损事故，都是这四个词
没使对：钱扣了没加上、两个请求互相抹掉对方的更新、
死锁把整个服务卡成幻灯片。

这一关不让你背八股。四个程序全是真刀真枪：
SQLite 里真的会脏读，两个线程真的会互相丢更新，
锁管理器真的会死锁给你看，还是你亲手写出来的死锁。
心法这东西，眼睛看懂了不算数，手指头记住才算。

## 二、学习目标

学完本模块，你要能做到：

1. 把 ACID 四个字母翻译成具体机制：谁靠 undo log、谁靠 WAL、
   谁靠锁和 MVCC——而不是四句顺口溜
2. 用 RAII 把事务生命周期焊死，异常路径自动回滚，
   并且知道 SQLITE_BUSY 不是错误、是「待会儿再来」
3. 亲口复现脏读、不可重复读、丢失更新，
   并说出三种修法（原子 SQL / FOR UPDATE / 版本 CAS）各自的场景
4. 徒手写出带等待队列的 S/X 锁管理器和等待图死锁检测，
   从此看 innodb 锁文档像看自己家户口本
5. 线上遇到锁等待/死锁报警，知道先查什么、后查什么

## 三、文件列表与学习重点

一共 8 个文件，按顺序来，程序必须自己跑一遍：

- **01_acid_in_sqlite.sql** ← 用 SQL 亲手验证 ACID
  - 重点：BEGIN/COMMIT/ROLLBACK、SAVEPOINT 部分回滚、
    WAL 模式、两个会话互相锁门（文件里注释记了实测过程）
  - 跑法：`sqlite3 :memory: ".read 01_acid_in_sqlite.sql"`

- **02_transaction_raii.cpp** ← 把事务装进析构函数
  - 重点：RAII 事务类（析构自动 COMMIT/ROLLBACK）、
    SQLITE_BUSY 指数退避重试、两个线程写同一行
  - 跑法：`g++ -std=c++17 -O2 02_transaction_raii.cpp -o 02.exe -lsqlite3`

- **03_isolation_anomaly_lab.cpp** ← 异常复现车间（本关核心）
  - 重点：多线程独立连接复现脏读/不可重复读/丢失更新/写写冲突，
    三种修法实测对比（原子 UPDATE、悲观锁、乐观锁 CAS）
  - 跑法：`g++ -std=c++17 -O2 03_isolation_anomaly_lab.cpp -o 03.exe -lsqlite3`
  - 注意：输出里的毫秒时间线是灵魂，对照着读

- **04_lock_and_deadlock.cpp** ← 手搓锁管理器（本关珠峰）
  - 重点：S/X 兼容矩阵、FIFO 等待队列、锁升级、
    等待图 DFS 找环、受害者回滚+整事务重试、
    随机加锁 vs 排序加锁的死锁率对比
  - 跑法：`g++ -std=c++17 -O2 04_lock_and_deadlock.cpp -o 04.exe`
  - 不用 sqlite，纯 C++ 标准库

- **acid_cheatsheet.md** ← 小抄，考前和出事的时候看
  - 重点：ACID↔机制对照、隔离级别表（含各家默认值）、
    死锁排查三板斧

- **mvcc_animator.html** ← 版本链动画
  双击打开就能玩。RC/RR 一键切换，看 read view 怎么决定
  你「看得见」哪个版本。

- **isolation_playground.html** ← 双事务游乐场
  三个预设场景（转账/库存/报表）× 四种隔离级别，
  异常发生的瞬间给你亮红牌。先玩 RU 和 RC，再切 RR 对比。

- **index.html** ← 本关总览页（浏览器打开）

## 四、命令速查

编译运行（SQLite 系列要挂 -lsqlite3）：

```powershell
g++ -std=c++17 -O2 02_transaction_raii.cpp -o t02.exe -lsqlite3
.\t02.exe
```

SQL 验证：

```powershell
sqlite3 :memory: ".read 01_acid_in_sqlite.sql"
```

跑完记得清场：删 *.exe、*.db、*.db-wal、*.db-shm。

## 五、老师傅的叮嘱

1. 03 这个程序第一次跑，脏读怎么都复现不出来，
   我卡了一晚上——原因是读写两条连接没共用页缓存，
   还多挂了 FULLMUTEX。代码里把这些坑都标出来了，
   你踩到同样的坑时，注释就是你的地图。

2. 04 的锁管理器我前前后后修了六个 bug：S-S 判成冲突、
   授权记错资源、僵尸等待挡队头、受害者摘错人……
   每一个都写在注释里。并发程序的 bug 从不崩溃，
   它们只是安静地变慢或变错——所以每个演示才要打印耗时。

3. 别跳过两个 HTML。动画里「快照建立的时机」那一下，
   看懂了，RC 和 RR 这辈子不用背。

4. 每关末尾的练习不是摆设。04 的 wait-die 练习做出来，
   你对死锁的理解就超过一半自称熟练的 C++ 选手了。
