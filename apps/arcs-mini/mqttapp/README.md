# mqttapp

`mqttapp` 在现有 `apps/arcs-mini` 应用中运行，通过 MQTT over TLS 连接 EMQX 云平台。

## 配置

编辑 `mqttapp_config.h`，至少填写：

- WiFi 已由 `apps/arcs-mini` 的系统网络服务管理，本模块不需要单独配置。
- `MQTTAPP_BROKER_HOST`：EMQX 云平台地址。
- `MQTTAPP_BROKER_PORT`：标准 MQTTS 端口通常为 `8883`。
- `MQTTAPP_CLIENT_ID`：设备唯一 Client ID。
- `MQTTAPP_USERNAME` / `MQTTAPP_PASSWORD`：EMQX 应用凭证。
- `MQTTAPP_CA_CERT_PEM`：EMQX Broker 证书链对应的根 CA，保留 PEM 头尾。
- `MQTTAPP_PUBLISH_TOPIC`：ARCS-MINI 控制上报主题，当前为 `/topic/appcli/up`。
- `MQTTAPP_SUBSCRIBE_TOPIC`：ARCS-MINI 订阅的 ESP 设备主题，当前为 `/topic/espcli/up`。
- `MQTTAPP_VOICE_COMMAND_TOPIC`：语音控制发布主题，当前与 `MQTTAPP_PUBLISH_TOPIC` 相同。

默认启用服务器证书和主机名验证。不要在生产环境关闭 `MQTTAPP_TLS_VERIFY_CERTIFICATE`。

## 语音控制转发

设备完成唤醒和语音识别后，`mqttapp` 会订阅系统内部最终归并后的在线/离线识别命令：

- `VOICE_MSG_RECOGNIZED_ONLINE_COMMAND`
- `VOICE_MSG_RECOGNIZED_OFFLINE_COMMAND`

固件只处理灯光控制命令：

- “开灯”或“打开灯”发布 `{"light":1}`。
- “关灯”或“关闭灯”发布 `{"light":0}`。

命令发布到 `/topic/appcli/up`。设备还会订阅 `/topic/espcli/up`，当前用于接收和记录 ESP 设备上报内容。测试用周期消息默认关闭，不会污染控制主题；可通过 `MQTTAPP_DIAGNOSTIC_MESSAGES` 重新启用。

## 构建

从仓库根目录执行：

```powershell
.\build.ps1 -S .\apps\arcs-mini -B build-mqttapp-mini3 -DBOARD=arcs_mini3
```

Linux、macOS 或 WSL：

```bash
./build.sh -S ./apps/arcs-mini -B build-mqttapp-mini3 -DBOARD=arcs_mini3
```
