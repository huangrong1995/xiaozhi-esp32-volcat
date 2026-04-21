# ESP-VoCat 喵伴

## 简介

<div align="center">
    <a href="https://oshwhub.com/esp-college/echoear"><b> 立创开源平台 </b></a>
</div>

ESP-VoCat 喵伴是一款智能 AI 开发套件，搭载 ESP32-S3-WROOM-1 模组，1.85 寸 QSPI 圆形触摸屏，双麦阵列，支持离线语音唤醒与声源定位算法。硬件详情等可查看[立创开源项目](https://oshwhub.com/esp-college/echoear)。

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

### 基本配置
- `Xiaozhi Assistant` → `Board Type` → 选择 `Espressif ESP-VoCat`

### UI风格选择

ESP-VoCat 支持多种不同的 UI 显示风格，通过 menuconfig 配置选择：

- `Xiaozhi Assistant` → `Select display style` → 选择显示风格

#### 可选风格

##### 表情动画风格 (Emote animation style) - 推荐
- **配置选项**: `USE_EMOTE_MESSAGE_STYLE`
- **特点**: 使用自定义的 `EmoteDisplay` 表情显示系统
- **功能**: 支持丰富的表情动画、眼睛动画、状态图标显示
- **适用**: 智能助手场景，提供更生动的人机交互体验
- **类**: `emote::EmoteDisplay`

**⚠️ 重要**: 选择此风格需要额外配置自定义资源文件：
1. `Xiaozhi Assistant` → `Flash Assets` → 选择 `Flash Custom Assets`
2. `Xiaozhi Assistant` → `Custom Assets File` → 填入资源文件地址：
   ```
   https://dl.espressif.com/AE/wn9_nihaoxiaozhi_tts-font_puhui_common_20_4-echoear.bin
   ```

##### 默认消息风格 (Enable default message style)
- **配置选项**: `USE_DEFAULT_MESSAGE_STYLE` (默认)
- **特点**: 使用标准的消息显示界面
- **功能**: 传统的文本和图标显示界面
- **适用**: 标准的对话场景
- **类**: `SpiLcdDisplay`

##### 微信消息风格 (Enable WeChat Message Style)
- **配置选项**: `USE_WECHAT_MESSAGE_STYLE`
- **特点**: 仿微信聊天界面风格
- **功能**: 类似微信的消息气泡显示
- **适用**: 喜欢微信风格的用户
- **类**: `SpiLcdDisplay`

> **说明**: ESP-VoCat 喵伴使用16MB Flash，需要使用专门的分区表配置来合理分配存储空间给应用程序、OTA更新、资源文件等。

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

将 ESP-VoCat 喵伴连接至电脑，**注意打开电源**，并运行：

```bash
idf.py flash
```

## 触摸交互功能

### 触摸手势

| 手势 | 触发条件 | 反馈效果 |
|------|----------|----------|
| 单击 | 触碰后 < 350ms 松开 | 开心表情 (2秒) + 音效 |
| 双击 | 350ms 内连续触碰两次 | 惊讶表情 (2.5秒) + 双音效 |
| 长按 | 按住 > 1.2秒后松开 | 切换静音模式 + 震动音效 |
| 三击 | 500ms 内连续触碰三次 | 进入情绪学习模式 |

### 情绪学习模式

三击进入情绪学习模式，用于教儿童识别不同表情：

**使用方法：**
1. 在待机状态下，快速连续触碰三次进入学习模式
2. 屏幕显示当前表情（开心/伤心/生气/惊讶/困惑/困了/喜欢/平静）
3. 每次单击切换到下一个表情
4. 长按退出学习模式

**学习的表情：**
- 开心 (happy)
- 伤心 (sad)
- 生气 (angry)
- 惊讶 (shocked)
- 困惑 (confused)
- 困了 (sleepy)
- 喜欢 (loving)
- 平静 (neutral)

## 定时提醒功能

设备会在以下时间自动播放提醒：

| 时间 | 提醒内容 | 显示表情 |
|------|----------|----------|
| 08:00 | 该起床了~ | 开心 |
| 12:00 | 该吃午饭了~ | 开心 |
| 14:00 | 该睡午觉了~ | 困了 |
| 18:00 | 该吃晚饭了~ | 开心 |
| 21:00 | 该睡觉了~ | 困了 |

提醒触发时：播放提示音两次 + 显示对应表情 5 秒。

## 待机表情动画

设备在待机状态下，每 30 秒自动切换一次表情：

**表情循环：** 开心 → 困惑 → 生气 → 惊讶 → 开心

切换到倾听或说话状态时会暂停动画。