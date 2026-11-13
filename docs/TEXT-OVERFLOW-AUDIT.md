# TEXT-OVERFLOW-AUDIT

`RenderResources` 构造三个 `IDWriteTextFormat` 时统一设成 `DWRITE_WORD_WRAPPING_NO_WRAP`
（`src/ui/RenderResources.cpp:22-25`），`DrawText` 再以 `D2D1_DRAW_TEXT_OPTIONS_CLIP` 绘制。结果是：
**任何一条文案只要比 rect 宽，就会被从右边缘静默切掉，没有任何换行、省略号或告警。**

这份审计把 `src/ui/` 下全部 62 个 `DrawText` 调用点过了一遍，用实测宽度和 rect 宽度比对。

## 测量方法

- 参考几何：客户端 **1180×760 DIP，96 DPI**（与 QA 截图同尺寸）。窗口尺寸变化时，凡是 `right - 常量`
  形式的 rect 会跟着变宽，凡是固定宽度的 rect（如侧边栏、按钮）不变；下表的"rect 宽"标注了是否可伸缩。
- 字体：`Microsoft YaHei UI`，三个字号 `18 / 14 / 12` DIP，与 `RenderResources` 完全一致。
- 测量手段：GDI `GetTextExtentPoint32W`，用同一个字体族、同一 DIP 字号。
  **没有直接用 `RenderResources::MeasureTextWidth`**，原因是它固定用 14 DIP 的 `body_` 格式测量，
  而本表有大量 12 DIP 的次要文案；把 14 DIP 的结果按 `12/14` 缩放会引入几个 DIP 的误差，
  而本轮恰好有好几处余量只有 2~3 DIP。GDI 与 DirectWrite 的理想步进对同一 TrueType 中文字面
  相差不到 1%，比缩放更准。`SettingsDialog` 里那段运行期分行逻辑仍然用 `MeasureTextWidth`
  加 `12/14` 换算，因为那里只需要"放得下/放不下"的粗判。
- 中文在 12 DIP 下实测正好 12.0 DIP/字，14 DIP 下 14.0 DIP/字，18 DIP 下 18.0 DIP/字，与 em 假设一致。
  因此下表"容量"一列可以直接按字数换算。

### NO_WRAP 的唯一例外

`src/ui/controls/TextBox.cpp:108` 在 `multiline_` 为真时临时把**共享的** `body_` 格式改成
`DWRITE_WORD_WRAPPING_WRAP`，画完立刻改回 `NO_WRAP`。所以只有"编辑任务 / 新增任务"里的备注框会
正常换行，其余全部调用点都是单行。这段临时改写是全局状态，任何在它两行之间插入的 `DrawText`
都会意外获得换行能力——属于隐患，但不是当前缺陷。

## 结论汇总

| 分类 | 条数 |
| --- | --- |
| 调用点总数（不含 `RenderResources::DrawText` 定义） | 62 |
| 实测溢出、**本轮已修** | 3 |
| 实测溢出、**未修**（涉及用户输入或他人负责的文件） | 12 |
| 余量偏窄（< 80 DIP，能显示，但改文案就溢出） | 6 |
| 安全 | 41 |

## 一、本轮已修（3 条）

| 文件:行 | 文案 | rect 宽 | 实测宽 | 溢出 | 处置 |
| --- | --- | --- | --- | --- | --- |
| `MainWindow.cpp:318` | `已保存的 MySQL 凭据无法解密，可能由其他 Windows…` | 432 | **434** | **是，-2** | rect 左边界 `right-460` → `right-640`（宽 612）。文本右对齐，只是向左长进空白，footer 概览文字止于 x≈603，不撞 |
| `MainWindow.cpp:304` | `MySQL · 只读 · 数据可能不是最新 · 托盘不可用` | 240 | **254** | **是，-14** | rect 右边界改为自适应 `max(420, min(600, width-390))`，1180 宽下为 420；760 DIP 最小宽度下退回原 240，不会钻到动作按钮下面 |
| `SettingsDialog.cpp:230` | `无法打开数据目录，请手动定位：C:\Users\Mayn\…` | 452 | **465** | **是，-13** | 改文案为 `无法自动打开数据目录，请按上方路径手动打开`（252）。路径本来就在错误行上方两行里完整显示，不丢信息 |

前两条只有"MySQL + 只读 + 托盘不可用"这种组合才会触发，第三条只要 `ShellExecute` 失败就触发，
而长路径必然触发，属于真机可复现。

## 二、实测溢出但未修（12 条）

全部与**用户输入**有关：分类名上限 80 字符、任务标题上限 500 字符（`TaskRules.cpp:59/73`），
而下方容量普遍只有 6~8 个字。也就是说这些不是极端情况，起个 9 个字的分类名就会看到截断。

