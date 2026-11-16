# 贡献指南

感谢你愿意改进这个项目。下面是这个仓库特有的约定，与通用的 GitHub 流程叠加使用。

## 开发环境

1. Visual Studio 2022 或更高版本，安装「使用 C++ 的桌面开发」工作负载。
2. SQLite 已内置在 `third_party/sqlite/`，不需要额外安装数据库或驱动。
3. 编译并跑测试：

   ```bat
   build.bat test
   ```

   每次构建同时产出 Debug 与 Release；测试只使用临时 SQLite 文件，可以离线运行。

## 代码分层（改动前请先读这一条）

```
domain  ->  不依赖 Windows / Direct2D / SQL / 任何第三方库
core    ->  只依赖 domain 与仓储接口
data    ->  SQL 只允许出现在这一层，使用 SQLite
ui      ->  不拼 SQL，不直接访问数据库
app     ->  负责装配与线程编排
config  ->  配置与路径
```

提交前请自检：

- `domain` 里出现了 `#include <windows.h>` 或 `sqlite3.h` → 不允许。
- `ui` 里出现了 SQL 字符串 → 不允许。
- 用 `sprintf`/`strcpy` 这类不安全的 C 函数 → 用对应的 `_s` 版本或 `std::format`。

## 界面文案与渲染

`RenderResources` 把三个文本格式的 `DWRITE_WORD_WRAPPING` 设成了 `NO_WRAP`，**文字不会自动换行**，
超宽会被 `D2D1_DRAW_TEXT_OPTIONS_CLIP` 静默裁掉。新增 `DrawText` 调用时，请用
`MeasureTextWidth` 核对文案宽度是否放得下，或自行分行绘制。

## 提交信息

采用 `<area>: <做了什么>` 的形式，正文说明「为什么」而不是「改了什么」：

```
ui: 让设置卡片按内容收窄

固定高度按旧表单的五个字段计算，现在只有三行，卡片底部会留出空洞。
```

`area` 取值：`domain` / `core` / `data` / `ui` / `app` / `config` / `build` / `docs` / `tests`。

## 提 PR 前

- `build.bat test` 全绿，且没有把已有断言改松。
- 不提交 `bin/`、`obj/`、`*.pdb`、日志，以及 `%APPDATA%` 下的配置样本。
- 不提交任何真实数据库文件、token 或密码。
- 改了视觉相关代码，请附上浅色与深色两套截图。

## 报告问题

请附上：Windows 版本、DPI、`%LOCALAPPDATA%\TodoListWin\Logs\todolist.log` 的相关片段。
