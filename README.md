# 引言
这是一个Android Studio项目，使用系统调用来实现Android程序的硬件断点。不像rwProcMem33那样需要修改内核重新编译之类的复杂操作。
```C
static long perf_event_open(
        struct perf_event_attr *attr,
        pid_t pid,
        int cpu,
        int group_fd,
        unsigned long flags)
{
    return syscall(
            __NR_perf_event_open,
            attr,
            pid,
            cpu,
            group_fd,
            flags
    );
}
```
参数：
```C
long perf_event_open(
        struct perf_event_attr *attr,
        pid_t pid,
        int cpu,
        int group_fd,
        unsigned long flags);
```
# 使用
手机得是root的，执行以下命令行。
```Shell
echo -1 > /proc/sys/kernel/perf_event_paranoid
setenforce 0
```
还原：
```Shell
echo 3 > /proc/sys/kernel/perf_event_paranoid
setenforce 1
```
## 视使用场景修改代码
一是代码执行断点，二是内存访写断点。
### (1) 执行断点
`JNI_OnLoad`代码示例，位于`native-lib.cpp`：
```C
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    LOGD("We're here to begin.");
    // HW_BREAKPOINT_X来自 #include <linux/hw_breakpoint.h>
    /*
     * 内存断点：
     * 更改结构体属性即可
        attr.bp_type = HW_BREAKPOINT_W;   // 或 HW_BREAKPOINT_R / HW_BREAKPOINT_RW
        attr.bp_addr = target_data_addr;  // 要监控的内存地址
        attr.bp_len  = HW_BREAKPOINT_LEN_4; // 操作多少个字节？
     * */
    start_perf((uint64_t)add, HW_BREAKPOINT_X);
    // Exec. Test: Call `add` for 3 times.
    add(333,444);
    add(111,222);
    add(33333,444);
    // Memory read test
    return JNI_VERSION_1_6;
}

// DO NOT INLINE A FUNCTION.
// To find the right address of `add`.
__attribute__((noinline))
int add(uint32_t n1, uint32_t n2)
{
    volatile uint32_t a = n1;
    volatile uint32_t b = n2;
    return a + b;
}
```
### (2) 内存访写断点
定义一个全局变量：
```C
static int karina = 0;
```
写个函数来修改它的值：
```C
void setVar(){
    karina = 0x20000411;
}
```
再监视：
```C
extern "C"
JNIEXPORT void JNICALL
Java_com_aespa_perf_1event_MainActivity_getRegs_1Mem(JNIEnv *env, jobject thiz) {
// 这是jni函数，可以放在按钮点击事件里调用。
    LOGD("call");
    LOGD("We're here to begin.");
    start_perf((uint64_t)&karina, HW_BREAKPOINT_W);
    setVar();
   return;
}
```
# 效果图
## 执行断点




