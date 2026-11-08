# 文档导航 / Documentation index

[中文首页](../README.md) · [English README](../README.en.md) · [中文架构](ARCHITECTURE.md) · [Architecture in English](ARCHITECTURE.en.md)

中英文首页与架构文档对应介绍当前系统；下列专题保留各自的实现、限制和验证证据。部分早期专题反映当时阶段，阅读时结合当前架构、最新专题及源码判断，不把历史“待完成”列表或测试数量视为当前结论。

The READMEs and architecture guides provide matching Chinese and English overviews. Topic documents retain implementation details, limitations and validation evidence. Older documents may describe earlier stages; consult the current architecture, newer topic documents and source before treating historical pending-work lists or test counts as current status.

## 推荐阅读 / Suggested paths

| 目标 / Goal | 阅读顺序 / Reading order |
| --- | --- |
| 上手 / Getting started | [中文 README](../README.md) / [English README](../README.en.md) → [测试 / Tests](../tests/README.md) |
| 理解架构 / Understand the system | [中文架构](ARCHITECTURE.md) / [English architecture](ARCHITECTURE.en.md) → [存储 / Storage formats](STORAGE_FORMAT.md) → [原生事务 / Page transactions](PAGE_TRANSACTIONS.md) |
| 选择后端 / Choose an engine | [oneTBB](TBB_STORAGE.md) · [RocksDB](ROCKSDB_STORAGE.md) · [WiredTiger](WIREDTIGER_STORAGE.md) |
| 类型存储 / Typed storage | [String](STRING_PAGES.md) · [List](LIST_STORAGE.md) · [Hash](MAP_STORAGE.md) · [Set](SET_STORAGE.md) · [ZSet](ZSET_STORAGE.md) |
| 一致性 / Consistency | [批量 / Batches](ATOMIC_BATCH.md) → [事务 / Transactions](TRANSACTIONS.md) → [源端 EXEC / Source EXEC](SOURCE_EXEC.md) → [MOVE](MOVE.md) |
| 复制与恢复 / Replication and recovery | [基础 / Base stream](REPLICATION.md) → [页 / Pages](PAGE_REPLICATION.md) → [EXEC](EXEC_REPLICATION.md) → [有序 / Ordered stream](ORDERED_REPLICATION.md) → [MOVE startup](MOVE_STARTUP.md) |
| 运维与权限 / Operations and permissions | [认证 / Authentication](AUTH.md) · [ACL](ACL_PERMISSIONS.md) · [配置 / Configuration](CONFIGURATION.md) · [INFO](INFO.md) · [CLIENT](CLIENT_INFO.md) |
| 扩展命令 / Additional commands | [Bitmap](BITMAPS.md) · [BITFIELD](BITFIELD.md) · [HyperLogLog](HYPERLOGLOG.md) · [GEO](GEO.md) · [Blocking Lists](BLOCKING.md) · [Blocking ZSets](BLOCKING_ZSET.md) · [Pub/Sub](PUBSUB.md) |
| 兼容与计划 / Compatibility and plans | [兼容性 / Compatibility](COMPATIBILITY.md) · [阶段进展 / Roadmap](ROADMAP_STATUS.md) |

## 全部专题 / All topics

保留原专题文件名，方便搜索代码引用和历史记录。下表涵盖当前顶层专题；原始测量与验证报告位于 `benchmarks/`，由对应专题链接。

Existing topic names are preserved for source references and history. The table covers current top-level topics; raw measurement and validation reports live in `benchmarks/` and are linked from their topic documents.

