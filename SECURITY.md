# 安全说明

## 报告漏洞

请通过 GitHub 的私有漏洞报告（Security → Report a vulnerability）提交，不要开公开 issue。
请在报告里说明复现步骤、影响范围与受影响的版本。

## 凭据与数据存放

本应用完全在本机运行，不向任何服务器回传数据。

| 内容 | 位置 | 保护方式 |
| --- | --- | --- |
| 配置文件 | `%APPDATA%\TodoListWin\config.ini` | 明文，但不含密码 |
| 本地数据库 | `%LOCALAPPDATA%\TodoListWin\data\todolist.db` | 明文 SQLite 文件，依赖 NTFS ACL |
| MySQL 密码 | `%LOCALAPPDATA%\TodoListWin\credentials\mysql.cred` | Windows DPAPI，当前用户作用域 |

DPAPI 的密文只能由同一 Windows 用户在同一台机器上解开。**这意味着复制到另一台机器或另一个用户下
无法解密**，需要重新输入密码。

## 关于内置的默认连接参数

程序内置 `localhost:3306/root/123456` 作为本地开发的占位默认值，目的是让本地环境开箱可用。
它不是任何真实环境的凭据，请在「数据库」设置里改成你自己的账号。

## 已知的边界

- 本地 SQLite 文件与配置文件本身没有加密，能读到该用户目录的人就能读到数据。若需要更强保护，
  请启用 BitLocker 或 EFS。
- 日志文件 `todolist.log` 不含密码，但可能包含主机名、数据库名与错误堆栈，分享日志前请自行检查。
- 应用以当前用户权限运行，不做提权。
