# Clawd Mochi - Claude Code 桌面伙伴升级设计

**日期**：2026-06-06
**状态**：设计稿

---

## 1. 背景与目标

Clawd Mochi 当前是一个独立的 ESP32-C3 桌面摆件——能显示眼睛、画板、终端等动画，通过 WiFi 热点 + 手机网页控制。但它**不知道你电脑上的 Claude Code 在做什么**，因此只能被动看，没法成为工作流的一部分。

本次升级的目标是把 Clawd Mochi 从"会动的摆件"变成"**Claude Code 工作伙伴**"——你写代码时余光扫一眼就能知道 Claude 在干嘛、是否在等你、消耗了多少 token，并且在关键时刻把你的注意力拉回来。

### 核心场景

- 你在另一个屏幕看视频，Claude 跑完了任务 → 玩偶眼睛变 ✓，背景变绿，你看到就回来
- 你离开座位，Claude 卡在权限确认 → 玩偶背景闪黄，你回来一看就知道要批准
- 你在写代码，余光看玩偶屏幕 → 知道当前 Claude 在 Edit / Bash / Read 哪个工具
- 一天结束 → 玩偶显示今天 Claude 帮你跑了多少工具、消耗了多少 token

### 非目标

- 不做物理按键/触摸传感器（保留软件控制即可）
- 不做蜂鸣器（用户没有该硬件）
- 不做 OTA（手动 USB 烧录仍然是开发流程）
- 不替换现有 4 种视图（Normal Eyes / Squish Eyes / Claude Code / Canvas），而是**新增**第 5 种 Status 视图

---

## 2. 功能需求

| # | 功能 | 说明 |
|---|------|------|
| 1 | 工作状态指示 | Claude 思考/工作/空闲时眼睛动画不同 |
| 2 | 当前工具显示 | 屏幕显示 Editing / Running bash / Reading 等 |
| 3 | Token 用量进度条 | 当前会话 token 消耗 / 上限 |
| 4 | 当前任务标题 | 从 TodoWrite 抓取正在做的任务 |
| 5 | 思考时长计时 | 当前工具/任务已运行时长 |
| 6 | Git 分支 + 项目名 | 顶栏显示当前工作目录信息 |
| 7 | 任务完成提醒 | Stop hook 触发时眼睛变 ✓ |
| 8 | 需要确认呼叫 | Notification hook 触发时闪黄 |
| 9 | 错误警示 | 工具失败时眼睛变 ❌，背景闪红 |
| 18 | 心情灯 | 背景色随状态自动渐变 |
| 20 | 每日统计 | 工具调用数、token 总数、会话数、错误数 |

外加 **多 WiFi 自动切换**（基础设施需求）。

---

## 3. 整体架构

