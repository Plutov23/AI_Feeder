# AI喂食器硬件模块框架

已完成红外、DS1302 和 SG90 转角测试。当前调试配置启用红外与 SG90，暂停 DS1302。OLED 为旧版驱动，本版硬件不安装。

## 当前模块

| 模块 | 驱动文件 | 默认状态 | 用途 |
| --- | --- | --- | --- |
| HX711 | `main/drivers/hx711.*` | 关闭 | 原始值、平均值、去皮和重量换算 |
| DS1302 | `main/drivers/ds1302.*` | 暂停 | 开机校时与每秒串口输出 |
| 红外传感器 | `main/drivers/ir_sensor.*` | 启用 | 防抖、状态读取和进食事件计数 |
| SSD1306 OLED | `main/drivers/oled_ssd1306.*` | 关闭 | I2C初始化、清屏和英文数字显示 |
| SG90 90° 舵机 | `main/drivers/servo.*` | 启用 | 每秒切换目标角度和输出脉宽日志 |

所有模块由 `main/hardware/feeder_hardware.*` 统一初始化和测试。联网、OneNET和DeepSeek代码仍保留在 `main/main.c`。

## 暂定引脚

| 模块 | 信号 | ESP32-S3 GPIO |
| --- | --- | ---: |
| HX711 | DOUT | 4 |
| HX711 | SCK | 5 |
| OLED | SDA | 8 |
| OLED | SCL | 9 |
| DS1302 | CE/RST | 10 |
| DS1302 | SCLK | 11 |
| DS1302 | IO | 12 |
| 红外传感器 | OUT | 13 |
| 舵机 | PWM | 14 |

引脚集中定义在：

```text
main/hardware/feeder_board_config.h
```

硬件接线变化时只修改这个文件。

## 逐个模块调试

首次接线时，不要一次启用全部模块。在 `feeder_board_config.h` 中每次只把一个开关改成 `1`：

```c
#define FEEDER_ENABLE_HX711 1
#define FEEDER_ENABLE_DS1302 0
#define FEEDER_ENABLE_IR_SENSOR 0
#define FEEDER_ENABLE_OLED 0
#define FEEDER_ENABLE_SERVO 0
```

烧录并确认当前模块正常后，再关闭它并启用下一个模块。全部模块分别通过后才一起启用。

## HX711调试

1. 接好称重传感器和HX711。
2. 启用 `FEEDER_ENABLE_HX711`。
3. 空载时调用 `feeder_hardware_tare_scale()` 去皮。
4. 放置已知重量砝码。
5. 根据原始差值修改 `FEEDER_HX711_SCALE`。

HX711读取带超时。模块未接或数据线异常时不会永久卡住系统。

## DS1302调试

重新启用时将 `FEEDER_ENABLE_DS1302` 设为 `1`。烧录后打开 115200 波特率串口。`FEEDER_DS1302_SET_BUILD_TIME_ON_BOOT` 默认开启，每次开机写入一次固件编译时间并回读校验，随后每秒打印日期和时间。测试备用电池走时或保留现有时间时，将校时开关改为 `0` 后重新烧录。

## 红外传感器调试

将模块数字输出接到 `GPIO13`，与 ESP32-S3 共地，数字输出使用 3.3 V 逻辑。串口开机及检测状态切换时显示 `红外 GPIO13: 有 (level=0)` 或 `红外 GPIO13: 无 (level=1)`。

启用 `FEEDER_ENABLE_IR_SENSOR` 后，每次稳定遮挡计为一次事件。默认按低电平有效，并设置80毫秒防抖。如果模块为高电平有效，修改：

```c
#define FEEDER_IR_ACTIVE_LEVEL 1
```

## OLED调试

驱动默认配置为SSD1306、128x64、I2C地址 `0x3C`。启用后显示标题、重量、红外计数和时间。

## 舵机调试

SG90 按 0–90° 位置舵机配置，测试使用 50 Hz、1000–2000 µs 的初始标定范围。舵机信号线接 `GPIO14`，上电立即从中位 45° 开始，每秒按 45°、80°、45°、10° 循环。115200 波特率串口会显示目标角度和脉宽设置值。

实测转角通过。测试中观察到供电条件不足时 ESP32-S3 反复重启。初次测试时先断开出粮机构。舵机按额定电压使用外部电源，电源负极与 ESP32-S3 的 GND 相连。

将 `FEEDER_SERVO_AUTO_TEST` 设为 `0` 后重新烧录即可退出自动测试。普通模式开机移到关闭角度，确认机构安全后可调用：

```c
feeder_hardware_dispense_once();
```
