# TodoList for Windows · 免安装版

这是预先构建好的 Windows x64 Release 版本，不需要安装，也不需要数据库服务。

双击 `todolist4win.exe` 即可。首次启动会自动在本地创建 SQLite 数据库并初始化表结构。

## 系统要求

- Windows 10 版本 1903 及以上，或 Windows 11，64 位。
- 如果提示找不到 `VCRUNTIME140.dll` / `MSVCP140.dll`，说明系统缺少 VC++ 运行库，
  安装「Microsoft Visual C++ 2015-2022 可再发行组件包 (x64)」即可；本目录已附带这 4 个文件，
  正常情况下不需要额外安装。

## 文件说明

| 文件 | 说明 |
| --- | --- |
| `todolist4win.exe` | 主程序 |
| `msvcp140.dll`、`msvcp140_atomic_wait.dll`、`vcruntime140.dll`、`vcruntime140_1.dll` | VC++ 运行库，随程序本地部署 |

## 数据放在哪

| 内容 | 位置 |
| --- | --- |
| 配置 | `%APPDATA%\TodoListWin\config.ini` |
| 数据库 | `%LOCALAPPDATA%\TodoListWin\data\todolist.db` |
| 日志 | `%LOCALAPPDATA%\TodoListWin\Logs\todolist.log` |

想整体备份或迁移，复制这几个位置即可；想重置为初始状态，删掉 `todolist.db` 后重新启动。

## 关闭行为

默认关闭窗口会收起到系统托盘；托盘图标左键单击可重新打开主窗口。右键菜单里有「新建任务」、
「打开主窗口」和「退出」。

## 卸载

本程序不写注册表、不安装服务。删掉程序目录，再按需删除上表中 `%APPDATA%` 与
`%LOCALAPPDATA%` 下的 `TodoListWin` 文件夹即可。

## 许可

MIT。第三方组件许可见随仓库的 `THIRD-PARTY-NOTICES.md`。
