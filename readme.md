# HackMatrix-优化版

<img src="images/header_img.png" width="800">

一个3D桌面环境 (也是一个游戏引擎！！！)

<a href="#">演示视频正在准备哦</a>

## 使用方式

### 在3D环境自由活动（就像在Minecraft）
使用鼠标转动鼠标

<img src="vids/lookAround.gif" width="420">

使用WASD移动 <br>
<img src="images/wasd.webp" width="100">

<img src="vids/move.gif" width="420">

### 打开窗口

HackMatrix 使用 `wofi`.

`d` 原本用于移动, 所以你得按下 `v` 来打开启动器

输入程序名然后按下 `<enter>` 启动应用程序

窗口将会在你面朝的方向启动

按下 `r` 来 [focus](#manually-focus-window) 在窗口上

<img src="vids/openWindow.gif" width="420">

### 手动锁定视角到窗口
看向窗口然后按下 `r`

如果按 `r` 没有反应，先确认准星正对窗口；仍不行时可以用 `Super + 1/2/3...` 直接锁定。


<img src="vids/focus.gif" width="420">

### 退出锁定
视角锁定到特定窗口时按下 `Super+e`

<img src="vids/exitWindow.gif" width="420">

### 关闭窗口
视角锁定到特定窗口然后按下 `Super+q`

### 拖动窗口
在自由视角下按住G可以任意拖动窗口位置，也可以按移动方向键移动窗口位置

### 调整窗口大小
在自由视角下准星对准窗口边缘按住鼠标左键就可以调整窗口大小

### 快捷键

为了便于使用和调试，这个桌面环境有很多特殊快捷键

`Super+<num>` 来定位窗口

<img src="vids/hotkey.gif" width="420">

### 退出 HackMatrix
在自由视角下按下 `<esc>`

退出时会先请求关闭所有窗口，并终止由 HackMatrix 启动的子应用。

### 屏幕截图

按下 `p` 可以把屏幕截图保存到 `<project_dir>/screenshots`
（我还在确定这个project_dir是啥）

### Hackmatrix 菜单
菜单默认隐藏。屏幕左侧边缘有一个竖直长条，中间带箭头。

处于自由视角时，按下 `f` 呼出鼠标（就像二游按下 Alt 呼出鼠标），然后点击左侧长条，菜单会从左侧滑出，约占屏幕宽度五分之一；再次点击长条或点面板右上角 `<<` 收起。

你可以用这个菜单查看/修改/增删模型（HackMatrix 的游戏引擎哲学喵~）、查看日志，或进入 Multiplayer 面板。

`System Config` 页可以调音量/静音、屏幕亮度，以及 FOV、可视距离、鼠标灵敏度、相机速度、窗口动画等 HackMatrix 参数。

找到 Entity Editor

See the [wiki page](https://github.com/collinalexbell/HackMatrix/wiki/Game-Engine) for more info about the game engine and how to use the editor.

这个菜单还有Multyplayer模式，功能大致是可以让其他使用hackmartix的用户可以加入其他人的环境（有点意思，原作者没有说明这个功能，但我发现这个只是能相互看到各自的头像，不能多窗口互联）

### 多人聊天

连接多人模式后，按下 `T` 打开右下角聊天栏：

- 输入文字后按 `<enter>` 发送；
- 按 `<esc>` 关闭；
- 玩家加入/离开、窗口控制授权也会以系统消息显示；
- 目前输入只支持 ASCII，中文输入仍受 fcitx5 问题影响。

## 编译 / 安装

完整的依赖安装、发行版命令、wlroots / Crow 配置、构建选项、测试、打包和专用服务器构建，见：

- **[docs/BUILDING.md](docs/BUILDING.md)**

最简流程：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./launch
```

不要用 `sudo` 运行；它需要当前用户的 Wayland / DRM 权限。

---

## 后续计划

| 功能 | 状态 | 说明 |
|------|------|------|
| 多人游戏 | 正在测试 | 连接性已支持，窗口共享控制功能在测试中 |
| 专用服务器 | 正在测试 | 正常运行，可以被连接，稳定性问题仍需测试 |
| 更好的交互 | 正在测试 | 增加准星操作，灵感来源于Minecraft的WaylandCraft MOD |
| 场景自定义便捷化 | 待办项 | 目的实现场景自由，让用户有一个属于自己的空间 |

**祝您体验愉快！**