```
┌──────────────────────────────────────────────────────────┐
│  你的电脑（Mac/Linux/Windows）                            │
│                                                          │
│  ┌──────────────┐                                        │
│  │ Claude Code  │  ~/.claude/settings.json 全局 hooks    │
│  │              │  ├─ PreToolUse  ─┐                     │
│  │              │  ├─ PostToolUse ─┤                     │
│  │              │  ├─ Stop        ─┼─→ HTTP POST         │
│  │              │  ├─ Notification─┤   localhost:7878    │
│  │              │  └─ UserPromptSubmit ─┘                │
│  └──────┬───────┘                  │                     │
│         │                          ▼                     │
│         │              ┌─────────────────────────┐       │
│         │              │ clawd-daemon (Python)   │       │
│         │ 读取          │  常驻 systemd/launchd   │       │
│         └─读取─────────→│                         │       │
│           ~/.claude/    │  • 接收 hook 事件        │       │
│           sessions/*    │  • 计算 token / git 信息 │       │
│           todos/*       │  • 状态聚合 + 优先级     │       │
│                         │  • 每日统计 (SQLite)     │       │
│                         └────────┬────────────────┘       │
│                                  │ HTTP POST             │
└──────────────────────────────────┼───────────────────────┘
                                   │ (家里/公司 WiFi)
                                   ▼
┌──────────────────────────────────────────────────────────┐
│  ESP32-C3 (AP + STA 双模)                                │
│                                                          │
│  ┌────────────────────────────────────────────────────┐  │
│  │ STA: 自动连已知 WiFi（WiFiMulti，最多 5 个）        │  │
│  │ AP : ClaWD-Mochi (192.168.4.1) ← 手机控制 + 配网   │  │
│  └────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────┐  │
│  │ 状态机（优先级从高到低）                            │  │
│  │ ERROR > AWAITING > DONE > THINKING > IDLE          │  │
│  └────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────┐  │
│  │ 视图（5 个，可切换）                                │  │
│  │ • Normal Eyes  • Squish Eyes                       │  │
│  │ • Claude Code  • Canvas                            │  │
│  │ • Status（新增 - 显示工作状态/git/token/任务）     │  │
│  └────────────────────────────────────────────────────┘  │
│  ┌────────────────────────────────────────────────────┐  │
│  │ NVS 持久化：WiFi 凭证 × N + 每日统计               │  │
│  └────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

### 数据流（典型场景：Claude 调用 Edit 工具）

```
1. Claude 准备调 Edit
   → PreToolUse hook 触发
   → POST localhost:7878/event {type:"pre_tool", tool:"Edit", ...}

2. daemon 收到事件
   → 更新内存状态：state=THINKING, tool="Edit", start_t=now
   → 读取 git 信息、当前 todo
   → POST esp32:80/api/status { state, tool, task, git_branch, ... }

3. ESP32 收到 status
   → 更新状态机
   → 重绘 Status 视图（如果当前在该视图）
   → 渐变背景色到蓝色（THINKING 对应色）

4. Edit 完成
   → PostToolUse hook 触发，success=true
   → daemon 更新 state=IDLE，记一笔到每日统计
   → POST esp32 → 背景渐变回橙色，眼睛恢复正常
```

---

## 4. 组件设计

### 4.1 ESP32 固件（`clawd_mochi/clawd_mochi.ino`）

保持单文件原则（README 明确要求新手友好），但内部用清晰的逻辑分区：

```cpp
// === 配置 ===
//   WiFi AP 名称、密码、引脚定义、屏幕尺寸（已有）

// === 状态机 ===
// 枚举值即优先级数字（数字越大优先级越高）
enum ClaudeState {
  IDLE      = 0,
  THINKING  = 1,
  DONE      = 2,
  AWAITING  = 3,
  ERROR     = 4
};
struct ClaudeStatus {
  ClaudeState state;
  String tool;          // "Edit" / "Bash" / "Read" ...
  String task;          // 当前 TodoWrite 任务标题
  uint32_t duration_s;  // 工具/任务已耗时
  uint32_t tokens_used;
  uint32_t tokens_max;
  String git_branch;
  String project;
  uint32_t last_update_ms;  // 用于检测 daemon 离线
};

// === 视图系统 ===
enum View { VIEW_EYES_NORMAL, VIEW_EYES_SQUISH, VIEW_CODE, VIEW_DRAW, VIEW_STATUS };
//   每个视图一个 draw 函数，已有 4 个，新增 drawStatusView()

// === WiFi 子系统 ===
//   WiFiMulti 管理多 WiFi
//   AP 始终开启（ClaWD-Mochi）
//   STA 后台扫描+连接已保存的最强信号

// === Web 服务器路由 ===
//   原有：/ /view /speed /bg /pen /backlight /term /draw
//   新增：
//     POST /api/status       接收 daemon 推送
//     POST /api/stats/daily  接收每日统计
//     GET  /api/state        Web UI 查询当前状态
//     GET  /wifi             WiFi 配置页面（HTML）
//     POST /wifi/add         添加 WiFi
//     POST /wifi/delete      删除 WiFi
//     GET  /wifi/scan        扫描周围 WiFi

