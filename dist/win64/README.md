# TodoList for Windows — 免安装版本

这是预先构建好的 Windows x64 Release 版本，**不需要安装，也不需要 MySQL**。

## 运行

双击 `todolist4win_mysql.exe` 即可。首次启动会自动在本地创建 SQLite 数据库并初始化表结构。

- 系统要求：Windows 10 版本 1903 及以上，或 Windows 11，64 位。
- 如果提示找不到 `VCRUNTIME140.dll` / `MSVCP140.dll`，说明系统缺少 VC++ 运行库，
  安装「Microsoft Visual C++ 2015-2022 可再发行组件包 (x64)」即可；本目录已附带这 4 个文件，
  正常情况下不需要额外安装。

## 文件说明

| 文件 | 说明 |
| --- | --- |
| `todolist4win_mysql.exe` | 主程序 |
| `libcrypto-1_1-x64.dll`、`libssl-1_1-x64.dll` | MySQL 连接器的 TLS 依赖（连本地 SQLite 时用不到，但缺失会导致程序无法启动） |
| `msvcp140.dll` 等 4 个 | Visual C++ 运行库，随程序本地部署 |

## 数据放在哪

| 内容 | 位置 |
| --- | --- |
| 配置 | `%APPDATA%\TodoListWin\config.ini` |
| 本地数据库 | `%LOCALAPPDATA%\TodoListWin\data\todolist.db` |
| MySQL 密码（加密） | `%LOCALAPPDATA%\TodoListWin\credentials\mysql.cred` |
| 日志 | `%LOCALAPPDATA%\TodoListWin\Logs\todolist.log` |

想整体备份或迁移，复制这几个位置即可；想重置为初始状态，删掉 `todolist.db` 后重新启动。

## 切换到 MySQL

点右上角「数据库」→ 选「MySQL」→ 填主机、端口、用户名、密码、数据库名 →「测试并保存」。
密码会用 Windows DPAPI 加密保存，配置文件里不会出现明文，下次打开设置也不会回显。

本地库和 MySQL 库是两套独立数据，切换只改变「当前用哪一侧」，不会互相复制或合并。

## 卸载

本程序不写注册表、不安装服务。删掉程序目录，再按需删除上表中 `%APPDATA%` 与
`%LOCALAPPDATA%` 下的 `TodoListWin` 文件夹即可。

## 许可

MIT。第三方组件许可见随仓库的 `THIRD-PARTY-NOTICES.md`。
