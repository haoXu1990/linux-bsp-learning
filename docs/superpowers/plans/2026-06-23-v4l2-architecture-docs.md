# V4L2 Architecture Documentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `notes/09-v4l2/` 下建立一套以 Linux 4.9 为主、从直观模型深入到内核调用链和驱动编写流程的 V4L2 架构文档集。

**Architecture:** 使用一个总入口和十篇单一职责专题文档。各文档共享“控制面、数据面、拓扑面”模型，并通过交叉链接串联 APP、V4L2 Core、videobuf2、controls、简单驱动、Media Controller、subdev 和 MIPI pipeline。

**Tech Stack:** Markdown、Mermaid、Linux 4.9 V4L2/videobuf2/media API、v4l2-utils。

---

### Task 1: 建立入口与全景模型

**Files:**
- Create: `notes/09-v4l2/README.md`
- Create: `notes/09-v4l2/architecture/00-V4L2学习路线与全景图.md`

- [ ] 写出文档目录、推荐阅读顺序和资料边界。
- [ ] 用总图解释控制面、数据面和拓扑面。
- [ ] 区分整体式 UVC/虚拟设备与 SoC MIPI pipeline。

### Task 2: 编写 APP 与 V4L2 Core

**Files:**
- Create: `notes/09-v4l2/architecture/01-APP接口与采集流程.md`
- Create: `notes/09-v4l2/architecture/02-V4L2-Core设备模型与ioctl调用链.md`

- [ ] 完整描述 MMAP streaming 流程和 buffer 所有权。
- [ ] 解释三类设备节点和单/多平面 API。
- [ ] 解释 `v4l2_device`、`video_device`、`v4l2_fh`、fops 和 ioctl ops。
- [ ] 给出 open/ioctl/poll/mmap 调用链。

### Task 3: 编写 videobuf2 与 controls

**Files:**
- Create: `notes/09-v4l2/architecture/03-videobuf2缓冲区管理.md`
- Create: `notes/09-v4l2/architecture/04-Controls参数控制框架.md`

- [ ] 解释 vb2 对象、三组 ops、状态机和内存后端。
- [ ] 展示 REQBUFS/QBUF/STREAMON/完成/DQBUF 的调用路径。
- [ ] 解释 control handler、control ops、标准 ID 和硬件映射。

### Task 4: 编写简单驱动指南

**Files:**
- Create: `notes/09-v4l2/architecture/05-简单V4L2设备驱动编写.md`

- [ ] 给出驱动对象布局、注册顺序和错误回滚。
- [ ] 给出 Linux 4.9 风格的核心代码骨架。
- [ ] 说明 active queue、完成一帧、streamoff 归还 buffer。

### Task 5: 编写 Media/Subdev 与 MIPI pipeline

**Files:**
- Create: `notes/09-v4l2/architecture/06-Media-Controller与V4L2-Subdev.md`
- Create: `notes/09-v4l2/architecture/07-MIPI摄像头Pipeline与驱动编写.md`

- [ ] 解释 entity/pad/link/pipeline 和 subdev ops。
- [ ] 解释异步绑定、设备树 endpoint 和聚合驱动。
- [ ] 分别给出 Sensor、CSI/ISP、Capture 和聚合驱动伪代码。
- [ ] 说明格式传播、启动顺序和错误回滚。

### Task 6: 汇总调用链与调试方法

**Files:**
- Create: `notes/09-v4l2/architecture/08-完整数据流与典型调用链.md`
- Create: `notes/09-v4l2/architecture/09-调试工具与常见问题.md`

- [ ] 用时序图集中展示格式、control、buffer、UVC、DMA 和 MIPI 流程。
- [ ] 给出 v4l2-ctl/media-ctl/v4l2-compliance/yavta 命令。
- [ ] 建立分层排查顺序和常见故障表。

### Task 7: 全集校验

**Files:**
- Verify: `notes/09-v4l2/README.md`
- Verify: `notes/09-v4l2/architecture/*.md`

- [ ] 检查十篇专题和入口文件均存在。
- [ ] 检查相对链接目标存在。
- [ ] 检查 Mermaid 围栏成对。
- [ ] 搜索并清除 TODO/TBD。
- [ ] 检查核心术语和 Linux 4.9 API 表述的一致性。
- [ ] 审查 Git diff，确保不包含用户其他工作区改动。