// === NVS 持久化 ===
//   wifi:count                  存了几个 WiFi
//   wifi:0:ssid / wifi:0:pass   第 0 个
//   wifi:1:ssid / wifi:1:pass   第 1 个 ...
//   stats:YYYY-MM-DD            每日统计 JSON
```

#### Status 视图布局（240×240）

```
┌────────────────────────┐  ← y=0
│ ▌main · clawd-mochi  📶│  顶栏 24px：git 分支 + 项目 + WiFi 信号
├────────────────────────┤  ← y=24
│                        │
│                        │  眼睛区 120px
│      ●     ●           │  状态动画：
│      ●     ●           │   IDLE→眨眼  THINKING→旋转
│                        │   DONE→✓     ERROR→❌
│                        │   AWAITING→闪烁
├────────────────────────┤  ← y=144
│ ◆ Editing file.ts      │  工具行 24px
│ ▶ 实现登录功能          │  任务行 24px
├────────────────────────┤  ← y=192
│ ⏱ 12s    ░░░░░░ 45/200k│  底栏 48px：耗时 + token 进度条
└────────────────────────┘  ← y=240

背景色（心情灯）：
  IDLE      → 橙 (原配色)
  THINKING  → 蓝
  AWAITING  → 黄（呼吸闪烁）
  DONE      → 绿
  ERROR     → 红
```

> 字符串过长时自动截断 + ellipsis（避免越界）

#### 状态机优先级与超时

```
ERROR     最高     持续 5 秒后降到 IDLE
AWAITING  次高     持续到 daemon 推送清除
DONE      中       持续 3 秒后降到 IDLE
THINKING  低       持续到 PostToolUse / Stop
IDLE      最低     默认状态
```

如果超过 **60 秒**没收到 daemon 推送，自动切回 IDLE 并显示"daemon offline"提示。

#### 视图切换策略

- 默认显示用户上次在 Web UI 选择的视图（NVS 记忆）
- 当从 IDLE 切换到 ERROR / AWAITING / DONE 时，**自动切到 Status 视图**（重要事件需要看到）
- 切换为 ERROR/AWAITING/DONE 之前，把当前视图记下来；过 5 秒/3 秒/被清除后再切回去
- 用户可通过 Web UI 关闭"自动切换"开关

---

### 4.2 Python Daemon（`clawd-daemon/`）

#### 目录结构

```
clawd-daemon/
├── pyproject.toml           # 依赖：fastapi/uvicorn, httpx, gitpython, watchdog
├── README.md                # 安装、启动、调试说明
├── clawd_daemon/
│   ├── __init__.py
│   ├── main.py              # 入口：启动 HTTP server + 后台任务
│   ├── config.py            # ESP32 IP、端口、轮询频率
│   ├── state.py             # ClaudeStatus 状态对象 + 优先级合并逻辑
│   ├── server.py            # FastAPI app：接收 hook 事件的 HTTP 端点
│   ├── pusher.py            # 推送状态到 ESP32（节流 + 重试）
│   ├── hooks/
│   │   ├── pre_tool.py
│   │   ├── post_tool.py
│   │   ├── stop.py
│   │   ├── notification.py
│   │   └── user_prompt.py
│   ├── collectors/
│   │   ├── git_info.py      # subprocess 调 git
│   │   ├── token_counter.py # 读 transcript JSONL 累加 token
│   │   ├── todo_reader.py   # 读 ~/.claude/todos/*.json
│   │   └── daily_stats.py   # SQLite 聚合
│   ├── esp32_client.py      # httpx 封装
│   └── storage.py           # SQLite 初始化与 CRUD
├── hooks_config/
│   └── settings.json        # 用户拷贝到 ~/.claude/settings.json 的模板
└── tests/
    ├── test_state.py
    ├── test_token_counter.py
    └── test_pusher.py       # mock ESP32
