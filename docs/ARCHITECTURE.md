# 架构

## 目标

Windows x64 单用户桌面待办应用：纯 Win32 + Direct2D 自绘、无边框 Acrylic 窗口、本地 SQLite 单文件
持久化、完整的窗口过渡动画。运行时不需要任何数据库服务或网络。

## 分层

```
src/
  domain/   模型、校验、排序权重、分页、时间范围、正文文档（不依赖 Win32 / D2D / SQL）
  core/     任务、分类、历史、统计服务；只依赖 domain 与仓储接口
  data/     SQLite 连接、schema/迁移、各仓储与 UnitOfWork；SQL 只在这一层
  ui/       窗口、自绘控件、Direct2D 渲染、主题、图标与过渡动画
  app/      装配、AppController、DbWorker、托盘、日志与 UI 派发
  config/   AppConfig / ConfigStore / AppPaths
```

依赖方向：`ui -> app -> core -> domain`，`data` 实现 `core` 的仓储接口，`domain` 不反向依赖任何层。

## 启动与装配

1. `main` 创建 `Application`，解析 `AppPaths`（配置、数据库、日志）。
2. `ConfigStore` 读取 `%APPDATA%\TodoListWin\config.ini`，损坏值回退到默认。
3. `SqliteUnitOfWorkFactory` 指向 `%LOCALAPPDATA%\TodoListWin\data\todolist.db`，启动时执行幂等迁移。
4. `DbWorker` 在专用线程执行数据库操作，完成后通过 `PostMessage(WM_APP_DB_COMPLETION)` 回到 UI 线程。
5. `MainWindow` 只通过 `AppController` 读写状态；后台完成事件由控制器批量消费。

## 数据库

- SQLite 3.45.3 amalgamation，WAL 模式，`foreign_keys=ON`，`busy_timeout=5000`。
- `SqliteSchema` 用 `user_version` / `schema_migration` 双版本闸门，迁移失败整体回滚。
- 一个 `UnitOfWork` 对应一个连接与一个事务；完成任务时状态更新与历史事件在同一事务提交。
- 图片正文以 BLOB 保存，单图与单任务都有字节上限；读取缺图返回占位状态，不跨源查找。

## 窗口与渲染

- `CreateSwapChainForComposition` + `DXGI_ALPHA_MODE_PREMULTIPLIED` + `WS_EX_NOREDIRECTIONBITMAP`
  实现真透背景，Acrylic 由 DWM 提供。
- `WM_NCCALCSIZE` 让客户区精确等于窗口矩形；最大化且动画结束后才钉到工作区。
- 交换链尺寸在创建、显示、动画结束、`WM_SIZE` 非动画分支和每帧渲染兜底处与客户区同步，
  避免出现未绘制的边缘。
- `RenderEngine` 通过 `IDCompositionEffectGroup` 设置整体透明度，通过 `SetVisualStretch` 对内容做
  非均匀缩放，使内容在窗口矩形动画时铺满客户区。

## 窗口过渡

`MainWindow` 用一个 `Transition` 枚举和一条时间线覆盖全部场景：

| 过渡 | 时长 | 起点 → 终点 |
| --- | --- | --- |
| Appear（首次显示 / 托盘 / 任务栏恢复） | 0.36 s | 最终矩形 5% → 最终矩形，透明度 0 → 1 |
| Close（关闭到托盘 / 退出） | 0.26 s | 当前矩形 → 当前矩形 5%，透明度 1 → 0 |
| Minimize | 0.26 s | 当前矩形 → 当前矩形 5%，透明度 1 → 0 |
| Maximize | 0.22 s | 当前矩形 → 工作区，内容拉伸 |
| Restore | 0.24 s | 最大矩形 → 恢复矩形，内容拉伸 |

`BeginTransition` 记录起止矩形、时长和最终客户区尺寸；`AdvanceWindowRectAnimation` 每帧用
`1-(1-t)^3` 更新窗口矩形、内容缩放与透明度，结束后 `PostMessage(WM_APP+32)` 在消息栈外收尾。
最小化完成后保留 tiny 矩形交给系统，任务栏恢复时由 `WM_SIZE(SIZE_RESTORED)` 启动 grow-back。

## 构建

- `Directory.Build.props` 探测 v145 / v143 / v142 工具集。
- `build\SqliteDependencies.props` 集中 SQLite 头文件与库路径。
- `build.bat` 的所有目标都同时构建 `Debug|x64` 与 `Release|x64`；产物为
  `bin\<Configuration>\todolist4win.exe` 与 `bin\<Configuration>\todolist4win_tests.exe`。
