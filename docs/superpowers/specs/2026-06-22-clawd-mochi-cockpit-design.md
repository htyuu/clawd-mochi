# Clawd Mochi Cockpit 设计文档

**日期**：2026-06-22
**状态**：设计已确认，待实现
**作者**：yuu + Claude

---

## 1. 项目概述

### 背景

用户已有 **Clawd Mochi** 实体桌面伙伴：ESP32 + 1.54" TFT 屏幕的硬件玩偶，通过 Python daemon 接收 Claude Code 的 hooks 事件，把工作状态实时显示在屏幕上。daemon 同时把每次工具调用记录到 SQLite（`daily_stats` + `tool_events`）。

实体玩偶只能**显示**，无任何输入交互能力。当前缺少一个面向用户的 GUI 来：

- 主动控制玩偶的表情/颜色/状态
- 查看 daemon 累积的历史数据
- 在物理玩偶和电脑之间建立"双向"的仪式感

### 目标

搭建一个**单用户本地 Web 应用**（Clawd Mochi Cockpit），作为实体玩偶的 GUI 伴侣，提供：

1. 🎮 **遥控**：手动切换玩偶的表情、心情灯、情景模式
2. 📊 **数据看板**：今日工作数据可视化
3. ⏪ **时间回放**：拖动时间轴查看一天工作流
4. 🌅 **仪式打卡**：早安/午休/晚安/下班按钮，触发玩偶状态序列 + 留下打卡记录
5. 📖 **年鉴**：周报、月报、年度回顾

### 非目标