| 文件:行 | 文案 | rect 宽 | 实测宽 / 容量 | 溢出 | 处置建议 |
| --- | --- | --- | --- | --- | --- |
| `NavigationView.cpp:92` | 分类名（用户输入） | 104（14 DIP，固定） | **7 个汉字**；80 字符上限可达 1120 | **是** | measure 后截断加 `…`。文件不在本轮归属内 |
| `NavigationView.cpp:76` | 待办计数 | 32（12 DIP） | 5 位数字 **35** ≥ 10000 条时溢出 | **是** | 万级以上改 `1.2万` 或放大到 40 |
| `NavigationView.cpp:93` | 分类待办计数 | 28（12 DIP） | `1234` = **28**，正好卡死；5 位 **35** | **是** | 同上 |
| `TaskPage`（Button） | 分类按钮文字（用户输入） | 90（14 DIP） | **6 个汉字**；长名 182 | **是** | 同上，`TaskPage.cpp` 不在归属内 |
| `HistoryPage`（Button） | 分类筛选按钮文字（用户输入） | 117（14 DIP） | **8 个汉字**；长名 182 | **是** | 同上 |
| `ChartsControl.cpp:71` | 分类名（用户输入） | 94（12 DIP） | **7 个汉字**；长名 182 | **是** | 同上 |
| `ChartsControl.cpp:80` | `已完成/总数` | 38（12 DIP） | `9999/9999` = **61**，`999/999` = 47 | **是** | 千级以上溢出；缩成 `1.2k/2.0k` 或加宽到 60 |
| `PopupMenu.cpp:23` | 菜单项文字（用户输入） | 174（190 宽菜单）/ 204（220 宽菜单） | **12 / 14 个汉字**；80 字符上限可达 1120 | **是** | 同上 |
| `TaskListControl.cpp:123` | 任务标题（用户输入） | 792（14 DIP，可伸缩） | **56 个汉字**；500 字符上限可达 7000 | **是** | 同上。注意 `:128` 的删除线也用 `MeasureTextWidth` 量同一串，会一起溢出 |
| `HistoryListControl.cpp:54` | 历史快照标题（用户输入） | 712（14 DIP） | **50 个汉字** | **是** | 同上 |
| `SettingsDialog`（Button `category_`） | 任务编辑的分类按钮文字（用户输入） | 254（14 DIP） | **18 个汉字**；长名 364 | **是** | 我的文件，但同类问题在 5 个文件里，建议一次做成共享 helper |
| `TextBox.cpp:105/109` | 输入框内容 | 字段宽 - 20（可伸缩） | 不定 | **结构性** | 见下 |

### TextBox 是一个结构性缺陷，不只是溢出

`TextBox::Paint` 用 `PushAxisAlignedClip(inner)` 裁剪，配合 `NO_WRAP`，且**没有任何水平滚动**：
`CaretX()` 从 `inner.left` 起算，一旦文本超过 inner 宽度，插入符会被裁到可视区之外，
用户看不到自己在输入什么，也无法用方向键把视野拉回来。字段越窄越明显
（历史页"开始/结束 YYYY-MM-DD"字段在 1180 宽下只有 173 DIP，inner 153）。
这是输入体验缺陷，不是排版缺陷，修复需要给 TextBox 加一个 `scrollOffset_`。

## 三、余量偏窄的 6 条（能显示，但改文案就要重算）

| 文件:行 | 文案 | rect 宽 | 实测宽 | 余量 |
| --- | --- | --- | --- | --- |
| `NavigationView.cpp:76` | 4 位计数 `1234` | 32 | 28 | **4** |
| `ChartsControl.cpp:102` | 日期标签 `12/31` | 48 | 33 | 15 |
| `SettingsDialog.cpp:415` | `数据库文件` | 98（12 DIP） | 60 | 38 |
| `SettingsDialog`（Button） | `测试并保存` | 128（14 DIP） | 70 | 58 |
| `SettingsDialog.cpp:403` / `:414` | `数据源` | 98 | 36 | 62 |
| `SettingsDialog`（Button） | `清除已保存密码` | 170（14 DIP） | 98 | 72 |

真正危险的只有 `NavigationView.cpp:76` 的 4 DIP 余量：计数到 9999 就贴边，到 10000 直接溢出（见第二节）。

## 四、安全清单（41 条，已实测）

