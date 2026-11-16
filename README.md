# TodoList for Windows

纯 C++20 + Win32 + Direct2D 1.1 全自绘的 Windows 桌面待办应用。无边框窗口、Acrylic 真透桌面、
橙白配色，数据只保存在本机一个 SQLite 文件里，不依赖任何数据库服务。

## 特性

- **原生自绘界面**：不使用 Qt / MFC / wxWidgets，窗口与全部控件由 Direct2D 1.1 + DirectWrite 绘制；
  Per-Monitor V2 DPI 感知。
- **真实半透明**：`CreateSwapChainForComposition` + `DXGI_ALPHA_MODE_PREMULTIPLIED` +
  `WS_EX_NOREDIRECTIONBITMAP` + DWMWA Acrylic，窗口背景真的透出桌面。
- **窗口动画**：首次显示、关闭、最小化、最大化、还原和从托盘 / 任务栏恢复共用一条过渡链路，
  窗口矩形按 ease-out 曲线插值，内容同步缩放并淡入淡出；收起时缩到最终尺寸的 5% 后再消失。
- **单数据源**：`%LOCALAPPDATA%\TodoListWin\data\todolist.db` 一个 SQLite 文件，启动时自动建库建表，
  不联网、不上传、不依赖本机数据库服务。
- **任务闭环**：回车新建、双击编辑、复选框完成/取消、悬停删除、拖拽排序、优先级、分类、截止日期
  （弹出式日历，不手输文本）。
- **系统视图与筛选**：全部任务 / 今日待办 / 逾期任务，配合关键词搜索、状态、优先级、完成留底策略。
- **历史与统计**：完成事件与任务状态在同一事务内写入历史；历史页支持时段、分类、关键词过滤、
  恢复、永久删除；统计页为自绘环形完成率、分类柱状图与近 7 天趋势。
- **桌面集成**：拖拽移动、边缘缩放、置顶、关闭到托盘、托盘唤起、亮/暗主题跟随系统。

## 构建

### 依赖

| 依赖 | 说明 |
| --- | --- |
| Windows 10 1903+ / Windows 11，x64 | 仅支持 x64，工程不含 Win32 配置 |
| Visual Studio 2022 或更高 | 需勾选「使用 C++ 的桌面开发」工作负载；工具集自动探测 v145 → v143 → v142 |
| Windows 10 SDK | `Directory.Build.props` 固定 `WindowsTargetPlatformVersion=10.0` |

SQLite 3.45.3 以官方 amalgamation 内置于 `third_party/sqlite/`，无需额外安装。

### 命令

```bat
build.bat               :: 同时构建 Debug + Release，并运行两套测试
build.bat app           :: 只构建应用，Debug + Release 各一份
build.bat test          :: 构建并运行测试，Debug + Release 各一份
build.bat all           :: 构建应用与测试，Debug + Release 各一份，然后运行测试
build.bat start         :: 运行已构建的 Debug 版本
build.bat start Release :: 运行已构建的 Release 版本
build.bat clean         :: 清理 bin\ 与 obj\
```

构建应用（`build.bat app` / `all`）产出：

```
bin\Debug\todolist4win.exe        bin\Release\todolist4win.exe
```

构建测试（`build.bat test` / `all`）产出：

```
bin\Debug\todolist4win_tests.exe  bin\Release\todolist4win_tests.exe
```

也可以直接用 Visual Studio 打开 `todolist4win.sln`（配置为 `Debug|x64` 与 `Release|x64`）。

## 数据存放

| 内容 | 路径 |
| --- | --- |
| 配置 | `%APPDATA%\TodoListWin\config.ini`（UTF-16LE，含 BOM） |
| 数据库 | `%LOCALAPPDATA%\TodoListWin\data\todolist.db` |
| 日志 | `%LOCALAPPDATA%\TodoListWin\Logs\todolist.log` |

应用名可随时在「设置」里修改，无需重启；配置文件与数据库都在本机，换机器需要自行复制。

## 目录结构

```
src/
  domain/      领域模型与业务规则（不依赖 Windows / Direct2D / SQL）
  core/        服务层，只依赖仓储接口
  data/        SQLite 仓储、schema 与迁移，SQL 只出现在这一层
  ui/          自绘控件、窗口、动画、渲染与主题
  app/         装配、控制器、后台 worker 与完成队列
  config/      配置读写与路径解析
build/         SQLite 依赖路径配置
tests/         领域规则、仓储契约、事务、配置与分页测试
third_party/   SQLite 3.45.3 amalgamation
dist/          预构建的 Windows x64 免安装版本
```

## 测试

```bat
build.bat test
```

测试不访问网络，也不触碰用户真实数据库：仓储契约通过 `IUnitOfWorkFactory` 抽象在临时 SQLite 文件上
执行，结束后删除临时目录。

## 快捷键

| 快捷键 | 作用 |
| --- | --- |
| `Ctrl+N` | 聚焦新建任务输入框 |
| `Ctrl+F` | 聚焦搜索框 |
| `Ctrl+T` | 切换窗口置顶 |
| `Ctrl+H` | 打开历史页 |
| `Esc` | 关闭弹层 / 取消编辑 |
| `↑` `↓` | 在任务列表中切换选中项 |

## 已知限制

- 仅 Windows x64；不含 Win32/x86 配置。
- 界面仅中文。
- 尚不支持导入/导出与快捷键自定义。
- 多显示器热插拔、远程桌面会话切换下的 DPI 边界场景未做穷举验证。
- 超长文本在列表与筛选条上会被横向截断；单行输入框没有水平滚动，插入符可能移出可视区。

## 第三方组件

见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。运行时只额外依赖系统自带的 Direct2D / DirectWrite /
DirectComposition / DWM 与 VC++ 运行库。

## 许可证

[MIT](LICENSE)