- ❌ 不做多用户、不做用户系统、不做社交
- ❌ 不部署到公网，仅本地访问
- ❌ 不修改 daemon 源码（保持只读关系）
- ❌ 不接入 AI（评语用规则模板生成）
- ❌ 不做手机端响应式（电脑浏览器优先）

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│  浏览器 (http://localhost:3000)                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  ① 今日驾驶舱  ② 时间回放  ③ 仪式  ④ 年鉴               │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────┬──────────────────────────────┬────────────┘
                  │ HTTP                         │ HTTP 轮询
                  ▼                              ▼
        ┌─────────────────────────────────────────────────┐
        │  Cockpit Server (Node + Express, 端口 3000)      │
        │  ─────────────────────────────────────────────  │
        │  /api/today      → 读 daemon SQLite             │
        │  /api/timeline   → 读 tool_events 时间轴         │
        │  /api/year       → 聚合 daily_stats              │
        │  /api/status     → 透传 daemon 状态              │
        │  /api/control    → 透传 daemon /event/*          │
        │  /api/rituals    → 自家库存仪式打卡              │
        │  /api/presets    → 自家库情景模式预设            │
        └────┬─────────────────────────────────┬──────────┘
             │ 只读 SQL                          │ HTTP 转发
             ▼                                  ▼
   ┌──────────────────────┐         ┌──────────────────────┐
   │ daemon 的 SQLite       │         │  Clawd Daemon         │
   │ ~/.local/share/        │         │  127.0.0.1:7878       │
   │ clawd-daemon/clawd.db  │         │  (FastAPI)            │
   │                       │         └──────────┬───────────┘
   │ - daily_stats         │                    │
   │ - tool_events         │                    │ HTTP
   └──────────────────────┘                    ▼
                                    ┌──────────────────────┐
                                    │  ESP32 (实体玩偶)      │
                                    │  192.168.x.x          │
                                    └──────────────────────┘

   ┌──────────────────────┐
   │ Cockpit 自有 SQLite    │  ← 仪式打卡、情景模式预设
   │ h5-cockpit/cockpit.db │     (不污染 daemon 数据)
   └──────────────────────┘
```

### 核心架构原则

1. **只读 daemon 数据**：不修改 daemon 代码、不写它的库
2. **新数据另开一个库**：Cockpit 专属数据写到独立 `cockpit.db`
3. **控制走 daemon**：H5 的"切表情/切心情灯"指令转发到 daemon `/event/*`，由 daemon 维护权威状态
4. **单文件优先**：前端不引入构建工具，单 `index.html` + Tab 切换

---

## 3. 技术栈

| 层 | 选型 | 理由 |
|----|------|------|
| 后端 | Node.js + Express 4 | 与已有 demo 一致，简单 |
| 数据库 | better-sqlite3 (Cockpit 自家) + sqlite3 只读连接 (daemon 库) | 与已有 demo 一致 |
| 前端 | 原生 HTML/CSS/JS | 无构建步骤，单文件易维护 |
| 实时同步 | HTTP 轮询 (2s 一次) | SSE 不必要，轮询足够简单 |

---

## 4. 功能详解

### 4.1 Tab ①：今日驾驶舱（首页）

**布局**：左右两栏

#### 左栏 — 遥控台

- **虚拟 Clawd 图标**：纯 CSS 绘制的像素螃蟹，根据 `/api/status` 返回的当前状态实时同步显示表情和背景色
- **表情按钮组**（4 个）：正常 😺 / 笑眼 😆 / 错愕 😵 / 困倦 😴
  - 点击 → POST `/api/control` → daemon → ESP32
- **心情灯按钮组**（5 色）：橙 / 蓝 / 黄 / 绿 / 红
  - 点击 → 切换玩偶背景色
- **情景模式按钮**：从 `cockpit.db` 的 `presets` 表读取
  - 默认 3 个内置预设：写代码 / 开会 / 摸鱼
  - 每个预设 = (表情 + 颜色 + 自动切换开关) 组合
  - 点击 → 一次性下发多参数到 daemon

#### 右栏 — 今日数据

- **4 个核心数字卡片**：
  - 工具调用次数
  - Token 使用量（含进度条，max=200,000）
  - 会话数（distinct session）
  - 错误数
- **工具调用排行**：横向柱状图，从今天的 `tool_events` 按 `tool` 字段聚合 GROUP BY
- **当前任务卡片**：显示 daemon 实时推送的 `task` / `git_branch` / `project` / `session_duration_s` / `tool_count`

#### 状态指示

页面顶部状态栏：
- 🟢 daemon ✅ ESP32 ✅ — 全连通
- 🟡 daemon ✅ ESP32 ❌ — daemon 在跑但 ESP32 离线
- 🔴 daemon ❌ — daemon 没启动（遥控按钮置灰禁用）

---

### 4.2 Tab ②：时间回放

**布局**：单列纵向

- **日期选择器**：默认今天，可前后翻日期，"今天" 按钮一键回到当天
- **横向时间条**：
  - 把当天 `tool_events` 按 5 分钟一格聚合（一天最多 288 格）
  - 每格颜色取该 5 分钟内"主导状态"（按工具频率推断）
  - 颜色对应规则同心情灯：空闲橙、工具调用蓝、错误红
- **拖动播放头**：左右移动到任意 5 分钟格，下方信息卡片更新
- **信息卡片**显示该时刻：
  - 时间点
  - 当时正在使用的最频繁工具
  - 项目 + 分支（取该时间窗最后一条 event 关联的 session）
  - Token 用量
  - 最近 5 个工具调用序列
- **重现按钮**：点击后调 `/api/control` 把玩偶切回那个时刻的状态，5 秒后自动恢复当前实时状态

**实现细节**：
- 时间轴用 `<canvas>` 绘制（性能比 288 个 div 好）
- 播放头是覆盖在 canvas 上的一个 div，拖动通过监听 mousemove

---

### 4.3 Tab ③：仪式

**布局**：上方按钮组 + 下方热力图

#### 仪式按钮（4 个）

| 仪式 | 触发的玩偶状态序列 |
|------|------------------|
| ☀️ 早安 | 橙底 + 正常表情 + 闪烁 3 次 |
| 🍱 午休 | 黄底 + 困倦表情 + 持续 30 分钟（屏保） |
| 🌙 晚安 | 进入屏保模式 |
| 🚀 下班 | 绿底 + 笑眼 3 秒 |

点击按钮时：
1. POST 到 `/api/rituals` → 写入 `ritual_logs` 表
2. 转发对应的预设指令到 daemon

按钮上显示当天该仪式的打卡时间（已打卡则置灰但可重复点击）。

#### 打卡日历

- 显示过去 90 天的热力图（类似 GitHub contribution graph）
- 每格颜色按当天完成的仪式数量染色：
  - 4 个全打：深绿
  - 2-3 个：中绿
  - 1 个：浅绿
  - 0 个：灰
- 鼠标悬停看具体哪几个打了

#### 连续打卡统计

- 早安连续天数
- 晚安连续天数
- 全部 4 仪式连续天数

---

### 4.4 Tab ④：年鉴

**布局**：三个子 Tab + 内容区

#### 子 Tab：周报 / 月报 / 年度回顾

每个子 Tab 包含相同的结构：

1. **顶部数字卡片**（3 个）：工具调用 / Token / 会话数
2. **每日趋势图**：横向柱状图（周报 7 列，月报 30 列，年报 12 列汇总）
3. **本期之最**：
   - 最肝的一天（按 `tools_called` 排序取最高）
   - 最爱用的工具（聚合 `tool_events.tool`）
   - 最忙的项目（如果 git_branch/project 字段有）
4. **Clawd 评语**：规则生成

#### 评语规则（伪代码）

```
if tools_called > 500 in week:
    "这周累了。最高一天到 xxx 次。Clawd 心疼。"
elif tools_called < 50 in week:
    "这周清闲。Clawd 也在摸鱼。"
elif errors / tools_called > 0.05:
    "错误率偏高。今天有点难。"
elif token_usage 接近 limit:
    "token 烧得猛。该重启会话了。"
else:
    "稳稳的一周。Clawd 满意。"
```

模板组合的池子约 20 条，根据多维度命中规则随机选一条以避免重复。

---

## 5. 数据库设计

### 5.1 daemon 的 SQLite（只读）

路径：`~/.local/share/clawd-daemon/clawd.db`

| 表 | 关键字段 | Cockpit 用途 |
|----|---------|--------------|
| `tool_events` | ts, tool, success, tokens, session | 今日数据、时间轴、工具排行 |
| `daily_stats` | date, tools_called, tokens_total, sessions, errors | 年鉴聚合 |

Cockpit 只 `SELECT`，永不写入。

### 5.2 Cockpit 自有 SQLite

路径：`h5-cockpit/cockpit.db`

```sql
-- 仪式打卡记录
CREATE TABLE ritual_logs (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    type         TEXT NOT NULL,          -- 'morning' | 'lunch' | 'night' | 'offwork'
    created_at   DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_ritual_logs_created ON ritual_logs(created_at);

-- 情景模式预设
CREATE TABLE presets (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT NOT NULL UNIQUE,   -- '写代码' | '开会' | '摸鱼'
    state        TEXT NOT NULL,          -- 'idle'|'thinking'|'done'|'awaiting'|'error'
    color        TEXT NOT NULL,          -- hex 颜色
    bright       INTEGER DEFAULT 255,    -- 亮度 0-255
    auto_switch  INTEGER DEFAULT 1       -- 是否自动切换视图
);
```

启动时插入 3 个默认预设（写代码 / 开会 / 摸鱼），如果用户没删除则一直存在。

---

## 6. API 设计

所有接口返回 `application/json`。

### 6.1 数据查询接口（读 daemon SQLite）

| 方法 | 路径 | 返回示例 |
|------|------|---------|
| `GET` | `/api/today` | `{ tools_called, tokens_total, sessions, errors, top_tools: [...], project, git_branch }` |
| `GET` | `/api/timeline?date=2026-06-22` | `{ buckets: [{ ts, state, tool, count, ... }, ...] }` 长度 288 |
| `GET` | `/api/year?range=week\|month\|year` | `{ summary: {...}, trend: [...], top: {...} }` |

### 6.2 状态/控制接口（转发 daemon）

| 方法 | 路径 | 功能 |
|------|------|------|
| `GET` | `/api/status` | 返回 daemon 当前 ClaudeStatus 全字段 |
| `POST` | `/api/control` | Body `{ state, color, bright, auto_switch }` 转发到 daemon |

### 6.3 仪式接口（Cockpit 自有）

| 方法 | 路径 | 功能 |
|------|------|------|
| `GET` | `/api/rituals?days=90` | 返回过去 N 天的打卡记录（按日期聚合） |
| `POST` | `/api/rituals` | Body `{ type }`，写入 ritual_logs 并触发对应控制 |
| `GET` | `/api/rituals/streak` | 返回 `{ morning, night, all_four }` 连续天数 |

### 6.4 情景模式接口（Cockpit 自有）

| 方法 | 路径 | 功能 |
|------|------|------|
| `GET` | `/api/presets` | 列出所有预设 |
| `POST` | `/api/presets` | 新增/更新（按 name upsert） |
| `DELETE` | `/api/presets/:id` | 删除预设 |

---

## 7. 前端组织

### 7.1 文件结构

```
h5-cockpit/
├── server.js          # Express 后端
├── cockpit.db         # 自有数据库（自动生成）
├── package.json
├── public/
│   ├── index.html     # 单页应用
│   ├── style.css      # 全局样式
│   └── app.js         # 前端逻辑
└── README.md
```

### 7.2 前端 Tab 切换

```html
<nav>
  <button data-tab="today" class="active">今日驾驶舱</button>
  <button data-tab="timeline">时间回放</button>
  <button data-tab="ritual">仪式</button>
  <button data-tab="year">年鉴</button>
</nav>

<section id="tab-today" class="tab active">...</section>
<section id="tab-timeline" class="tab">...</section>
<section id="tab-ritual" class="tab">...</section>
<section id="tab-year" class="tab">...</section>
```

每个 Tab 是一个独立的 `<section>`，切换时改变 `class="active"`，CSS 控制 `display: none / block`。

### 7.3 实时状态同步

- 启动时立即调一次 `/api/status`
- 之后每 2 秒轮询一次
- 拿到结果后更新：
  - 虚拟 Clawd 图标的表情和背景色
  - 顶部状态指示器（daemon/ESP32 连通性）
  - 今日驾驶舱右栏的"当前任务"卡片

### 7.4 视觉主题

| 状态 | 背景色 | 用途 |
|------|--------|------|
| 空闲 | `#FF8C42` 暖橙 | 首页默认 |
| 思考 | `#2B6EB0` 深蓝 | 工具调用中 |
| 完成 | `#2D9D5A` 翠绿 | 完成时 |
| 待确认 | `#E8C547` 姜黄 | 等待响应 |
| 错误 | `#D64545` 珊瑚红 | 出错时 |
| 深色面板 | `#1A1A2E` 暗紫 | 年鉴/回放页 |

Clawd 图标用纯 CSS（多个 `box-shadow` 拼像素螃蟹），不依赖图片，避免静态资源管理。

---

## 8. 错误处理

| 场景 | 处理方式 |
|------|---------|
| daemon 未启动 (`/health` 超时) | 顶部红色横幅 "⚠️ daemon 未连接"。遥控按钮置灰禁用。数据面板正常显示（daemon 数据库即使 daemon 没跑也能读） |
| daemon SQLite 不存在 | API 返回 `{ empty: true, message: "暂无数据" }`，前端显示空状态 |
| ESP32 不在线 (daemon push 失败) | daemon 自己处理，Cockpit 不感知。控制指令静默失败，虚拟 Clawd 仍然在前端反映用户的点击 |
| cockpit.db 损坏 | 启动时检测，损坏则备份原文件后重建 |
| 跨天/某天无数据 | 图表绘制空白格子，不抛错 |
| 时间轴拖到未来日期 | 前端禁用，日期选择器只让选今天及之前 |

---

## 9. 实施考虑

### 9.1 不做的事情（YAGNI）

- ❌ 用户认证
- ❌ 多设备同步
- ❌ WebSocket（轮询够用）
- ❌ ORM（直接 SQL）
- ❌ 国际化
- ❌ Docker 化部署
- ❌ 单元测试框架（手测为主）

### 9.2 开发顺序建议

1. 搭基础工程结构（Express + 路由骨架）
2. 实现 daemon SQLite 只读访问，跑通 `/api/today`
3. 实现 `/api/status` 透传，前端跑通虚拟 Clawd 实时同步
4. 完成今日驾驶舱 Tab（端到端走通一个完整功能）
5. 加入仪式 Tab（含 cockpit.db 写入）
6. 加入时间回放 Tab
7. 加入年鉴 Tab
8. 加入情景模式预设（CRUD 完整闭环）
9. 错误处理打磨 + 视觉优化

### 9.3 验收标准

- ✅ 在本地启动 `npm start`，浏览器打开 http://localhost:3000 可访问
- ✅ daemon 运行时，虚拟 Clawd 状态与实体玩偶同步 (≤2s 延迟)
- ✅ 点遥控按钮，实体玩偶能响应（屏幕变化）
- ✅ 点仪式按钮，cockpit.db 写入新记录，热力图能反映
- ✅ 跨天打开页面，今日数据自动归零，年鉴里多出新一行
- ✅ daemon 停止后，控制功能优雅禁用，数据面板仍能看历史

---

## 10. 未来扩展（暂不实现）

- 接入 AI 评语（用户已主动放弃，留作后续）
- 自定义表情包（结合 Clawd Mochi 的 canvas 功能上传像素图）
- 导出周报为图片分享
- 长时间趋势（>1 年的多年对比）
- 集成番茄钟逻辑到仪式 Tab
