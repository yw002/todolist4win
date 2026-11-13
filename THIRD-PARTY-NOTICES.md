# 第三方组件与许可

本项目自身以 MIT 许可发布（见 `LICENSE`），但构建与运行依赖以下第三方组件。分发二进制时请一并遵守
下列条款。

## SQLite 3.45.3

- 位置：`third_party/sqlite/`
- 许可：Public Domain（作者已明确放弃著作权）
- 说明：以官方 amalgamation 形式内置，未做源码修改。

## MySQL Connector/C++ 8.0

- 用途：MySQL 数据源的驱动，静态链接进可执行文件
- 许可：GPLv2 with FOSS Exception
- 说明：本项目依据 FOSS Exception 在 MIT 许可下使用该驱动。FOSS Exception 允许将受 GPL 约束的
  MySQL 连接器与采用所列开源许可（含 MIT）的程序一同分发。**若你要把本项目用于闭源分发，必须自行
  向 Oracle 获取商业许可，或改用其他驱动。**

## OpenSSL 1.1.1（`libcrypto-1_1-x64.dll` / `libssl-1_1-x64.dll`）

- 用途：MySQL 连接器运行时的 TLS 依赖，随二进制一并分发
- 许可：OpenSSL License 与原始 SSLeay License（均为 BSD 风格）
- 说明：这两个 DLL 来自 MySQL 官方发行包，未做修改。

## Microsoft Direct2D / DirectWrite / DirectComposition / DWM

- 用途：界面渲染与窗口材质
- 许可：随 Windows SDK 提供，运行时由操作系统自带，不再分发

## Microsoft Visual C++ 运行库

- 用途：`MultiThreadedDLL` 运行时（`vcruntime140*.dll`、`msvcp140*.dll`）
- 许可：依据 Visual Studio 许可条款中的可再分发清单；若 `dist/win64/` 内含这些文件，即为该条款允许的
  应用程序本地部署。也可以改为让用户安装 VC++ 2015-2022 x64 可再发行组件包。

## 字体

界面使用系统自带的 Microsoft YaHei UI，不内嵌字体文件，无字体再分发问题。