| 调用点 | 文案 | rect 宽 | 实测宽 |
| --- | --- | --- | --- |
| `MainWindow.cpp:289` | `TodoList`（18） | 116 | 81 |
| `NavigationView.cpp:75` | `全部任务` / `今日待办` / `逾期任务`（14） | 118 | 56 |
| `NavigationView.cpp:78` | `分类`（12） | 164 | 28 |
| `NavigationView.cpp:98` | `+ 新增分类`（14） | 164 | 61 |
| `NavigationView.cpp:103/108` | `历史记录` / `统计分析`（14） | 138 | 56 |
| `HistoryPage.cpp:58` | `历史事件`（18） | 180 | 76 |
| `HistoryPage`（Button） | `应用筛选` | 86 | 56 |
| `HistoryPage`（Button） | `批量恢复` | 122 | 56 |
| `HistoryPage`（Button） | `永久删除所选` | 116 | 84 |
| `StatisticsPage.cpp:9` | `统计分析`（18） | 164 | 76 |
| `StatisticsPage`（Button） | `刷新统计` | 110 | 56 |
| `ChartsControl.cpp:33` | `当前任务完成率`（18） | 296 | 133 |
| `ChartsControl.cpp:58` | `100%`（18） | 152.7 | 32 |
| `ChartsControl.cpp:63` | `分类任务与完成量`（18） | 533 | 152 |
| `ChartsControl.cpp:64` | `暂无分类数据`（12） | 565 | 72 |
| `ChartsControl.cpp:86` | `近 7 天完成事件趋势`（18） | 880 | 176 |
| `ChartsControl.cpp:87` | `暂无完成事件`（12） | 912 | 72 |
| `DatePicker.cpp:212` | `‹` `›`（18） | 30 | 7 |
| `DatePicker.cpp:218` | `2026 年 1 月`（18） | 204 | 116 |
| `DatePicker.cpp:231` | `日`…`六`（12） | 37.7 / 格 | 12 |
| `DatePicker.cpp:262` | 日期数字（14） | 37.7 / 格 | 16 |
| `DatePicker.cpp:271` | `今天`（12） | 72 | 24 |
| `DatePicker.cpp:274` | `清空`（12） | 72 | 24 |
| `HistoryListControl.cpp:39` | `暂无历史事件`（18） | 912 | 72 |
| `HistoryListControl.cpp:40` | `完成任务后，完成与取消完成事件会显示在这里`（12） | 912 | 252 |
| `HistoryListControl.cpp:53` | `✓`（12） | 18 | 9 |
| `HistoryListControl.cpp:58` | `取消完成 · 未分类 · 事件 2026-01-08 14:23:45`（12） | 712 | 268 |
| `HistoryListControl.cpp:59` | `恢复`（12） | 56 | 24 |
| `HistoryListControl.cpp:60` | `永久删除`（12） | 66 | 48 |
| `ModalDialog.cpp:37` | `删除任务`（18） | 576 | 76 |
| `ModalDialog.cpp:38` | `确定要删除这 3 条历史记录吗？此操作无法撤销。`（14） | 576 | 310 |
| `TaskListControl.cpp:75` | `暂无任务`（18） | 912 | 48 |
| `TaskListControl.cpp:76` | `在上方输入标题并按 Enter，或使用 Ctrl+N 新建`（12） | 912 | 261 |
| `TaskListControl.cpp:131` | `未分类 · 2026-01-08 · 已完成`（12） | 792 | 176 |
| `TaskListControl.cpp:132` | `删除`（12） | 46 | 28 |
| `TaskPage`（Button） | `设置日期` / `截止 2026-01-08` | 192.6 | 56 / 108 |
| `TaskPage`（Button） | `中优先级` / `高优先级` / `低优先级` | 119.7 | 56 |
| `TaskPage`（Button） | `添加任务` | 86 | 56 |
| `TaskPage`（Button） | `全部状态` / `仅待办` / `仅已完成` | 147 | 56 |
| `TaskPage`（Button） | `全部优先级` / `完成留底` / `完成归档` | 147 | 70 / 56 / 56 |
| `SettingsDialog.cpp:417` | `（路径不可用）`（12） | 346 | 84 |
| `SettingsDialog`（Button） | `本地数据库` / `MySQL` | 120 / 94 | 70 / 47 |

其余 `SettingsDialog` 调用点（标题 `数据源设置` 452/95、`数据源` 标签 98/36、`优先级` 98/36、
路径目录行 346/285、文件名行 346/104、提示语两行 452/**349** 与 452/**216**、错误行 452/≤434、
`取消` 130/28、`保存` 128/28）均在安全区，本轮加入 24 DIP 卡片内边距后 rect 从 464/354 收到 452/346，
上述文案仍然放得下。

## 五、如果要继续修，建议顺序

1. **`TextBox` 加水平滚动**（结构性，影响所有输入框，最值得做）。
2. 一个共享的 `Ellipsize(resources, text, maxWidth)`，放在 `src/ui/TextFit.{h,cpp}`，
   然后统一替换 6 处用户输入渲染点：`NavigationView.cpp:92`、`TaskPage` 分类按钮、
   `HistoryPage` 分类按钮、`ChartsControl.cpp:71`、`PopupMenu.cpp:23`、
   `TaskListControl.cpp:123`、`HistoryListControl.cpp:54`、`SettingsDialog` 任务编辑的分类按钮。
3. 计数类文案的千位缩写（`NavigationView.cpp:76/93`、`ChartsControl.cpp:80`）。

第 2 项会同时改到 5 个不属于本轮的源文件，所以本轮只记录、不动手。
