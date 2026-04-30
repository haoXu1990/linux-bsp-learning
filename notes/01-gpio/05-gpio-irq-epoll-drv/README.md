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
  - 



大致理清楚了，应该有2条线

```shell

# 1. 硬中断
#在 kernel/irq/handle.c 中
  __handle_irq_event_percpu 
    __irq_wake_thread
      if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags)) 
会把当前唤醒的线程flag 设置为  IRQTF_RUNTHREAD，开始一直以为这个方法只是单纯设置直flag，后来才发现这个方法是判断当前
action->thread_flags 等于 IRQTF_RUNTHREAD 就往后面执行，意味着这里做了判断
当前线程是RUNNTHREAD 那就 return
当前线程不是 RUNTHREAD 那就继续往下执行唤醒线程
这就对应了我的问题部分答案，有人在判断当前thread_fn 是否在运行；

# 2. IRQ 线程
 在 kernel/irq/manage.c 中
  irq_thread
    irq_wait_for_interrupt
会把当前的 irq线程设置  set_current_state(TASK_INTERRUPTIBLE);

// irq_wait_for_interrupt 就是一个事件等待器，一直在等待 IRQTF_RUNTHREAD 
//  如果没有等待到就 schedule；
// 如果等待到了就执行循环体里面的内容
while (!irq_wait_for_interrupt(action)) {
		irqreturn_t action_ret;

		irq_thread_check_affinity(desc, action);

		action_ret = handler_fn(desc, action);
		if (action_ret == IRQ_WAKE_THREAD)
			irq_wake_secondary(desc, action); // 新问题，这里为什么又会有一个 secondary 来wake，并且和handler_fn的事件源是一样的？

		wake_threads_waitq(desc);
	}
	
```
