#include <jni.h>
#include <string>
#include <dlfcn.h>
#include <android/log.h>
#include <linux/hw_breakpoint.h>
#include "perf.h"
#define LOG_TAG "PerfBP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
// 声明区

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved);
__attribute__((noinline)) int add(uint32_t n1, uint32_t n2);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{

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


extern "C"
JNIEXPORT void JNICALL
Java_com_aespa_perf_1event_MainActivity_getRegs(JNIEnv *env, jobject thiz) {
    LOGD("call");
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
}
static int karina = 0;
void setVar(){
    karina = 0x20000411;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_aespa_perf_1event_MainActivity_getRegs_1Mem(JNIEnv *env, jobject thiz) {
    LOGD("call");
    LOGD("We're here to begin.");
    start_perf((uint64_t)&karina, HW_BREAKPOINT_W);
    setVar();
   return;
}