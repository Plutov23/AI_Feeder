# 通信与数据协议

> 本文档是第一版字段草案。云平台和后端技术确定后，再补充正式主题、URL、鉴权和错误码。

## 1. 通用约定

- 时间使用 Unix 时间戳，单位为秒。
- 重量单位为克。
- 温度单位为摄氏度。
- 湿度单位为百分比。
- 每条消息包含 `device_id`。
- 控制命令包含唯一 `command_id`，用于防重复和查询结果。
- 字段名称使用小写蛇形命名。

## 2. 设备状态

```json
{
  "device_id": "feeder-001",
  "timestamp": 0,
  "online": true,
  "temperature_c": 0.0,
  "humidity_percent": 0.0,
  "food_weight_g": 0.0,
  "food_level_percent": 0,
  "eating_count_today": 0,
  "wifi_rssi": 0,
  "sensor_status": {
    "infrared": "ok",
    "temperature_humidity": "ok",
    "weight": "ok"
  }
}
```

## 3. 出粮命令

```json
{
  "command_id": "uuid",
  "device_id": "feeder-001",
  "type": "feed",
  "amount_g": 20,
  "requested_at": 0
}
```

约束：

- `amount_g` 必须大于 0。
- 最大值由服务端和固件共同限制。
- ESP32-S3 收到重复 `command_id` 时不得重复出粮。

## 4. 命令结果

```json
{
  "command_id": "uuid",
  "device_id": "feeder-001",
  "status": "succeeded",
  "planned_amount_g": 20,
  "actual_amount_g": 0.0,
  "error_code": null,
  "finished_at": 0
}
```

`status` 可选值：

- `accepted`
- `running`
- `succeeded`
- `failed`
- `rejected`

## 5. 定时计划

```json
{
  "schedule_id": "schedule-001",
  "hour": 8,
  "minute": 30,
  "amount_g": 20,
  "enabled": true,
  "revision": 1
}
```

设备以 `schedule_id` 和 `revision` 判断新增、更新和重复同步。

## 6. 进食事件

```json
{
  "event_id": "uuid",
  "device_id": "feeder-001",
  "started_at": 0,
  "ended_at": 0,
  "duration_s": 0,
  "food_consumed_g": 0.0,
  "trigger": "infrared"
}
```

只有满足有效遮挡时间和有效离开时间的事件才计入次数。

## 7. AI 分析结果

```json
{
  "analysis_id": "uuid",
  "device_id": "feeder-001",
  "period": "daily",
  "date": "YYYY-MM-DD",
  "risk_level": "low",
  "summary": "",
  "suggestions": [],
  "generated_at": 0,
  "model_version": ""
}
```

`risk_level` 可选值：

- `low`
- `medium`
- `high`
- `unknown`

## 8. 错误码规划

| 错误码 | 含义 |
| --- | --- |
| INVALID_AMOUNT | 出粮克数不合法 |
| RATE_LIMITED | 操作过于频繁 |
| MOTOR_TIMEOUT | 电机执行超时 |
| WEIGHT_SENSOR_ERROR | 称重传感器异常 |
| SCHEDULE_CONFLICT | 计划冲突 |
| DEVICE_OFFLINE | 设备离线 |
| INTERNAL_ERROR | 未分类内部错误 |
