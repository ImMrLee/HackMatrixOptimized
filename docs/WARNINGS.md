# 编译告警清单

- 生成时间: 2026-11-22
- 命令: `cmake -DCMAKE_BUILD_TYPE=Release && make -j`（`-Wall -Wextra`，含测试目标）
- 总数: **261**

## 按类型

```
    153 [-Wreorder]
     24 [-Wmissing-field-initializers]
     18 [-Wunused-variable]
     17 [-Wunused-parameter]
     16 [-Wsign-compare]
      9 [-Wdeprecated-enum-enum-conversion]
      7 [-Wswitch]
      5 [-Wunused-function]
      1 [-Wmisleading-indentation]
      1 [-Wignored-attributes]
```

## 真 bug 级别

```
include/stb/stb_image.h:5166:56: warning: writing 1 byte into a region of size 0 [-Wstringop-overflow=]
src/miniz.c:3841:5: warning: this ‘if’ clause does not guard... [-Wmisleading-indentation]
```
> 剩余 2 处均在第三方 vendored 代码 (`stb_image.h`, `miniz.c`)，非本项目代码。

## 按文件 Top 15

```
     96 include/SQLPersisterImpl.h
     27 include/controls.h
     24 include/stb/stb_image_write.h
     17 src/world.cpp
     11 src/wayland/wlr_compositor.cpp
      9 src/renderer.cpp
      9 include/enet/enet.h
      6 src/mesher.cpp
      5 src/wayland/WaylandApp.cpp
      4 src/MultiPlayer/Client.cpp
      4 include/renderer.h
      4 include/api.h
      3 src/WindowManager/WindowManager.cpp
      3 src/engineGui.cpp
      3 src/controls.cpp
```
