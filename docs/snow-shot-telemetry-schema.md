# Snow Shot 埋点数据字典

> 版本：1.4.2 · 更新日期：2026-10-30
> 适用：Seq 平台 `SNOW_SHOT_USAGE` 流 / 本地 JSONL 日志

## 1. 概述

Snow Shot 通过 `DiagnosticsService` 在客户端采集用户行为事件，经 512 槽环形缓冲、60 秒批量聚合后，以 **CLEF（Compact Log Event Format）** 上报至内网 Seq 服务器。本地同时在 `%APPDATA%\SpringScreenShot\Spring_ScreenShot\logs\<日期>\` 保留 JSONL 明细，供离线排查。

上报链路：

```
logEvent(category, event, fields)
  → DiagnosticsService::record()
    → EventTap（实时旁路，字段白名单之前）
    → UsageSample（512 槽环形缓冲）
    → 60s 批量 POST → https://desktop.cqczy.cn/seq/api/events/raw?clef
```

上报端点：Seq 标准 Raw Events Ingestion API（`POST /api/events/raw?clef`，NDJSON CLEF，Content-Type `application/vnd.serilog.clef`，无认证头、内网匿名写入）。请求/响应契约与重试策略详见集成规范文档 `snow-shot-telemetry-integration-spec.md` §3。

## 2. Seq 端字段清单

每条上报记录包含以下字段（CLEF JSON）：

| 字段 | 类型 | 必填 | 说明 | 示例 |
|---|---|---|---|---|
| `@t` | ISO8601 | ✅ | UTC 时间戳 | `2026-10-30T05:12:34.567Z` |
| `@mt` | string | ✅ | 消息模板（固定） | `SnowShot usage {EventName}` |
| `@l` | string | ✅ | 日志级别（固定 `Information`） | `Information` |
| `AppName` | string | ✅ | 应用名 | `Snow Shot` |
| `UserId` | string | ✅ | 用户哈希 ID（匿名） | `a1b2c3d4...` |
| `Date` | date | ✅ | 日期 | `2026-10-30` |
| `EventName` | string | ✅ | 事件名（snake_case，见 §3） | `annotation.stroke_committed` |
| `Count` | int | ✅ | 固定 `1` | `1` |
| `AppVersion` | string | ⭕ | 应用版本号 | `1.4.2` |
| `SessionId` | string | ⭕ | 会话 UUID | `178dbc09-e241-...` |
| `Outcome` | string | ⭕ | 结果（见 §4.1） | `succeeded` |
| `Feature` | string | ⭕ | AI 请求用途（仅 `api.request_finished`，见 §4.2） | `translation` |
| `Tool` | string | ⭕ | 标注工具（见 §4.3，**新增**） | `arrow` |
| `Surface` | string | ⭕ | 使用界面（见 §4.4，**新增**） | `capture` |

✅ = 必有；⭕ = 条件性（非空时插入）

## 3. 事件清单（25 个）

### 3.1 会话（session）

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `session.start` | 应用启动 | — |
| `session.clean_shutdown` | 正常关闭 | — |
| `session.unexpected_termination` | 上次崩溃残留标记 | `Outcome` |

### 3.2 截图（capture）

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `capture.finished` | 截图完成/取消 | `Outcome` |

### 3.3 标注（annotation）✨ 新增

| EventName | 触发时机 | 附带维度 | 说明 |
|---|---|---|---|
| `annotation.tool_selected` | 切换标注工具 | `Tool`, `Surface` | 按 Surface 去重，同一工具在同一界面只上报一次 |
| `annotation.stroke_committed` | 落笔提交 | `Tool`, `Surface` | 每次落笔都上报，不去重 |

### 3.4 OCR 文本识别

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `ocr.submitted` | 发起文本识别 | — |
| `ocr.finished` | 识别完成 | `Outcome` |
| `ocr.cancelled` | 识别取消 | — |

### 3.5 录屏（recording）

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `recording.started` | 开始录屏 | — |
| `recording.paused` | 暂停 | — |
| `recording.resumed` | 继续 | — |
| `recording.export_finished` | 导出完成 | `Outcome` |
| `recording.failed` | 录制/渲染失败 | `Outcome` |
| `recording.trim_opened` | 裁剪界面就绪 | — |

### 3.6 导出（export）

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `export.image_finished` | 图片导出完成 | `Outcome` |
| `export.cancelled` | 导出取消 | — |

### 3.7 固定到屏幕（pin）

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `pin.started` | 开始固定 | — |
| `pin.finished` | 固定完成 | `Outcome` |
| `pin.rejected` | 固定被拒 | `Reason` |

### 3.8 剪贴板 / 打印 / API

| EventName | 触发时机 | 附带维度 |
|---|---|---|
| `clipboard.finished` | 复制到剪贴板完成 | `Outcome` |
| `print.snapshot_requested` | 请求打印快照 | — |
| `print.failed` | 打印失败 | `Reason` |
| `api.request_finished` | 任意后端请求结束 | `Feature`（按 `request_kind` 映射） |

## 4. 维度值表

### 4.1 Outcome（结果）

| 值 | 说明 |
|---|---|
| `succeeded` | 操作成功完成 |
| `failed` | 操作失败 |
| `cancelled` | 操作被用户取消 |
| `rejected` | 操作被拒绝（如固定窗口被拒） |

> `Unknown` 不落字段：Outcome 未知时该记录不包含 `Outcome` 字段。

### 4.2 Feature（AI 请求用途，仅 `api.request_finished` 携带）

| 值 | 说明 | 对应 request_kind |
|---|---|---|
| `translation` | 文本翻译 | `translation` |
| `table_extraction` | 表格提取 | `table_extract` / `table_vision_extract` |
| `latex_extraction` | 公式提取 | `latex_extract` / `latex_vision_extract` |
| `image_conversion` | 图片转文字 | `image_conversion` |
| `other` | 其他（保留） | — |

> 其余事件不携带 `Feature` 字段；模型目录刷新（`chat_models`）等无关请求不上报。

### 4.3 Tool（标注工具，21 个）

| 值 | 说明 |
|---|---|
| `select` | 选择 |
| `shape` | 形状 |
| `arrow` | 箭头 |
| `line` | 直线 |
| `free_draw` | 自由画笔 |
| `rectangle_highlight` | 矩形高亮 |
| `pen_highlight` | 笔刷高亮 |
| `eraser` | 橡皮擦 |
| `rectangle_filter` | 矩形马赛克 |
| `watermark` | 水印 |
| `text` | 文本 |
| `serial_number` | 序号 |
| `pen_filter` | 笔刷马赛克 |
| `spotlight` | 聚光灯 |
| `auto_filter` | 自动马赛克 |
| `rectangle_eraser` | 矩形橡皮擦 |
| `brush_eraser` | 笔刷橡皮擦 |
| `distance` | 距离测量 |
| `angle` | 角度测量 |
| `magnifier` | 放大镜 |
| `speech_bubble` | 对话气泡 |

### 4.4 Surface（使用界面，3 个）

| 值 | 说明 |
|---|---|
| `capture` | 截图浮层 |
| `pinned` | 固定到屏幕 |
| `image_editing` | 图像编辑窗口 |

## 5. 本地日志额外字段

本地 JSONL 日志（`diagnostics.cpp` 字段白名单）除上述 Seq 字段外，还保留以下调试字段：

| 字段 | 说明 |
|---|---|
| `category` | 事件类别（如 `snow_shot.app`） |
| `event` | 原始事件名（snake_case） |
| `level` | 日志级别 |
| `format` / `pixel_format` | 图像格式 |
| `expected_width` / `expected_height` | 期望尺寸 |
| `mode` | 工作模式 |
| `dpr` | 设备像素比 |
| `reason` | 失败原因 |
| `width` / `height` | 实际尺寸 |
| `request_kind` | API 请求类型 |
| `duration_ms` | 耗时（毫秒） |
| `source` | 事件来源（截断到 512 字符） |

## 6. Seq 查询示例

### 6.1 基础统计

```sql
-- 日活用户 (DAU)
SELECT Date, COUNT(DISTINCT UserId) AS DAU
FROM SNOW_SHOT_USAGE
GROUP BY Date
ORDER BY Date

