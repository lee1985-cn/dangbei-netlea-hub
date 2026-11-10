# 添加尼特利滤桶到 Home Assistant

固件跑起来、`http://<ESP32-IP>/filter/state` 能返回 JSON 之后，照下面做。

> 本文档的配置**已在 HA 2026.8.3 上实测跑通**，包含四个只有踩过才知道的坑。

---

## 0. 先让固件记住滤桶 MAC

```
http://10.0.0.54/filter/scan?s=8
```

扫到后固件会记住。也可以在固件里写死 `FILTER_MAC_DEFAULT`。

## 1. 接口

| 接口 | 作用 | 返回 |
|---|---|---|
| `/filter/state` | 读缓存（**不碰蓝牙**，适合高频轮询） | `{connected,gear,model,err,charge,voltage,mac}` |
| `/filter/readgatt` | 连上去读一次真实状态 | `{len,raw,gear,model,...}` |
| `/filter/read` | 写 `02 02` 到 EEE4 触发上报 | `{sent,gear,model}` |
| `/filter/on` `/filter/off` | 开/停泵 | `{sent}` |
| `/filter/speed?n=1..20` | 设档位 | `{sent,gear}` |
| `/filter/feed?min=1..60` | 喂食暂停 | `{sent,minutes}` |

**关键**：`/filter/state` 只读固件缓存，不建立蓝牙连接，所以可以每 30 秒轮询；真正读设备要用 `/filter/readgatt`，每次会占用 1~2 秒的 BLE 连接。

## 2. 一个 REST sensor 拿全部状态（rest.yaml）

不要为每个字段写一个 rest sensor（会 4 倍轮询），用 `json_attributes`：

```yaml
- resource: "http://10.0.0.54/filter/state"
  scan_interval: 30
  timeout: 20
  sensor:
    - name: "netlea_filter"
      unique_id: netlea_filter_state
      value_template: "{{ value_json.gear }}"
      icon: mdi:water-pump
      json_attributes:
        - gear
        - model
        - err
        - charge
        - voltage
        - connected
        - mac
```

## 3. 命令：用 shell_command + 外部脚本，不要用 rest_command 模板

### ⚠️ 坑 1：`rest_command` 的 url 模板传参不生效

文档说 `url` 支持模板，但实测：

```yaml
# 这样写，调用时传 data: {n: "9"} —— 实测无效，滤桶档位没变
netlea_speed:
  url: "http://10.0.0.54/filter/speed?n={{ n }}"
```

### ⚠️ 坑 2：action 名字不能模板化

`action: "rest_command.netlea_speed_{{ option }}"` 从 2024.10 起被禁止，整个 `select` 块会被**静默丢弃**（不报错，实体就是不出现）。

### ✅ 可行方案：shell_command + 外部脚本

`/config/netlea_cmd.sh`：

```sh
#!/bin/sh
url="$1"
i=1
r=""
while [ "$i" -le 4 ]; do
  r=$(curl -s -m 20 "$url")
  case "$r" in
    *'"sent":true'*) echo "$r"; exit 0 ;;
    *'"len":'*)      echo "$r"; exit 0 ;;
  esac
  i=$((i+1))
  sleep 2
done
echo "FAILED: $r"
exit 1
```

`configuration.yaml`：

```yaml
shell_command:
  netlea_speed_cmd: '/bin/sh /config/netlea_cmd.sh "http://10.0.0.54/filter/speed?n={{ n }}"'
  netlea_on_cmd: '/bin/sh /config/netlea_cmd.sh "http://10.0.0.54/filter/on"'
  netlea_off_cmd: '/bin/sh /config/netlea_cmd.sh "http://10.0.0.54/filter/off"'
  netlea_feed_cmd: '/bin/sh /config/netlea_cmd.sh "http://10.0.0.54/filter/feed?min={{ min }}"'
  netlea_read_cmd: '/bin/sh /config/netlea_cmd.sh "http://10.0.0.54/filter/readgatt"'
```

### ⚠️ 坑 3：YAML 单引号字符串里不能出现裸单引号

想在一行里写 `case "$r" in *'"sent":true'*)` 会**直接把 YAML 打断**，HA 启动进入 safe mode（只剩十几个实体）。所有 shell 逻辑放 `.sh` 文件里，YAML 只写路径。

### ⚠️ 坑 4：命令有概率第一次不生效

滤桶是单连接设备，快速连断时 `ble_gap_connect` 会失败。实测首次成功率约 50%。上面的重试脚本（4 次，间隔 2 秒）实测 **4/4 成功**。

## 4. 实体（template.yaml）

### ⚠️ 坑 5：`options` 必须是模板字符串，不能是 YAML 列表

```yaml
# ❌ 报错：template value should be a string for 'select->0->options'
options: ["1","2","3"]

# ✅
options: "{{ range(1, 21) | map('string') | list }}"
```

完整实体：

