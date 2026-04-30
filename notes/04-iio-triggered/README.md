# DHT11 IIO

## 源码阅读




## 使用
```shell
这个顺序很关键
# 新建一个IIO triger， 记住这里是独立的还没关联
# cd /sys/bus/iio/devices/iio_sysfs_trigger
# echo 0 > add_trigger


# cd  /sys/bus/iio/devices/iio:device1
# echo 0 > buffer/enable
# echo 1 > scan_elements/in_temp_en
# echo 1 > scan_elements/in_humidityrelative_en
# echo 1 > scan_elements/in_timestamp_en
# echo 8 > buffer/length
#
# echo sysfstrig0 > trigger/current_trigger
#
#
# echo 1 > buffer/enable
#
#
# echo 1 > /sys/bus/iio/devices/trigger0/trigger_now
#
# hexdump -C -n 16 /dev/iio:device1

```
### devm_iio_device_register 主线
```shell
相关文件：
/driver/iio/industrialio-core.c
/driver/iio/industrialio-buffer.c

# 重点跟了一下函数
iio_device_register_debugfs(indio_dev);
没细跟，只是开了一个debufs 和我们的主题没关系

iio_device_register_sysfs(indio_dev);
创建普通 sysfs 属性，比如 name、in_temp_input、in_humidityrelative_input。

iio_device_register_eventset(indio_dev);
这个没细看，可能是有那种边沿触发的时间驱动；


iio_buffer_alloc_sysfs_and_mask(indio_dev);
创建 buffer/ 和 scan_elements/，并分配 scan_mask。


cdev_init(&indio_dev->chrdev, &iio_buffer_fileops);
cdev_device_add(&indio_dev->chrdev, &indio_dev->dev);
注册 /dev/iio:deviceX，用来读取 buffer 二进制数据。
```

### Trigger
``` shell

1. trigger0/trigger_now 从哪里来
echo 0 > iio_sysfs_trigger/add_trigger
  -> 创建 sysfstrig0
  -> 初始化 sysfs_trig->work  这里有个cpu 队列 work queue,异步执行回调
  -> trigger0/trigger_now
2. current_trigger 怎么把 iio:device1 绑定到 sysfstrig0
echo sysfstrig0 > iio:device1/trigger/current_trigger
  -> iio_trigger_write_current()
  -> dev_to_iio_dev(dev) 得到 iio:device1 对应的 indio_dev
  -> iio_trigger_acquire_by_name("sysfstrig0") 得到 struct iio_trigger
  -> indio_dev->trig = trig
   
3. sysfs trigger 写 trigger_now 后怎么进入 irq_work
iio-trig-sysfs.c
  ->iio_sysfs_trigger_poll
    ->irq_work_queue(&sysfs_trig->work);
这里并没有直接调用驱动的回调函数，只是把 sysfs trigger 的 work 排队；

4. buffer/enable 才是把 pollfunc 挂到 trigger 的关键

5. trigger 触发后最终会调用你的 dht11_trigger_handler


```
