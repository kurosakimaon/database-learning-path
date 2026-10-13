# ACID 小抄 —— 贴在显示器边上那种

> 第 3 关通关文牒。别背，理解一遍然后忘了它，忘了再来看。

## 一、ACID 各是哪个部件干的活

| 字母 | 承诺 | 靠什么兑现 | 崩了会怎样 |
|---|---|---|---|
| **A** 原子性 | 要么全做，要么全没做 | **undo log（回滚日志）**：改之前先把旧值抄一份，回滚时倒放 | 转账扣了钱没加钱，钱凭空蒸发 |
| **C** 一致性 | 数据满足业务规则 | **约束（主键/外键/唯一/检查）+ 业务代码 + 触发器**，AID 都是为它打工的 | 余额为负、订单挂着不存在的用户 |
| **I** 隔离性 | 并发事务互不串味儿 | **锁（写写/读写串行化）+ MVCC（读写不互挡，各看各的快照）** | 脏读、不可重复读、丢失更新（03 实验全复现过） |
| **D** 持久性 | 提交了的雷打不动 | **redo log / WAL：先写日志再改数据（WAL 原则），日志 fsync 落盘才算提交** | 断电后"提交成功"的数据没了 |

老师傅的两句大白话：

- **A 和 D 是一对反义词**：A 用 undo log 往回倒，D 用 redo log 往前钉。一份改动的生前身后事，两本账。
- **C 是目标，AID 是手段**。数据库只能兜住约束能表达的那部分一致性（唯一、外键），"转账后两边余额相等"这种得靠你自己把两个 UPDATE 塞进同一个事务。

### WAL 链条（面试问到就顺着背）

```
改数据页之前 → 先把「改动描述」追加进 redo/WAL → fsync WAL → 才允许改内存页
                                                        ↓（后台慢慢刷）
                                              checkpoint 把脏页刷回数据文件
```

提交 = WAL 里一条 commit 记录 fsync 成功。**数据页本身还没落盘**，断电了照样能重放回来——这就是"每组提交只付一次顺序写"的便宜。SQLite 的 `PRAGMA journal_mode=WAL`、`synchronous=FULL/NORMAL` 的差别就在 fsync 的时机上：NORMAL 模式下 WAL 不每组 fsync，省电但断电可能丢最近几组提交（01 的 SQL 里实测过模式切换）。

## 二、隔离级别对照表（核心中的核心）

| 隔离级别 | 脏读 | 不可重复读 | 丢失更新* | 典型实现 |
|---|---|---|---|---|
| READ UNCOMMITTED | ✘ 会 | ✘ 会 | ✘ 会 | 几乎没人用；SQLite shared-cache + `read_uncommitted=1`（03 实验 A 就是这么造出来的） |
| READ COMMITTED | ✔ 挡住 | ✘ 会 | ✘ 会 | 语句级快照。**PostgreSQL、Oracle、SQL Server 默认** |
| REPEATABLE READ | ✔ | ✔ 挡住 | 视实现**未必** | 事务级快照。InnoDB 额外上**间隙锁**管幻读，**MySQL 默认** |
| SERIALIZABLE | ✔ | ✔ | ✔ | 读也加锁（或冲突即 abort），吞吐最惨 |

\* 标准只列了脏读/不可重复读/幻读三异常，但**丢失更新才是实战中最疼的**：两个"读—改—写"各自基于旧值写回，后写的把前者的更新抹掉。注意：**RR 级别的快照读并不能防丢失更新**——InnoDB 的快照 UPDATE 会读到旧值再写回旧值+delta，照样丢（03 实验 C.1 实测）。防它只有三条路：

1. **单条 SQL 原子更新**：`UPDATE t SET v = v + 1`（数据库内部拿行锁做原子加，03 修法①，300 个并发一次不丢）；
2. **悲观锁**：`SELECT ... FOR UPDATE` 先锁再改（03 修法②）；
3. **乐观锁**：版本号 CAS，`UPDATE ... WHERE id=1 AND version=?`，失败重试（03 修法③）。

### 快照的时机，一句话分清 RC 和 RR