| 专题 / Topic | 内容 / Original document title |
| --- | --- |
| [ACL_AUDIT](ACL_AUDIT.md) | ACL security events |
| [ACL_PERMISSIONS](ACL_PERMISSIONS.md) | ACL users and runtime permissions |
| [ACL_PERSISTENCE](ACL_PERSISTENCE.md) | ACL file persistence |
| [ACTIVE_EXPIRY](ACTIVE_EXPIRY.md) | Active key expiration |
| [ATOMIC_BATCH](ATOMIC_BATCH.md) | Atomic multi-key operations |
| [ATOMIC_FAMILY_PLANS](ATOMIC_FAMILY_PLANS.md) | Atomic memory page-family plans |
| [AUTH](AUTH.md) | Default-user authentication and protected RPC streams |
| [AUTH_PERFORMANCE](AUTH_PERFORMANCE.md) | Authentication dispatch: local GET comparison |
| [BITFIELD](BITFIELD.md) | Native BITFIELD and BITFIELD_RO |
| [BITMAPS](BITMAPS.md) | Native String bitmap commands |
| [BITOP](BITOP.md) | Native String BITOP |
| [BLOCKING](BLOCKING.md) | Blocking lists |
| [BLOCKING_ZSET](BLOCKING_ZSET.md) | Blocking sorted sets |
| [CALLBACK_SHUTDOWN](CALLBACK_SHUTDOWN.md) | ACL callback shutdown |
| [CLIENT_INFO](CLIENT_INFO.md) | CLIENT INFO and LIST |
| [CLIENT_INFO_OOM](CLIENT_INFO_OOM.md) | Client metadata allocation-failure investigation |
| [CLIENT_KILL](CLIENT_KILL.md) | CLIENT KILL |
| [CLIENT_REGISTRY](CLIENT_REGISTRY.md) | Connection registry foundation |
| [CLIENT_SNAPSHOTS](CLIENT_SNAPSHOTS.md) | Linear KILL capture and safe client metadata |
| [CLIENT_STATE](CLIENT_STATE.md) | Connection metadata and RESET |
| [COMMAND_DISPATCH](COMMAND_DISPATCH.md) | Command dispatch |
| [COMMAND_METADATA](COMMAND_METADATA.md) | Command discovery and key arguments |
| [COMPATIBILITY](COMPATIBILITY.md) | Redis compatibility work |
| [CONFIGURATION](CONFIGURATION.md) | Runtime configuration |
| [CROSS_DB_REPLAY](CROSS_DB_REPLAY.md) | Atomic replay of memory database groups |
| [DBSIZE](DBSIZE.md) | Stored root counts |
| [DENSE_KEY_INDEX](DENSE_KEY_INDEX.md) | Dense memory key indexes |
| [DURABILITY_BARRIERS](DURABILITY_BARRIERS.md) | Physical WAL durability barriers |
| [EXEC_REPLICATION](EXEC_REPLICATION.md) | PKV3 whole-EXEC replication |
| [EXPIRY_PERFORMANCE](EXPIRY_PERFORMANCE.md) | Local expiry-index performance |
| [FIBER_STARTUP](FIBER_STARTUP.md) | macOS fiber event initialization |
| [FLOAT_INCREMENTS](FLOAT_INCREMENTS.md) | Floating-point increments |
| [GEO](GEO.md) | GEO commands and bounded storage |
| [GEO_RADIUS](GEO_RADIUS.md) | Legacy GEO radius commands |
| [GEO_SEARCH](GEO_SEARCH.md) | Indexed GEO search |
| [HASH_SAMPLING](HASH_SAMPLING.md) | Native Hash random-field sampling |
| [HGETDEL](HGETDEL.md) | HGETDEL: atomic Hash field retrieval and deletion |
| [HYPERLOGLOG](HYPERLOGLOG.md) | HyperLogLog |
| [INFO](INFO.md) | INFO and statistics |
| [INLINE_STRING_READS](INLINE_STRING_READS.md) | Inline String reads |
| [KEYSPACE](KEYSPACE.md) | Full keyspace matching |
| [KEY_SAMPLING](KEY_SAMPLING.md) | RANDOMKEY and TIME |
| [KEY_TRANSFERS](KEY_TRANSFERS.md) | Key transfers |
| [LCS](LCS.md) | Longest common subsequence |
| [LEGACY_EXPIRY](LEGACY_EXPIRY.md) | Legacy expiry migration |
| [LEGACY_TRANSFERS](LEGACY_TRANSFERS.md) | Bounded legacy COPY and RENAME |
| [LIST_PAGES](LIST_PAGES.md) | Bounded List storage primitives |
| [LIST_STORAGE](LIST_STORAGE.md) | Packed List operations over native pages |
| [LOGICAL_DATABASES](LOGICAL_DATABASES.md) | Logical databases and COPY |
| [MAP_STORAGE](MAP_STORAGE.md) | Bounded field/member storage |
| [MEMORY_MOVE_FAMILIES](MEMORY_MOVE_FAMILIES.md) | Memory MOVE page-family ownership |
| [MOVE](MOVE.md) | Public MOVE |
| [MOVE_INTENT_INTEGRATION](MOVE_INTENT_INTEGRATION.md) | MOVE and ordered replication integration plan |
| [MOVE_RECOVERY](MOVE_RECOVERY.md) | MOVE decision journal |
| [MOVE_SOURCE_CAPTURE](MOVE_SOURCE_CAPTURE.md) | Prepared MOVE observation and source capture |
| [MOVE_STARTUP](MOVE_STARTUP.md) | Native MOVE startup recovery |
| [MOVE_STORAGE](MOVE_STORAGE.md) | Retained native MOVE storage coordination |
| [MSETEX](MSETEX.md) | Atomic multi-key String assignment with expiry |
| [NATIVE_COMMIT_BUFFERS](NATIVE_COMMIT_BUFFERS.md) | Allocate native commit buffers on demand |
| [NATIVE_MOVE_REDO](NATIVE_MOVE_REDO.md) | Bounded native MOVE redo |
| [NATIVE_PATHS](NATIVE_PATHS.md) | Exact native storage roots |
| [ORDERED_REPLICATION](ORDERED_REPLICATION.md) | Ordered replication protocol 4 |
| [OWNERSHIP_REPLAY_VIEW](OWNERSHIP_REPLAY_VIEW.md) | Ordered ownership replay view |
| [PAGE_REPLICATION](PAGE_REPLICATION.md) | Bounded page-transaction stream components |
| [PAGE_SEQUENCES](PAGE_SEQUENCES.md) | Ordered page replay in one database |
| [PAGE_TRANSACTIONS](PAGE_TRANSACTIONS.md) | Atomic roots and bounded physical pages |
| [PAGE_WORKSPACES](PAGE_WORKSPACES.md) | Native page workspace for source transactions |
| [PREPARED_PAGE_COMMITS](PREPARED_PAGE_COMMITS.md) | Prepared page commits and cross-database coordination |
| [PROCESS_SHUTDOWN](PROCESS_SHUTDOWN.md) | Process shutdown and exit regression |
| [PUBSUB](PUBSUB.md) | Pub/Sub commands and transport |
| [PUBSUB_OUTPUT](PUBSUB_OUTPUT.md) | Pub/Sub output transport foundation |
| [QUEUE_SCALING](QUEUE_SCALING.md) | Waiting queue scaling and allocation ownership |
| [READ_GUARD_FAST_PATH](READ_GUARD_FAST_PATH.md) | Disabled replica-read guard fast path |
| [REPLICATION](REPLICATION.md) | Versioned live replication stream |
| [REPLICA_READONLY](REPLICA_READONLY.md) | Replica client-write admission |
| [REPLICA_READ_RETENTION](REPLICA_READ_RETENTION.md) | Replica client reads retain authoritative data |
| [RESP3](RESP3.md) | HELLO and typed RESP3 replies |
| [RESP3_PERFORMANCE](RESP3_PERFORMANCE.md) | RESP3 change: local hot-key GET comparison |
| [ROADMAP_STATUS](ROADMAP_STATUS.md) | PKV 阶段目标与剩余工作 |
| [ROCKSDB_STORAGE](ROCKSDB_STORAGE.md) | RocksDB native storage |
| [SCAN](SCAN.md) | Cursor scans |
| [SET_COMPARISON](SET_COMPARISON.md) | Conditional String updates |
| [SET_STORAGE](SET_STORAGE.md) | Native Set storage |
| [SORTED_SETS](SORTED_SETS.md) | Sorted Set commands and ranges |
| [SOURCE_EXEC](SOURCE_EXEC.md) | Source EXEC commit integration |
| [SOURCE_EXEC_BENCHMARK](SOURCE_EXEC_BENCHMARK.md) | Local source EXEC benchmark |
| [STARTUP_CANCELLATION](STARTUP_CANCELLATION.md) | Startup cancellation and serving handoff |
| [STORAGE_FORMAT](STORAGE_FORMAT.md) | Logical records and storage codecs |
| [STRING_DIGEST](STRING_DIGEST.md) | String digests and conditional deletion |
| [STRING_PAGES](STRING_PAGES.md) | Native String storage |
| [STRING_STORAGE_BENCHMARK](STRING_STORAGE_BENCHMARK.md) | Native String storage microbenchmark |
| [TBB_STORAGE](TBB_STORAGE.md) | oneTBB memory storage |
| [TRANSACTIONS](TRANSACTIONS.md) | MULTI, EXEC, DISCARD, WATCH and UNWATCH |
| [WIREDTIGER_STORAGE](WIREDTIGER_STORAGE.md) | WiredTiger native storage |
| [ZSET_STORAGE](ZSET_STORAGE.md) | Native ZSet storage component |
