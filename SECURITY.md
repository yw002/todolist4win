# 安全说明

## 报告漏洞

请通过 GitHub 的私有漏洞报告（Security → Report a vulnerability）提交，不要开公开 issue。
请在报告里说明复现步骤、影响范围与受影响的版本。

## 数据存放

本应用完全在本机运行，不向任何服务器回传数据。

| 内容 | 位置 | 保护方式 |
| --- | --- | --- |
| 配置文件 | `%APPDATA%\TodoListWin\config.ini` | 明文，只包含窗口与外观设置 |
| 数据库 | `%LOCALAPPDATA%\TodoListWin\data\todolist.db` | 明文 SQLite 文件，依赖 NTFS ACL |
| 日志 | `%LOCALAPPDATA%\TodoListWin\Logs\todolist.log` | 明文，不含凭据 |

应用没有远程数据库凭据，也不需要登录账号。

## 已知的边界

- 本地 SQLite 文件与配置文件本身没有加密，能读到该用户目录的人就能读到数据。若需要更强保护，
  请启用 BitLocker 或 EFS。
- 日志文件可能包含本机路径与错误堆栈，分享日志前请自行检查。
- 应用以当前用户权限运行，不做提权。
