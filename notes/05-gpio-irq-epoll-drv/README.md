# GPIO IRQ Driver Epoll 
GPIO 驱动练习, 主要目的练习中断，阻塞，epoll， 数据缓冲，按键去抖
APP：只读取按键事件
目的：通过 GPIO 按键 KEY1（PD7），点亮/控制 LED 灯（PD21）。

## 需求
- **IRQ（中断）**：KEY1（PD7）产生 GPIO 中断后，驱动第一时间得到通知。
- **内核动作**：驱动在内核中完成 LED（PD21）的点亮/翻转等动作（LED 控制在内核完成）。
- **事件缓冲**：驱动把“按键事件”写入事件环形缓冲。
- **用户态读取**：APP 只读按键事件（不负责控制 LED）：
- **阻塞 read**：无事件则睡眠等待；有事件立即返回。
- **poll/epoll**：APP 把设备 fd 加入 `epoll`，等待“可读”后再 `read()` 取事件。

## 关键实现要点（把 IRQ / 阻塞 / epoll 串起来）
- 核心粘合剂是 **waitqueue**：
  - **中断到来**：事件入队 → `wake_up_interruptible()` 唤醒读者
  - **read()**：无事件 → `wait_event_interruptible()` 睡眠；有事件 → `copy_to_user()` 返回
  - **poll()**：`poll_wait()` 关联同一 waitqueue；有事件返回 `POLLIN | POLLRDNORM`

## LED 控制约定
- 驱动在初始化时把 LED 设置到一个**已知初始状态**。
- 翻转 LED 时**推荐由驱动维护软件状态**（例如 `led_state`），每次按键事件到来时：
  - `led_state = !led_state`
  - 按 `led_state` 设置 LED GPIO 输出电平
