# 添加尼特利滤桶到 Home Assistant

固件跑起来、`http://<ESP32-IP>/filter/state` 能返回 JSON 之后，把下面这些加到 HA。

## 0. 先扫描一次，让固件记住滤桶 MAC

浏览器打开（`s` 是扫描秒数）：

```
http://10.0.0.54/filter/scan?s=8
```

返回里 `mac` 字段有值就成了。固件会把它存进 NVS，断电不丢。

如果扫不到，把 ESP32 靠近滤桶，或者手动指定：

```
http://10.0.0.54/filter/setmac?mac=AA:BB:CC:DD:EE:FF
```

## 1. 接口一览

| 接口 | 作用 | 返回 |
|---|---|---|
| `/filter/state` | 读缓存状态 | `{connected,gear,model,err,charge,voltage,mac,age_ms}` |
| `/filter/read` | 主动问设备要一次状态 | `{sent,gear,model}` |
| `/filter/scan?s=8` | 扫描并记住滤桶 MAC | `{found,mac}` |
| `/filter/setmac?mac=..` | 手动指定 MAC | `{mac}` |
| `/filter/on` | 开泵（沿用当前档位） | `{sent,gear}` |
| `/filter/off` | 停泵 | `{sent}` |
| `/filter/speed?n=1..20` | 设档位 | `{sent,gear}` |
| `/filter/feed?min=1..60` | 喂食暂停 N 分钟 | `{sent,minutes}` |
| `/filter/backble` | 从实体按键模式切回蓝牙控制 | `{sent}` |

状态字段含义：`gear` 0 表示停机；`model` 1=档位 2=档位+流量 3=喂食设置 5=喂食中 6=关 **7=自吸排气**；`err` 错误码。

## 2. 加到 `sensor.yaml`

```yaml
- platform: rest
  name: "滤桶档位"
  resource: "http://10.0.0.54/filter/state"
  method: GET
  value_template: '{{ value_json.gear }}'
  scan_interval: 30

- platform: rest
  name: "滤桶工作模式"
  resource: "http://10.0.0.54/filter/state"
  method: GET
  value_template: '{{ value_json.model }}'
  scan_interval: 30

- platform: rest
  name: "滤桶连接状态"
  resource: "http://10.0.0.54/filter/state"
  method: GET
  value_template: '{{ value_json.connected }}'
  scan_interval: 30
```

## 3. 加到 `rest_commands.yaml`

```yaml
filter_on:
  url: "http://10.0.0.54/filter/on"
  method: get
filter_off:
  url: "http://10.0.0.54/filter/off"
  method: get
filter_read:
  url: "http://10.0.0.54/filter/read"
  method: get
filter_speed_1:
  url: "http://10.0.0.54/filter/speed?n=1"
  method: get
# ... 2~20 各来一条，或者用下面的脚本方式
filter_feed_10:
  url: "http://10.0.0.54/filter/feed?min=10"
  method: get
filter_feed_20:
  url: "http://10.0.0.54/filter/feed?min=20"
  method: get
```

## 4. 加到 `template.yaml`

```yaml
- switch:
    - name: "滤桶"
      unique_id: netlea_filter_switch
      state: "{{ states('sensor.lu_tong_dang_wei')|int(0) > 0 }}"
      turn_on:
        - action: rest_command.filter_on
      turn_off:
        - action: rest_command.filter_off
      icon: mdi:water-pump

- select:
    - name: "滤桶档位"
      unique_id: netlea_filter_gear
      options: ["1","2","3","4","5","6","7","8","9","10",
                "11","12","13","14","15","16","17","18","19","20"]
      state: "{{ states('sensor.lu_tong_dang_wei') }}"
      select_option:
        - action: rest_command.filter_speed_{{ option }}

- button:
    - name: "滤桶喂食10分钟"
      unique_id: netlea_filter_feed10
      press:
        - action: rest_command.filter_feed_10
```

> `sensor.lu_tong_dang_wei` 是 HA 对「滤桶档位」自动生成的 entity_id，中文名会被拼音化。如果你的不一样，去「开发者工具 → 状态」里搜「滤桶」看实际 id。

## 5. 一个必须注意的自动化坑

**滤桶开机后要自吸排气（30 秒 ~ 15 分钟，`model=7`），这期间所有蓝牙控制会被设备拒绝。**

所以「开机 + 设档位」不能连着发，要等状态变成 `model=1` 再发：

```yaml
- alias: 换水后恢复滤桶
  triggers:
    - trigger: state
      entity_id: sensor.lu_tong_gong_zuo_mo_shi
      to: "1"
  actions:
    - action: rest_command.filter_speed_8
```

## 6. 单连接提醒

ESP32 连着滤桶时，手机小程序连不上，反之亦然。固件是「发完命令立刻断开」，所以：

- 你在家想用小程序，随时可以用，不会有"设备被占用"的报错
- 但 HA 里的状态是**缓存值**，不是实时的。想要最新状态就调一次 `/filter/read`
