# ESP32-S3 固件规划

## 开发方式

计划使用 ESP-IDF。正式创建工程前先确认 ESP-IDF 版本、开发板型号和硬件引脚。

## 模块边界

```text
firmware/
├── main/                 应用入口和任务装配
└── components/
    ├── feeder/           电机与出粮状态机
    ├── schedule/         定时计划
    ├── infrared/         进食检测
    ├── weight/           HX711 与余粮计算
    ├── environment/      温湿度采集
    ├── display/          OLED 显示
    ├── cloud/            Wi-Fi 与 MQTT
    ├── storage/          NVS 持久化
    └── health_data/      事件与统计
```

## 实现约束

- 电机控制使用状态机，避免长时间阻塞其他任务。
- 计划、校准参数和必要计数写入 NVS。
- 网络断开不影响本地计划。
- 设备不保存大模型 API 密钥。
- 每条远程命令使用 command_id 防重复。
- 传感器读取失败必须返回错误状态，不使用虚假默认值。
- 日志不得输出 Wi-Fi 密码、Token 或其他凭据。

## 待确定事项

- ESP-IDF 版本。
- 分区表。
- OTA 是否纳入第一版。
- MQTT 平台及主题。
- 本地离线数据缓存容量。
- 看门狗和故障恢复策略。