```

#### Hook → daemon 通信

Hook 是非常短命的 shell 命令。最轻量的做法是用 `curl` 把 hook 的 JSON payload 转发到 daemon 的本地 HTTP 端口：

```jsonc
// ~/.claude/settings.json
{
  "hooks": {
    "PreToolUse":  [{ "hooks": [{ "type": "command", "command": "curl -sX POST http://127.0.0.1:7878/event/pre_tool -d @-" }]}],
    "PostToolUse": [{ "hooks": [{ "type": "command", "command": "curl -sX POST http://127.0.0.1:7878/event/post_tool -d @-" }]}],
    "Stop":        [{ "hooks": [{ "type": "command", "command": "curl -sX POST http://127.0.0.1:7878/event/stop -d @-" }]}],
    "Notification":[{ "hooks": [{ "type": "command", "command": "curl -sX POST http://127.0.0.1:7878/event/notification -d @-" }]}],
    "UserPromptSubmit":[{ "hooks": [{ "type":"command","command":"curl -sX POST http://127.0.0.1:7878/event/prompt -d @-"}]}]
  }
}
```

> 这样 hook 本身永远秒级返回（curl 失败也不阻塞 Claude），daemon 异步处理。

#### 状态推送节流

ESP32 渲染屏幕大约 100ms，太频繁推送会卡。daemon 做节流：

- 状态发生质变（state/tool/task 变化）→ **立即推送**
- 仅 duration / tokens 数字变化 → **每 1 秒推送一次**
- 没变化 → **每 10 秒推送一次心跳**（让 ESP32 知道 daemon 还活着）

#### Token 计算

读 `~/.claude/sessions/<session_id>.jsonl`，累加 `usage.input_tokens + usage.output_tokens + usage.cache_*`。
上限按当前模型确定（Sonnet 4.6 = 200k，Opus 4.8 = 200k，Haiku = 200k；保存在 `config.py`）。

#### 每日统计（SQLite）

```sql
CREATE TABLE daily_stats (
  date TEXT PRIMARY KEY,        -- "2026-06-06"
  tools_called INTEGER,
  tokens_total INTEGER,
  sessions INTEGER,
  errors INTEGER
);

CREATE TABLE tool_events (
  ts INTEGER, tool TEXT, success INTEGER, tokens INTEGER
);  -- 用于按日聚合
```

每天 00:00 触发任务：聚合昨天的数据，POST 给 ESP32（ESP32 也存一份 NVS，便于离线显示）。

---

### 4.3 WiFi 配置子系统（ESP32 端）

#### 启动流程

```
1. NVS 读取已保存的 WiFi 列表
2. WiFi.mode(WIFI_AP_STA)
3. 启动 AP: ClaWD-Mochi / clawd1234
4. 如果有保存的 WiFi：
   - 用 WiFiMulti 尝试连接（自动选信号最强）
   - 连上 → STA 模式生效
   - 连不上 → 仅 AP 模式（用户用手机配网）
5. 每 5 分钟后台扫描一次，如果发现更强的已知 WiFi 自动切换
```

#### 配网界面（`GET /wifi`）

挂在 Web UI 主页一个"⚙️ WiFi"按钮，跳转到 `/wifi` 显示：

- **已保存的网络列表**（带"已连接"标记和删除按钮）
- **扫描周围 WiFi 按钮**（显示信号强度）
- **手动添加**（SSID + 密码 + 添加按钮）
- **当前连接信息**（SSID、IP、信号强度）

最多保存 5 个 WiFi。

---

## 5. 配置与部署

### 用户首次安装流程

```
1. 烧录新版 .ino 固件到 ESP32
2. ESP32 启动 → 显示 "Connect to ClaWD-Mochi WiFi"
3. 用户手机连 ClaWD-Mochi 热点 → 浏览器打开 192.168.4.1
4. 点 ⚙️ WiFi → 扫描 → 选家里 WiFi → 输密码 → 保存
5. ESP32 连上家里 WiFi → 屏幕显示其在家里网络的 IP（如 192.168.1.123）

6. 电脑安装 daemon：
   $ pip install -e clawd-daemon/
   $ clawd-daemon --esp32-host 192.168.1.123  (写入配置文件)
   $ clawd-daemon install  (创建 launchd/systemd 服务)
   $ clawd-daemon install-hooks  (写入 ~/.claude/settings.json)

