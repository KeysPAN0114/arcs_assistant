# MQTT 语音灯光控制改动记录

## 目标

在 ARCS-MINI 上通过语音唤醒并识别“开灯”或“关灯”后，将灯光控制结果以 MQTT JSON 消息上报，供 ESP 或其他 MQTT 设备使用。

最终协议：

| 项目 | 配置 |
| --- | --- |
| 语音控制发布主题 | `/topic/appcli/up` |
| ESP 设备订阅主题 | `/topic/espcli/up` |
| 开灯 payload | `{"light":1}` |
| 关灯 payload | `{"light":0}` |
| MQTT QoS | 1 |

## 实现过程

### 1. 语音最终命令转发

`mqttapp` 订阅以下系统事件：

- `VOICE_MSG_RECOGNIZED_ONLINE_COMMAND`
- `VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND`

最终识别命令通过 FreeRTOS 队列交给独立 MQTT 任务处理，避免在语音事件回调中阻塞：

- “开灯”或“打开灯”映射为 `{"light":1}`。
- “关灯”或“关闭灯”映射为 `{"light":0}`。
- 非灯光命令不发布，避免污染 `/topic/appcli/up`。

相关实现位于 `apps/arcs-mini/mqttapp/mqttapp.c`。

### 2. 在线 MCP 灯光命令接入

实机日志显示，云端语音“开灯”实际走的是 MCP 调用：

```json
{
  "method": "tools/call",
  "params": {
    "name": "ls.led_switch",
    "arguments": {
      "value": true
    }
  }
}
```

该路径不会直接产生最终在线识别命令，因此 `mqttapp` 额外订阅 `VOICE_MSG_CLOUD_MCP`，解析 `ls.led_switch` 调用：

- `value=true` 发布 `{"light":1}`。
- `value=false` 发布 `{"light":0}`。
- 若离线识别命令随后重复上报相同灯光状态，5 秒内只保留一次，避免在线 MCP 和离线回退路径重复发送。

MCP 解析和去重逻辑只新增在 `apps/arcs-mini/mqttapp/mqttapp.c`，没有修改原 `mcp_tool_led.c` 的工具实现。

### 3. MQTT 主题配置

当前配置见 `apps/arcs-mini/mqttapp/mqttapp_config.h`：

```c
#define MQTTAPP_PUBLISH_TOPIC "mqttapp/pub"
#define MQTTAPP_SUBSCRIBE_TOPIC "/topic/espcli/up"
#define MQTTAPP_VOICE_COMMAND_TOPIC "/topic/appcli/up"
#define MQTTAPP_PUBLISH_QOS 1
#define MQTTAPP_SUBSCRIBE_QOS 1
#define MQTTAPP_DIAGNOSTIC_MESSAGES 1
```

说明：

- `MQTTAPP_VOICE_COMMAND_TOPIC` 是灯光控制实际发布主题。
- `MQTTAPP_SUBSCRIBE_TOPIC` 用于订阅 ESP 上报内容，当前收到消息后打印日志。
- 原有启动消息和周期测试消息继续使用 `mqttapp/pub`，不与灯光控制主题混用。
- 如需关闭周期测试消息，将 `MQTTAPP_DIAGNOSTIC_MESSAGES` 设置为 `0`。

### 4. MQTT 发布生命周期

新增 `mqttapp_publish_voice_command()` 对外接口，声明位于 `apps/arcs-mini/mqttapp/mqttapp.h`。

发布流程：

1. 语音或 MCP 回调将命令复制到队列。
2. 独立语音命令任务等待 MQTT 连接。
3. 命令转换为灯光 JSON。
4. 通过 coreMQTT-Agent 发布并等待完成回调。
5. MQTT Agent 异常退出时调用 `MQTTAgent_CancelAll()`，确保挂起命令能够结束回调。

## 对原有代码的影响

- 没有修改 `apps/arcs-mini/mcp-tools/mcp_tool_led.c`。
- 没有修改 `src/`、`arcs-sdk/` 或设备驱动代码。
- 本次功能代码集中在新建的 `apps/arcs-mini/mqttapp/` 模块。
- 保留了 mqttapp 原有周期测试消息行为。

## 构建与烧录

构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -S .\apps\arcs-mini -B build-mqttapp-mini3 -DBOARD=arcs_mini3
```

构建产物：

- `build-mqttapp-mini3/arcs-mini.bin`
- `build-mqttapp-mini3/arcs-mini3.lpk`

烧录命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\adb_download.ps1 `
  -S res\arcs-mini3 -B build-mqttapp-mini3 app
```

实机烧录信息：

- ADB 设备：`6d7b101a3423caa5`
- 板型：`arcs_mini3`
- APP 分区地址：`0x600000`
- 烧录方式：ADB app-only

## 当前验证状态

- `arcs_mini3` 固件构建通过。
- APP 固件已通过 ADB 成功烧录，设备烧录后可重新上线。
- 日志中已确认 MQTT 周期消息正常发布，说明 MQTT TLS 和 Agent 发布链路可用。
- 语音触发 MCP 灯光开关的实机路径已经从日志确认。
- `/topic/appcli/up` 的最终语音触发发布结果仍需在 MQTT 客户端或 ESP 端进行一次实机验收。

## 注意事项

- `mqttapp_config.h` 当前包含明文 Broker 地址、账号和密码，生产环境应改为设备唯一凭证或安全配置下发方式。
- ESP 若不直接使用 `{"light":x}` 协议，可由 ESP 解析 JSON，或在 EMQX 侧转换为目标设备需要的 payload。