- **RC**：每条语句开始时重新拍快照 → 别的事务提交在两条语句之间"插进来"，第二次读就不一样了（不可重复读）。
- **RR**：事务第一条读语句拍一次快照，全程用它 → 外面翻江倒海，我毛都不看见。
- 类比：**RC 是每条语句刷新一次浏览器，RR 是进事务时截了张图看到底。**

### 各家默认值（记住够用）

- MySQL/InnoDB：**REPEATABLE READ** + 间隙锁（gap lock）防幻读，别被"标准说 RR 防不了幻读"绕晕——InnoDB 自己加码了。
- PostgreSQL / SQL Server / Oracle：**READ COMMITTED**（PG 实际是快照隔离 SI，比标准 RC 略强，但会有写 skew）。
- SQLite：默认就是"整个库一个快照"级别的串行化；shared-cache 模式下才有表级读写锁冲突（`SQLITE_LOCKED`/`SQLITE_BUSY`，03/02 都撞过）。

## 三、死锁排查套路（线上保命用）

**定性**：`ERROR 1213 (40001): Deadlock found` 是 InnoDB 检测后主动回滚了受害者；`ERROR 1205 Lock wait timeout` 是没检出环、干等超时的（04 演示 2 vs 演示 3 的区别）。

**现场抓取**：

```sql
-- MySQL 8：锁与等待一眼看全
SELECT * FROM performance_schema.data_locks;
SELECT * FROM performance_schema.data_lock_waits;
SHOW ENGINE INNODB STATUS\G   -- LATEST DETECTED DEADLOCK 段，事后也能看最近一次
-- PostgreSQL：
SELECT pid, wait_event_type, wait_event, state, query
FROM pg_stat_activity WHERE wait_event_type = 'Lock';
SELECT * FROM pg_locks WHERE NOT granted;
```

**排查三步**（04 演示 5 就是这个样子）：

1. 找**持锁最久**的事务（长事务、忘了 COMMIT 的交互式会话是头号嫌疑）；
2. 顺着等待链找**环**：A 等 B 的行、B 等 A 的行 → 死锁；
3. 看两条 SQL 的**执行计划**——同一个更新走不同索引，行序不同，加锁顺序就不同，这是"代码没改突然开始死锁"的经典答案（新索引上线！）。

**预防清单**（按性价比排序）：

- 所有路径按**同一全序**取锁：批量更新先按主键排序（04 演示 4：随机序死锁率 11.6%，排序后 0%）；
- 事务**尽量短**：别让 COMMIT 前面隔着一次 HTTP 调用；
- 热点行拆散（分片计数）或排队化（单线程队列吞更新）；
- 用 `SELECT ... FOR UPDATE SKIP LOCKED` 做任务队列，天然绕开排队互等；
- C++ 本地同款：多把 mutex 一律 `std::scoped_lock(m1, m2)`，它内部统一加锁顺序，反序手写 `lock(m1); lock(m2)` 必死。

**受害者怎么办**：收到 1213 后**整个事务从头重试**（不是补第二条语句！04 演示 3 实测），重试次数封顶 + 指数退避，别写死循环。

## 四、和 C++ 的对照表（你这关真正的收获）

| 数据库概念 | C++ 熟脸 |
|---|---|
| 行锁 | 按 key 分片的 `std::mutex`（哈希表分桶锁） |
| S / X 锁 | `std::shared_mutex` 的 `lock_shared()` / `lock()` |
| 锁超时 | `try_lock_for()` |
| 死锁 | 两个线程反序 `lock()` 同一对 mutex |
| 等待图找环 | 资源分配图 DFS 找环（操作系统课本那张） |
| 受害者回滚重试 | 释放全部锁后整段临界区重跑 |
| MVCC 版本链 | 写时复制 + 快照迭代器（`shared_ptr<Version>` 链） |
| RC vs RR | 快照建立的时机：每条语句一次 vs 整个事务一次 |

—— 写完收工。背不熟没关系，03、04 两个程序你亲手跑出了每一个异常，手指头记得比脑子牢。