-- AI 请求用途分布（Feature 仅随 api.request_finished 出现）
SELECT Feature, COUNT(*) AS Count
FROM SNOW_SHOT_USAGE
WHERE EventName = 'api.request_finished'
GROUP BY Feature
ORDER BY Count DESC

-- 事件分布
SELECT EventName, COUNT(*) AS Count
FROM SNOW_SHOT_USAGE
GROUP BY EventName
ORDER BY Count DESC
```

### 6.2 标注工具分析（新增）

```sql
-- 标注工具使用分布
SELECT Tool, COUNT(*) AS Uses
FROM SNOW_SHOT_USAGE
WHERE EventName = 'annotation.tool_selected'
GROUP BY Tool
ORDER BY Uses DESC

-- 落笔次数按工具
SELECT Tool, COUNT(*) AS Strokes
FROM SNOW_SHOT_USAGE
WHERE EventName = 'annotation.stroke_committed'
GROUP BY Tool
ORDER BY Strokes DESC

-- 标注工具 × 界面热力图
SELECT Surface, Tool, COUNT(*) AS Count
FROM SNOW_SHOT_USAGE
WHERE EventName LIKE 'annotation.%'
GROUP BY Surface, Tool
ORDER BY Count DESC
```

### 6.3 OCR 分析

```sql
-- OCR 成功率
SELECT Outcome, COUNT(*) AS Count
FROM SNOW_SHOT_USAGE
WHERE EventName IN ('ocr.submitted', 'ocr.finished', 'ocr.cancelled')
GROUP BY EventName, Outcome

