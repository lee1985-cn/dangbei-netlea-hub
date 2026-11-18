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


## 语音触发喂食（小爱同学）

> ⚠️ 本节是当时的写法。文末「2026-11 补记」已改成以「小爱训练回复（answers）」为准确信号——
> 直接抄下面的模板会踩「ASR 把整句拆坏」和「传感器两次写入」两个坑。

沿用「打开投影仪 / 打开电脑」那套写法：监听小爱对话传感器 + 关键词 + 40 秒时效窗口。

```yaml
- id: '1794318500001'
  alias: 鱼缸喂食
  triggers:
  - trigger: state
    entity_id:
    - sensor.xiaomi_lx06_ac65_conversation
    - sensor.xiaomi_lx04_5d57_conversation
  conditions:
  - condition: template
    value_template: >-
      {% set c = state_attr(trigger.entity_id, 'content') | string %}
      {% set msg_ts = as_timestamp(state_attr(trigger.entity_id, 'timestamp'), default=0) %}
      {{ ('鱼缸喂食' in c or '喂鱼' in c)
         and msg_ts > 0
         and (as_timestamp(now()) - msg_ts) < 40 }}
  - condition: state
    entity_id: switch.lu_tong
    state: ['on']
  actions:
  - action: script.turn_on
    target: {entity_id: script.netlea_feed_10}
  mode: single
```

「鱼缸换水」同构，关键词 `鱼缸换水` / `换水`，动作换成 `script.netlea_feed_30`。

要点：
- **必须做 40 秒时效判断**。对话传感器是"状态"实体，HA 重启或集成重连会重新写一次同样的
  content，没有时效窗口就会在重启后误触发。
- `switch.lu_tong` 必须是 `on` 才执行——泵本来就停着的时候不必再喂食。
- `mode: single`，重复喊只会排队一次。


## 语音触发喂食 / 换水（含 ASR 容错）

### 三个必须避开的坑

**坑 1：在 HA 网页 UI 里改模板会把它改坏**

用 UI 的自动化编辑器改别名或关键词后，模板里的换行会被序列化成字面的 `\n` 和 `\ `，
渲染时报 `TemplateSyntaxError: unexpected char '\'`。这条自动化从此永远不满足条件——
表现就是"第一次能用，之后怎么喊都没反应"。

**模板一律用 `>-` 块标量写**，不要用双引号单行：

```yaml
  conditions:
  - condition: template
    value_template: >-
      {% set c = state_attr(trigger.entity_id, 'content') | string %}
      ...
```

**坑 2：连续说同一句话，状态触发不会第二次触发**

对话传感器的 state 就是这句话本身。说两遍"大缸喂鱼"，第二次 state 文本没变，
`trigger: state` 不会触发。改成监听 `timestamp` 属性：

```yaml
  triggers:
  - trigger: state
    entity_id: [sensor.xiaomi_lx06_ac65_conversation, sensor.xiaomi_lx04_5d57_conversation]
    attribute: timestamp
```

**坑 3：小爱 ASR 会把「大缸」听成「大纲」**

实测历史里有 `大纲喂食` / `大纲喂鱼` / `大纲位于`。关键词必须放宽：

```jinja
{{ ('喂鱼' in c or '喂食' in c)
   and ('缸' in c or '大纲' in c)
   and '取消' not in c
   and '不喂' not in c
   and '完' not in c
   and msg_ts > 0
   and (as_timestamp(now()) - msg_ts) < 40 }}
```

换水同理：`'换水' in c and '取消' not in c and '完' not in c`。
排除 `完` 是为了让"换水完了"只触发取消、不触发 30 分钟喂食。

取消一条管全部：`'取消' in c or '不喂' in c or '换水完' in c or '换完水' in c`。

### 完整实体

| 实体 | 触发 | 动作 |
|---|---|---|
| `automation.yu_gang_wei_shi` | 含"喂鱼/喂食"且含"缸/大纲" | 停泵 10 分钟 |
| `automation.yu_gang_huan_shui` | 含"换水"，不含"取消/完" | 停泵 30 分钟 |
| `automation.qu_xiao_wei_shi` | 含"取消/不喂/换水完" | 恢复泵运行 |

