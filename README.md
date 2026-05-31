# SafeCharge

SafeCharge 是一个面向电动自行车和锂电池设备的改造型 IoT 充电安全系统。项目由 ESP32-S3 端固件和 Flask Web 仪表盘两部分组成：固件负责采集电流、电压、温湿度和烟雾等传感器数据，并根据安全状态控制继电器；仪表盘负责接收数据、展示历史记录、管理阈值并下发控制命令。

## 项目特性

- ESP32-S3 + ESP-IDF 固件，适配 `esp32-s3-devkitc-1`
- LVGL ST7789 本地屏幕界面
- INA219、电压/功率监测、DHT11、烟雾传感器等多源采集
- 继电器控制与安全状态管理
- Flask + Socket.IO 实时仪表盘
- 历史数据、事件日志、阈值配置和远程复位/断电控制

## 目录结构

- `src/`：ESP32 固件主逻辑与各模块实现
- `include/`：固件头文件
- `dashboard/`：Web 仪表盘、数据库与静态资源
- `platformio.ini`：PlatformIO 构建配置
- `partitions.csv`：ESP32 分区表

## 硬件与软件依赖

### 固件端

- PlatformIO
- ESP-IDF
- LVGL 8.3.11

### 仪表盘端

- Python 3.9+
- Flask
- Flask-SocketIO
- Flask-CORS

## 固件说明

固件入口位于 `src/main.c`。启动后会依次初始化 LVGL、界面、传感器任务、Wi-Fi、HTTP 通信、继电器和安全管理模块。

### 常见模块

- `src/ina219.c`：电流/电压/功率采集
- `src/dht11.c`：温湿度采集
- `src/smoke_sensor.c`：烟雾检测
- `src/relay_manager.c`：继电器控制
- `src/safety_manager.c`：安全状态判断
- `src/wifi_manager.c`：无线连接与联网管理
- `src/http_client_manager.c`：向仪表盘上传数据并轮询控制命令
- `src/ui.c`：本地屏幕 UI

## 仪表盘说明

仪表盘入口位于 `dashboard/app.py`，默认监听 `http://localhost:5050`。

### 主要接口

- `POST /api/data`：ESP32 上传实时数据
- `GET /api/command`：ESP32 轮询待执行命令
- `GET /api/latest`：最新一条数据
- `GET /api/history`：历史数据
- `GET /api/events`：事件日志
- `GET/POST /api/thresholds`：读取或更新阈值
- `POST /api/reset`：远程复位
- `POST /api/relay/off`：远程断电

## 快速开始

### 1. 编译并烧录固件

```bash
pio run
pio run -t upload
pio device monitor
```

### 2. 启动仪表盘

```bash
cd dashboard
pip install -r requirements.txt
python app.py
```

浏览器打开：`http://localhost:5050`

### 3. 初始化数据库与静态资源

如果需要重新生成前端静态资源，可运行：

```bash
cd dashboard
python setup_static.py
```

## 运行流程

1. ESP32 启动并连接 Wi-Fi。
2. 固件周期性采集传感器数据。
3. 数据上传到 Flask 仪表盘并写入 SQLite 数据库。
4. 仪表盘通过 Socket.IO 实时刷新页面。
5. 安全状态变化时，系统记录事件并可触发继电器控制或告警命令。

## 备注

- 仓库中包含若干生成文件和运行时文件，后续建议按需要进一步清理并完善 `.gitignore`。
- 实际硬件接线、阈值参数和外设型号请根据你的项目部署环境调整。