-- OCR 日趋势
SELECT Date, COUNT(*) AS Submissions
FROM SNOW_SHOT_USAGE
WHERE EventName = 'ocr.submitted'
GROUP BY Date
ORDER BY Date
```

### 6.4 用户留存

```sql
-- 7 日留存用户
SELECT UserId, COUNT(DISTINCT Date) AS ActiveDays
FROM SNOW_SHOT_USAGE
GROUP BY UserId
HAVING ActiveDays >= 2
ORDER BY ActiveDays DESC

-- 周活跃用户 (WAU)
SELECT DATEPART(week, Date) AS Week, COUNT(DISTINCT UserId) AS WAU
FROM SNOW_SHOT_USAGE
GROUP BY DATEPART(week, Date)
ORDER BY Week
```

## 7. 注意事项

1. **事件名大小写**：Seq 端 `EventName` 字段值为 **snake_case**（如 `annotation.stroke_committed`），不是 PascalCase。
2. **维度值大小写**：`Tool` / `Surface` / `Outcome` / `Feature` 字段值均为 **snake_case**（如 `arrow` / `capture` / `succeeded`）。
3. **新增字段**：`Tool` 和 `Surface` 仅在标注相关事件中出现，其他事件这两列为空。
4. **去重规则**：`annotation.tool_selected` 按 `(UserId, Surface, Tool)` 去重，同一会话中同一工具在同一界面只上报一次；`annotation.stroke_committed` 不去重，每次落笔都上报。
5. **上报延迟**：客户端 60 秒批量聚合，Seq 端看到的数据有最多 60 秒延迟。
6. **本地日志**：`%APPDATA%\SpringScreenShot\Spring_ScreenShot\logs\<日期>\snow-shot-*.log` 保留完整明细，含 Seq 端没有的调试字段。

---

**文档维护**：如需新增事件或维度，请同步更新本文档与 `snow_shot/src/telemetry/usageevents.cpp` 的 `kRules` / `usageEventName()` / `usageToolName()` / `usageSurfaceName()`。