三条都带 40 秒时效窗口（防 HA 重启重放旧内容误触发），喂食/换水还要求
`switch.lu_tong == on`（泵本来就停着不必再喂）。

### 测试方法

不用真喊，直接伪造传感器状态就能走完整链路：

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"state":"大缸喂鱼","attributes":{"content":"大缸喂鱼","timestamp":"<ISO时间>"}}' \
  http://10.0.0.15:8123/api/states/sensor.xiaomi_lx06_ac65_conversation
```

看 `automation.xxx` 的 `last_triggered` 是否变化，再看设备 `/filter/state` 的 `model`/`feed`。


## 语音触发词：用「60缸」而不是「大缸」

### 为什么换词

从 HA 历史里翻出 130 小时的对话记录，规律非常清楚：

| 说法 | 实际识别结果 | 出错情况 |
|---|---|---|
| 打开 60 缸灯 | `打开六零鱼缸灯` | **几十次，零错误** |
| 大缸喂鱼 | `大缸喂鱼` / `大纲喂鱼` / `大纲鲤鱼` | **经常错** |

`60` 转成「六零」极其稳定，而「大缸」几乎每次都可能变「大纲」，
「喂鱼」还会变「鲤鱼」。继续在「大缸」上打补丁只会越补越乱。

### 最终规则：以数字「六」为锚点

```jinja
{# 60缸喂食 #}
{{ (('六' in c) or ('60' in c) or ('陆' in c))
   and ('喂' in c or '鲤' in c or '食' in c)
   and '取消' not in c and '不' not in c and '完' not in c
   and msg_ts > 0
   and (as_timestamp(now()) - msg_ts) < 40 }}
```

```jinja
{# 60缸换水 #}
{{ (('六' in c) or ('60' in c) or ('陆' in c))
   and ('换' in c and '水' in c)
   and '取消' not in c and '完' not in c
   and msg_ts > 0
   and (as_timestamp(now()) - msg_ts) < 40 }}
```

```jinja
{# 取消喂食（不绑「六」，否则「不喂了」接不住） #}
{{ ('取消' in c or '不喂' in c)
   and ('喂' in c or '鲤' in c or '换' in c or '水' in c or '六' in c or '缸' in c)
   and msg_ts > 0
   and (as_timestamp(now()) - msg_ts) < 40 }}
```

**关键点：不再匹配「缸/纲/刚」**。因为锚点是「六」，缸字怎么写都不影响；
而「喂鱼」写成「鲤鱼」由 `'鲤' in c` 兜住。

### 验证矩阵（24 条，全过）

| 话术 | 喂食 | 换水 | 取消 |
|---|---|---|---|
| 60缸喂鱼 / 六零缸喂鱼 / 六十缸喂鱼 | ✅ | · | · |
| 六零鱼缸喂鱼 / 六零缸鲤鱼 / 六零鱼缸鲤食 | ✅ | · | · |
| 60缸喂食 / 六零缸换水 / 六十缸换水 | · | ✅ | · |
| 取消喂鱼 / 取消六零缸喂鱼 / 取消鲤鱼 | · | · | ✅ |
| 取消换水 / 不喂了 | · | · | ✅ |
| **打开六零鱼缸灯 / 关闭六零鱼缸灯** | · | · | · |
| 打开六零鱼缸灯并调亮 | · | · | · |
| 关闭餐厅灯 / 现在几点 / 三小时内天气 | · | · | · |
| **取消会议** | · | · | · |
| 打开电脑 | · | · | · |

最后几行是防误触发的关键：
- 「六零鱼缸灯」含「六」，但动作字里没有 喂/鲤/食，所以不触发喂食
- 「取消会议」含「取消」，但没有鱼缸相关字，所以不触发取消

### 换词的代价

说「大缸喂鱼」不再生效了。如果习惯改不过来，可以在小爱 App 的
「训练计划」里加一条同义词，让「大缸喂鱼」也输出「60缸喂鱼」。

---

## 2026-11 补记：ASR 会把整句拆坏，只靠 content 关键词必然失效

### 现象

小爱训练命中、口头回复也正常，但三条自动化一次都不触发。翻对话历史，同一条命令被拆成两种残句：

| 实际说 | 传感器 content | 缺了哪一半 |
|---|---|---|
| 六零缸喂鱼 | `六零缸位于`（喂鱼 wèiyú → 位于） | 动作词没了 |
| 六零缸喂食 | `刘灵刚喂食`（六零缸 → 刘灵刚） | 缸的锚点没了 |
| 取消喂鱼 | `取消位于` | 两边都没了 |

旧条件要求「六/60/陆」**且**「喂/鲤/食」同时成立，残句各缺一半，永远凑不齐。

### 根因二：对话传感器是「两次写入」

miot 的对话传感器说一句话会写两次状态：

1. 先写 `content` + `timestamp`，此时 **`answers` 是空列表 `[]`**
2. 零点几秒后再单独写 `answers`（小爱实际念出来的回复）

只监听 `attribute: timestamp` 时，条件在第一拍读到空 answers → 恒为 False。
trace 里能看到 `to_state.attributes.answers == []`，而 `from_state` 还留着上一句的回复，
极易误判成"属性没更新"。

### 正解：以「小爱训练回复」为准确信号

两个触发器都挂上，条件里以 `answers` 为主、`content` 兜底：

```yaml
triggers:
- trigger: state
  entity_id: [sensor.xiaomi_lx06_ac65_conversation, sensor.xiaomi_lx04_5d57_conversation]
  attribute: timestamp
- trigger: state
  entity_id: [sensor.xiaomi_lx06_ac65_conversation, sensor.xiaomi_lx04_5d57_conversation]
  attribute: answers
conditions:
- condition: template
  value_template: >-
    {% set c = state_attr(trigger.entity_id, 'content') | string %}
    {% set a = state_attr(trigger.entity_id, 'answers') | string %}
    {% set ts = as_timestamp(state_attr(trigger.entity_id, 'timestamp'), default=0) %}
    {{ ((('六' in c or '60' in c or '陆' in c) and ('喂' in c or '鲤' in c or '食' in c))
        or ('可以喂鱼' in a) or ('喂鱼了' in a))
       and '取消' not in c and '不' not in c and '完' not in c
       and ts > 0 and (as_timestamp(now()) - ts) < 40 }}
```

要点：

- **负向词只约束 `content`，不要约束 `answers`**。训练回复里常带"注意 10 分钟后会自动关闭"
  这类字，套上负向词会把条件否掉。
- `answers` 里的关键词直接照抄小爱训练里配的回复文本：喂食用 `可以喂鱼`，取消用 `水泵`，
  福特用 `已经启动`。改了训练词就要同步改这里。
- 第一拍（timestamp）条件失败没关系，第二拍（answers）会再判一次；`mode: single` 保证不会重复执行。

### 排障：看 trace，不要看当前状态

```
trace/list  → 看 run 的 last_step / script_execution
              failed_conditions = 触发成功、但被条件拦下
trace/get   → trigger/0[0].changed_variables.trigger.from_state / to_state.attributes
```

当前 `/api/states` 里的属性早被第二次写入覆盖，看不出问题；`last_triggered` 也不会因为
条件失败而变化，所以"没触发"和"触发了但条件拦下"必须靠 trace 区分。

### 安全测试：探针自动化

1. 建一条触发/条件与目标一致、动作只发 `persistent_notification` 的自动化（id 用 `9900000000001`）
2. 把三条真实自动化 `automation.turn_off`
3. 往**离线的** `sensor.xiaomi_lx04_5d57_conversation` 伪写两次状态（先 `answers=[]`，再填回复）
4. 看探针的 `last_triggered` 是否变化；测完删探针、恢复 `unavailable`、重新 `turn_on`

全程不碰滤桶硬件。

### 实测结论（2026-11-18）

| 自动化 | 触发时间 | 小爱训练回复 |
|---|---|---|
| 60缸喂食 | 21:11:58 | 好的，可以喂鱼了 |
| 60缸换水 | 21:17:03 | 好的，水泵将暂停30分钟 |
| 取消喂食 | 21:17:13 | 好的，重新打开水泵！ |

三条全部命中，ASR 残句（`六零缸位于` / `取消位于`）不再影响触发。
