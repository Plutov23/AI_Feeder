#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define ONENET_MQTT_URI "mqtt://YOUR_PRODUCT_ID.mqtts.acc.cmcconenet.cn:1883"
#define ONENET_PRODUCT_ID "YOUR_ONENET_PRODUCT_ID"
#define ONENET_DEVICE_NAME "YOUR_ONENET_DEVICE_NAME"
#define ONENET_DEVICE_TOKEN "YOUR_ONENET_DEVICE_TOKEN"

#define DEEPSEEK_API_URL "https://api.deepseek.com/chat/completions"
#define DEEPSEEK_API_KEY "YOUR_DEEPSEEK_API_KEY"
#define DEEPSEEK_MODEL "deepseek-chat"

#define AI_TEST_PROMPT \
    "以下是ESP32-S3智能喂食器生成的模拟近7天历史数据，并非真实测量：\n" \
    "11-13 温度25.1℃ 湿度52% 进食4次 总量82g 平均时长68秒；\n" \
    "11-14 温度25.4℃ 湿度55% 进食4次 总量79g 平均时长65秒；\n" \
    "11-15 温度26.2℃ 湿度58% 进食5次 总量96g 平均时长74秒；\n" \
    "11-16 温度27.1℃ 湿度64% 进食3次 总量61g 平均时长52秒；\n" \
    "11-17 温度27.6℃ 湿度68% 进食2次 总量38g 平均时长40秒；\n" \
    "11-18 温度26.8℃ 湿度63% 进食3次 总量55g 平均时长49秒；\n" \
    "11-19 温度26.3℃ 湿度60% 进食2次 总量31g 平均时长36秒，余粮120g。\n" \
    "请分析进食趋势、环境变化和异常风险，并给出建议。" \
    "使用中文纯文本，不超过120字。"

#endif