```yaml
- switch:
    - name: "滤桶"
      unique_id: netlea_filter_switch
      state: "{{ (state_attr('sensor.netlea_filter','gear')|int(0)) > 0 }}"
      turn_on:
        - action: shell_command.netlea_on_cmd
      turn_off:
        - action: shell_command.netlea_off_cmd
      icon: mdi:water-pump

- select:
    - name: "滤桶档位"
      unique_id: netlea_filter_gear
      options: "{{ range(1, 21) | map('string') | list }}"
      state: >-
        {% set g = state_attr('sensor.netlea_filter','gear')|int(0) %}
        {{ g if 1 <= g <= 20 else 1 }}
      select_option:
        - action: shell_command.netlea_speed_cmd
          data:
            n: "{{ option }}"
      icon: mdi:speedometer

- button:
    - name: "滤桶喂食10分钟"
      press:
        - action: shell_command.netlea_feed_cmd
          data:
            min: "10"
```

## 5. 定时刷新（automations.yaml）

固件重启后缓存是空的（`gear=-1`），要靠一次真实读取填充。加个定时任务：

```yaml
- id: netlea_filter_auto_refresh
  alias: 滤桶状态定时刷新
  trigger:
    - platform: time_pattern
      minutes: "/5"
  action:
    - action: shell_command.netlea_read_cmd
  mode: single
```

## 6. 自吸排气期不能控

滤桶开机后要排气 30 秒 ~ 15 分钟（`model=7`），期间设备拒绝蓝牙命令。自动化里"开机 + 设档位"要分开，等 `model` 变成 1 再设档。

## 7. 单连接提醒

ESP32 连着滤桶时手机小程序连不上。固件是"命令后保持 20 秒、空闲自动断开"，所以你在家随时能用小程序，代价是状态有最多 30 秒延迟。


## 喂食开关（点一下喂 / 再点取消）

小米鱼缸那种"点一下喂食、再点一下取消"的交互，靠一个分支脚本实现：

```yaml
netlea_feed_toggle:
  alias: 滤桶喂食切换
  mode: single
  sequence:
    - choose:
        - conditions:
            - condition: state
              entity_id: timer.netlea_feed
              state: active
          sequence:
            - action: script.turn_on
              target: {entity_id: script.netlea_feed_cancel}
      default:
        - action: script.turn_on
          target: {entity_id: script.netlea_feed_10}
```

取消脚本必须**先等 25 秒**（设备保护窗口）**再** `timer.cancel`，顺序反了会让卡片提前显示"已结束"但泵还没恢复：

```yaml
netlea_feed_cancel:
  mode: single
  sequence:
    - delay: {seconds: 25}          # 等过保护窗口，卡片显示"取消中…"
    - action: timer.cancel
      target: {entity_id: timer.netlea_feed}
    - repeat:                        # 重发当前档位（=取消喂食）
        count: 3
        sequence:
          - action: shell_command.netlea_speed_cmd
            data: {n: "{{ states('sensor.lu_tong_dang_wei_ji_yi') | int(8) }}"}
          - delay: {seconds: 4}
    - delay: {seconds: 2}
    - action: shell_command.netlea_read_cmd   # 收尾回读，档位/模式立刻恢复
```

卡片用 `script.netlea_feed_cancel` 的 `on` 状态显示"取消中…"，
因为这段时间 timer 还是 active（泵确实还没转起来），显示与实际一致。


## 开关实体状态判断（必读）

```yaml
- switch:
    - name: "滤桶"
      unique_id: netlea_filter_switch
      # model: 1/2=运行 3=喂食 6=关闭。gear 关泵时仍保留上次档位，不能单看 gear。
      state: >-
        {% set m = state_attr('sensor.netlea_filter','model')|int(0) %}
        {% set g = state_attr('sensor.netlea_filter','gear')|int(0) %}
        {{ (m in [1,2,3]) and (g > 0 or m == 3) }}
      turn_on:
        - action: shell_command.netlea_on_cmd
        - action: homeassistant.update_entity
          target: {entity_id: sensor.netlea_filter}
      turn_off:
        - action: shell_command.netlea_off_cmd
        - action: homeassistant.update_entity
          target: {entity_id: sensor.netlea_filter}
```

`turn_on` / `turn_off` 后面必须跟一次 `homeassistant.update_entity`，
否则 REST 传感器要等 30 秒轮询，卡片图标会滞后。

## 长按选喂食时长

button-card 的 `hold_action: more-info` 指向一个 template select，
下拉选 10/20/30 分钟就触发对应脚本：

```yaml
- select:
    - name: "滤桶喂食时长"
      unique_id: netlea_feed_duration
      # options 必须是模板字符串；写成 YAML 列表会被静默丢弃
      options: "{{ ['选时长','10分钟','20分钟','30分钟'] }}"
      # state 恒为"选时长"，所以每次下拉选同一个时长都算变化，一定会触发
      state: "选时长"
      select_option:
        - choose:
            - conditions: "{{ option == '10分钟' }}"
              sequence:
                - action: script.turn_on
                  target: {entity_id: script.netlea_feed_10}
            # ... 20/30 同理
```