7. 启动 Claude Code → 屏幕开始实时显示状态
```

### 配置文件

```toml
# ~/.config/clawd-daemon/config.toml
esp32_host = "192.168.1.123"
esp32_port = 80
listen_port = 7878
claude_dir = "~/.claude"
push_interval_ms = 1000
heartbeat_interval_s = 10
model_token_limits = { sonnet = 200000, opus = 200000, haiku = 200000 }
```

---

## 6. 错误处理

### ESP32 端

| 错误 | 处理 |
|------|------|
| Daemon 60s 无心跳 | 屏幕显示"daemon offline"小图标，状态降到 IDLE |
| WiFi 断线 | WiFiMulti 自动重连，AP 仍然可用 |
| 所有已存 WiFi 都连不上 | 仅 AP 模式运行，屏幕提示"setup wifi" |
| 收到非法 status JSON | 丢弃 + 在 serial log 打印 |
| 字符串超出屏幕宽度 | 截断 + "…" |
| NVS 写入失败 | 内存中保留，下次启动恢复默认 |

### Daemon 端

| 错误 | 处理 |
|------|------|
| ESP32 离线（HTTP 失败） | 内存中保留状态，每 5s 重试；失败不阻塞 hook |
| Claude 没装 hook 配置 | daemon 启动时检测并打印 warning |
| transcript 文件读不到 | token 显示为 "?"，其他字段正常 |
| SQLite 损坏 | 重建数据库，丢失历史统计（保留当天） |
| Hook 调 curl 但 daemon 没启动 | curl 失败不阻塞 Claude，丢失该事件 |

---

## 7. 测试策略

### ESP32 端

- **手动测试**：每种状态用 `curl -X POST esp32/api/status -d '...'` 触发，肉眼检查屏幕
- **配网测试**：连/断/换 WiFi，重启 ESP32 验证持久化
- **压力测试**：daemon 1 秒推一次连续 10 分钟，看是否卡顿/掉帧

### Daemon 端（pytest）

- **单元测试**：
  - state.py 优先级合并逻辑
  - token_counter.py 各种 transcript 格式
  - daily_stats.py 跨日聚合
- **集成测试**：
  - mock ESP32（fastapi TestClient），验证推送 payload
  - mock Claude session 文件，验证 collector 正确性
- **端到端测试**：
  - 实际触发 Claude Code 跑一个简单任务
  - 验证 daemon 收到所有 5 类 hook 事件
  - 验证最终推送给 ESP32 的状态序列正确

---

## 8. 实施分阶段（即使一次性设计也建议按顺序实现）

虽然设计一次性完成，实际开发可以按以下顺序，每步独立可用：

1. **基础设施**：ESP32 多 WiFi 支持 + AP/STA 双模 + 配网页面
2. **Daemon 骨架**：FastAPI 接收 hook 事件 + 写日志（不推 ESP32）
3. **状态显示**：Status 视图 + 状态机 + ESP32 接收 status API
4. **核心数据采集**：git_info、todo_reader、token_counter
5. **心情灯 + 优先级动画**：背景色渐变 + 各状态眼睛动画
6. **每日统计**：SQLite + 凌晨聚合 + 推 ESP32
7. **打磨**：错误处理、节流、装机脚本、文档

---

## 9. 已知风险与权衡

| 风险 | 评估 | 缓解 |
|------|------|------|
| 单文件 .ino 变得太大（当前 48KB） | 中等 | 用清晰分区注释，必要时拆出 .h（但 README 要求单文件友好）|
| ESP32-C3 内存 400KB，Web UI 字符串占用大 | 低 | Status 视图复用现有绘图函数，避免新增大字符串 |
| Hook 每次调 curl 有少许开销（~10ms） | 低 | 用户感知不到，比启动 Python 子进程快得多 |
| Daemon 跨平台启动（mac launchd / Linux systemd） | 中等 | 提供两套模板 + `install` 命令自动选择 |
| Token 上限因模型不同 | 低 | 在 transcript 里能读到模型名，daemon 按模型查表 |
| 多 WiFi 切换时连接中断（最坏 10 秒） | 低 | 中断时 ESP32 端用最后一次状态显示，不黑屏 |

---

## 10. 验证方法

设计执行完成后，按以下步骤端到端验证：

1. **配网验证**：手机进 ClaWD-Mochi 热点 → 配置家里 WiFi → 重启 ESP32 → 自动连上家里 WiFi
2. **多 WiFi**：再添加一个手机热点 → 关闭家里 WiFi → 应自动切换到手机热点
3. **Daemon 安装**：`clawd-daemon install` → `systemctl status clawd-daemon` 显示 running
4. **Hook 验证**：跑一个 Claude Code 命令，查看 daemon 日志收到 PreToolUse / PostToolUse 事件
5. **Status 视图**：在 Web UI 切到 Status，触发 Claude 干活，看屏幕实时变化
6. **每个状态**：手动 curl 推 ERROR / AWAITING / DONE，验证背景色 + 眼睛动画
7. **自动切换**：触发 ERROR 状态，验证 Status 视图自动弹出
8. **每日统计**：调整系统时间到次日 00:00，验证统计 POST 到 ESP32 并显示

---

## 附录 A：HTTP API 详细规范

### `POST /api/status`（daemon → ESP32）

请求体：
```json
{
  "state": "thinking",       // idle | thinking | awaiting | done | error
  "tool": "Edit",            // 可空
  "task": "实现登录功能",     // 可空，<= 30 字符
  "duration_s": 12,
  "tokens_used": 45000,
  "tokens_max": 200000,
  "git_branch": "main",      // 可空
  "project": "clawd-mochi"   // 可空
}
```

响应：`200 OK { "ok": true }` 或 `400 { "error": "..." }`

### `POST /api/stats/daily`（daemon → ESP32，每日午夜）

```json
{
  "date": "2026-06-06",
  "tools_called": 234,
  "tokens_total": 1850000,
  "sessions": 7,
  "errors": 3
}
```

### `GET /api/state`（Web UI → ESP32）

返回当前完整状态（用于 Web UI 同步显示）。

### `GET /wifi/scan`（Web UI → ESP32）

返回周围 WiFi 列表：
```json
[
  { "ssid": "home-wifi", "rssi": -45, "saved": true, "connected": true },
  { "ssid": "office-wifi", "rssi": -78, "saved": true, "connected": false },
  { "ssid": "neighbor", "rssi": -82, "saved": false, "connected": false }
]
```

### `POST /wifi/add` / `POST /wifi/delete`

```json
{ "ssid": "...", "password": "..." }    // add
{ "ssid": "..." }                       // delete
```

---

## 附录 B：状态机伪代码（ESP32）

```cpp
void onStatusReceived(ClaudeStatus newStatus) {
  // 优先级合并（如果新状态优先级低于当前 ERROR/AWAITING/DONE 的临时状态，忽略）
  if (currentTempState && newStatus.state < currentTempState) {
    pendingStatus = newStatus;  // 暂存
    return;
  }

  currentStatus = newStatus;
  lastUpdateMs = millis();

  // 重要事件自动切换到 Status 视图
  if (autoSwitchEnabled &&
      (newStatus.state == ERROR || newStatus.state == AWAITING || newStatus.state == DONE)) {
    if (currentView != VIEW_STATUS) {
      previousView = currentView;
      currentView = VIEW_STATUS;
    }
  }

  triggerRedraw();
}

void loop() {
  // 60 秒无更新 → daemon offline
  if (millis() - lastUpdateMs > 60000) {
    currentStatus.state = IDLE;
    showOfflineIndicator = true;
  }

  // ERROR/DONE 自动超时
  if (currentStatus.state == ERROR && millis() - errorStartMs > 5000) {
    currentStatus.state = IDLE;
    revertViewIfAuto();
  }
  // ... AWAITING 等待清除，DONE 类似
}
```
