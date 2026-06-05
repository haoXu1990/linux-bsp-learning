

## 1 USB 设备匹配过程
```shell
USB 设备插入
    ↓
USB core 枚举设备，读取 device/config/interface/endpoint 描述符
    ↓
USB core 遍历已注册的 usb_driver
    ↓
拿 interface 信息和 driver 的 id_table 比较
    ↓
匹配成功
    ↓
调用 driver->probe()
```
值得注意的是,USB 驱动匹配的是 interface, 不是我们理解的整个 device; 这里感觉是内核给我们减负了;

## 1.1 Probe
```shell
    // 1. 找 endpoint
    // 2. 分配 buffer
    // 3. 分配 urb
    // 4. 填充 urb
    // 5. 提交 urb
    // 6. 保存私有数据

```
