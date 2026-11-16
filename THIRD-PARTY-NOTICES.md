# 第三方组件与许可

本项目自身以 MIT 许可发布（见 `LICENSE`），构建与运行依赖以下组件。

## SQLite 3.45.3

- 位置：`third_party/sqlite/`
- 许可：Public Domain（作者已明确放弃著作权）
- 说明：以官方 amalgamation 形式内置，未做源码修改。

## Microsoft Direct2D / DirectWrite / DirectComposition / DWM

- 用途：界面渲染、窗口材质与动画
- 许可：随 Windows SDK 提供，运行时由操作系统自带，不再分发

## Microsoft Visual C++ 运行库

- 用途：`MultiThreadedDLL` 运行时（`vcruntime140*.dll`、`msvcp140*.dll`）
- 许可：依据 Visual Studio 许可条款中的可再分发清单；若 `dist/win64/` 内含这些文件，即为该条款允许的
  应用程序本地部署。也可以改为让用户安装 VC++ 2015-2022 x64 可再发行组件包。

## 字体

界面使用系统自带的 Microsoft YaHei UI，不内嵌字体文件，无字体再分发问题。
