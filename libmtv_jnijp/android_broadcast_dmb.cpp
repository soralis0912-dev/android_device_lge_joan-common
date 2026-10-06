/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "DmbJP"

#include <android/file_descriptor_jni.h>
#include <android_runtime/AndroidRuntime.h>
#include <android_runtime/android_view_Surface.h>
#include <binder/IServiceManager.h>
#include <gui/Surface.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <utils/Mutex.h>

#include <memory>
#include <unordered_map>

#include "IDmb.h"

/*
 * The native side of com.lge.broadcast.jfullseg.Dmb.
 *
 * This replaces the stock libmtv_jnijp.lge.so, which cannot run here: its
 * proxies put `Parcel data, reply;` on the stack sized for android 9, where
 * sizeof(Parcel) was 52 bytes rather than the current 60, so the second one
 * runs over the stack canary and the process aborts on the first binder call.
 * The marshalling lives inside the blob, so there is nothing to shim - see
 * joan-rs/DMB_PROTOCOL.md.
 */

namespace android {

namespace {

struct Fields {
    jclass clazz;             // global ref to com.lge.broadcast.jfullseg.Dmb
    jmethodID postEvent;      // postEventFromNative(Object, int, int, int, Object)
    jfieldID context;         // mNativeContext
    jfieldID surface;         // mSurface
    // The fourteen ints select() sends, in the order they go into the parcel.
    jfieldID select[14];
};

Fields gFields;

const char* const kSelectFields[] = {
        "mOpMode",   "mFreq",     "mServiceId", "mNetId",    "mTransportId",
        "mOriNetId", "mChannelId", "mServiceType", "mPmtPid", "mVideoPid",
        "mAudioPid", "mEmmPid",   "mEcmPid",    "mServiceId1Seg",
};

/*
 * The IDmbClient the service calls back on. It keeps the weak reference java
 * handed us at native_setup and turns every callback into the single
 * postEventFromNative() the java side dispatches from.
 */
class JNIDmbClient : public BnDmbClient {
public:
    JNIDmbClient(JNIEnv* env, jobject weakThiz) : mWeakThiz(env->NewGlobalRef(weakThiz)) {}

    /*
     * msg is LG's own message id, not the java callback number: the stock
     * library switches on its high byte to pick which of five callbacks to
     * post, and only then does java see a what of 0..4. Sending msg through
     * unchanged just makes the handler log "Unknown message type 2073".
     *
     * The signal and event paths pass the message id itself as arg1 and no
     * arg2; video and audio pass the two extras straight through.
     */
    void notifyCallback(int msg, int ext1, int ext2) override {
        JNIEnv* env = AndroidRuntime::getJNIEnv();
        if (env == nullptr) return;

        int what, arg1, arg2;
        switch (msg & 0xff00) {
            case 0x0100:
                what = kSignalCallback;
                arg1 = msg;
                arg2 = 0;
                break;
            case 0x0200:
                what = kVideoCallback;
                arg1 = ext1;
                arg2 = ext2;
                break;
            case 0x0400:
                what = kAudioCallback;
                arg1 = ext1;
                arg2 = ext2;
                break;
            case 0x0800:
            case 0x0c00:
                what = kEventCallback;
                // The one message the stock library rewrites on the way out.
                arg1 = (msg == 0x0c17) ? -32 : msg;
                arg2 = 0;
                break;
            default:
                ALOGW("notifyCallback: no mapping for msg=%d (%#x)", msg, msg);
                return;
        }

        ALOGV("notifyCallback msg=%#x -> what=%d arg1=%d arg2=%d", msg, what, arg1, arg2);
        env->CallStaticVoidMethod(gFields.clazz, gFields.postEvent, mWeakThiz, what, arg1, arg2,
                                  nullptr);
        checkException(env);
    }

    void dataCallback(int msg, int ext1, const sp<IMemory>& mem) override {
        JNIEnv* env = AndroidRuntime::getJNIEnv();
        if (env == nullptr) return;

        jbyteArray array = nullptr;
        if (mem != nullptr && mem->size() > 0) {
            array = env->NewByteArray(mem->size());
            if (array != nullptr) {
                env->SetByteArrayRegion(array, 0, mem->size(),
                                        static_cast<const jbyte*>(mem->unsecurePointer()));
            }
        }
        // The data path always reports itself as DATA_CALLBACK; what the stock
        // library passed as msg becomes arg1 and ext1 becomes arg2.
        env->CallStaticVoidMethod(gFields.clazz, gFields.postEvent, mWeakThiz, kDataCallback, msg,
                                  ext1, array);
        checkException(env);
        if (array != nullptr) env->DeleteLocalRef(array);
    }

    void release(JNIEnv* env) {
        if (mWeakThiz != nullptr) {
            env->DeleteGlobalRef(mWeakThiz);
            mWeakThiz = nullptr;
        }
    }

protected:
    ~JNIDmbClient() override {
        if (mWeakThiz != nullptr) {
            JNIEnv* env = AndroidRuntime::getJNIEnv();
            if (env != nullptr) env->DeleteGlobalRef(mWeakThiz);
        }
    }

private:
    static void checkException(JNIEnv* env) {
        if (env->ExceptionCheck()) {
            ALOGW("exception thrown from a Dmb callback");
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }

    jobject mWeakThiz;
};

// What a Dmb object is connected to.
struct DmbContext {
    sp<IDmb> dmb;
    sp<JNIDmbClient> client;
};

/*
 * mNativeContext is a java int, so it cannot hold a pointer in a 64-bit
 * process. It holds a handle into this table instead - 0 means not connected -
 * and the table hands out shared references, so an entry point that is already
 * running keeps its context alive across a concurrent native_release.
 */
Mutex gLock;
std::unordered_map<jint, std::shared_ptr<DmbContext>> gContexts;
jint gNextHandle = 1;

std::shared_ptr<DmbContext> contextOf(JNIEnv* env, jobject thiz) {
    Mutex::Autolock lock(gLock);
    auto it = gContexts.find(env->GetIntField(thiz, gFields.context));
    return it == gContexts.end() ? nullptr : it->second;
}

/*
 * Every entry point needs the IDmb, and calling any of them before native_setup
 * (or after native_release) is a programming error on the java side rather than
 * something to crash on, so it throws instead.
 */
sp<IDmb> dmbOf(JNIEnv* env, jobject thiz) {
    std::shared_ptr<DmbContext> ctx = contextOf(env, thiz);
    if (ctx == nullptr || ctx->dmb == nullptr) {
        jniThrowException(env, "java/lang/IllegalStateException", "Dmb is not connected");
        return nullptr;
    }
    return ctx->dmb;
}

// --- native methods ------------------------------------------------------

void dmb_native_setup(JNIEnv* env, jobject thiz, jobject weakThiz) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->checkService(String16("broadcast.dmb"));
    if (binder == nullptr) {
        ALOGE("broadcast.dmb is not published");
        jniThrowException(env, "java/lang/RuntimeException", "no DmbService");
        return;
    }

    sp<IDmbService> service = interface_cast<IDmbService>(binder);
    sp<JNIDmbClient> client = new JNIDmbClient(env, weakThiz);
    sp<IDmb> dmb = service->connect(client);
    if (dmb == nullptr) {
        ALOGE("DmbService::connect returned nothing");
        jniThrowException(env, "java/lang/RuntimeException", "DmbService connect failed");
        return;
    }

    auto ctx = std::make_shared<DmbContext>();
    ctx->dmb = dmb;
    ctx->client = client;

    Mutex::Autolock lock(gLock);
    jint handle = gNextHandle;
    while (handle == 0 || gContexts.count(handle)) handle++;
    gNextHandle = handle + 1;
    gContexts[handle] = ctx;
    env->SetIntField(thiz, gFields.context, handle);
    ALOGI("connected to broadcast.dmb");
}

void dmb_release(JNIEnv* env, jobject thiz) {
    std::shared_ptr<DmbContext> ctx;
    {
        Mutex::Autolock lock(gLock);
        auto it = gContexts.find(env->GetIntField(thiz, gFields.context));
        if (it != gContexts.end()) {
            ctx = std::move(it->second);
            gContexts.erase(it);
        }
        env->SetIntField(thiz, gFields.context, 0);
    }
    if (ctx == nullptr) return;
    if (ctx->dmb != nullptr) ctx->dmb->disconnect();
    if (ctx->client != nullptr) ctx->client->release(env);
}

void dmb_init(JNIEnv* env, jobject thiz, jint mode, jint arg) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->init(mode, arg);
}

void dmb_exit(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->exit();
}

void dmb_find(JNIEnv* env, jobject thiz, jint mode) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->find(mode);
}

void dmb_select(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb == nullptr) return;
    // native_select() takes no arguments: the tuning parameters are fields on
    // the java object, filled in before the call.
    SelectParam p;
    int32_t* out = &p.opMode;
    for (size_t i = 0; i < 14; i++) {
        out[i] = env->GetIntField(thiz, gFields.select[i]);
    }
    dmb->select(p);
}

void dmb_play_v3(JNIEnv* env, jobject thiz, jint mode, jstring path, jlong position, jlong arg1,
                 jlong arg2) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb == nullptr) return;
    const char* cpath = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    dmb->play(mode, cpath, position, static_cast<long>(arg1), static_cast<long>(arg2));
    if (path != nullptr) env->ReleaseStringUTFChars(path, cpath);
}

void dmb_stopPlay(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->stopPlay();
}

jint dmb_record(JNIEnv* env, jobject thiz, jint mode, jstring path, jlong duration) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb == nullptr) return -1;
    const char* cpath = path != nullptr ? env->GetStringUTFChars(path, nullptr) : "";
    jint result = dmb->record(mode, cpath, duration);
    if (path != nullptr) env->ReleaseStringUTFChars(path, cpath);
    return result;
}

jint dmb_record_fd(JNIEnv* env, jobject thiz, jint mode, jobject fileDescriptor, jlong duration) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb == nullptr) return -1;
    int fd = AFileDescriptor_getFd(env, fileDescriptor);
    if (fd < 0) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "bad FileDescriptor");
        return -1;
    }
    return dmb->record(mode, fd, duration);
}

void dmb_cancelRecord(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->cancelRecord();
}

void dmb_stopRecord(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->stopRecord();
}

void dmb_data(JNIEnv* env, jobject thiz, jint type, jint arg) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->setDataService(type, arg);
}

jint dmb_getDuration(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->getDuration() : 0;
}

void dmb_setVideoDimension(JNIEnv* env, jobject thiz, jint x, jint y, jint width, jint height,
                           jint rotation, jobject jsurface) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb == nullptr) return;

    sp<IGraphicBufferProducer> bufferProducer;
    if (jsurface != nullptr) {
        sp<Surface> surface = android_view_Surface_getSurface(env, jsurface);
        if (surface != nullptr) bufferProducer = surface->getIGraphicBufferProducer();
    }
    dmb->setVideoDimension(x, y, width, height, rotation, bufferProducer);
}

void dmb_setAudio(JNIEnv* env, jobject thiz, jint a, jint b, jint c) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->setAudio(a, b, c);
}

jint dmb_setMuteStatus(JNIEnv* env, jobject thiz, jint mute) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->setMuteStatus(mute) : -1;
}

void dmb_pause(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->pause();
}

jint dmb_resume(JNIEnv* env, jobject thiz, jint arg) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->resume(arg) : -1;
}

jint dmb_pauseForRecord(JNIEnv* env, jobject thiz, jint arg) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->pauseForBackgroundRecord(arg) : -1;
}

jint dmb_resumeForRecord(JNIEnv* env, jobject thiz, jint arg) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->resumeForBackgroundRecord(arg) : -1;
}

jint dmb_getPosition(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->getPosition() : 0;
}

jlong dmb_getPCR(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->getPCR() : 0;
}

jint dmb_fastForward(JNIEnv* env, jobject thiz, jint speed) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->fastForward(speed) : -1;
}

jint dmb_rewind(JNIEnv* env, jobject thiz, jint speed) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->rewind(speed) : -1;
}

jint dmb_getCASStatus(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    return dmb != nullptr ? dmb->getCASStatus() : -1;
}

void dmb_getDeviceID(JNIEnv* env, jobject thiz, jint type) {
    // The id comes back asynchronously through dataCallback.
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->getDeviceID(type);
}

void dmb_clearCASDB(JNIEnv* env, jobject thiz) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->clearCASDB();
}

void dmb_setKd(JNIEnv* env, jobject thiz, jint a, jint b) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->setKd(a, b);
}

void dmb_autoServiceChange(JNIEnv* env, jobject thiz, jlong a, jlong b) {
    sp<IDmb> dmb = dmbOf(env, thiz);
    if (dmb != nullptr) dmb->autoServiceChange(static_cast<long>(a), static_cast<long>(b));
}

const JNINativeMethod kMethods[] = {
        {"native_setup", "(Ljava/lang/Object;)V", (void*)dmb_native_setup},
        {"native_release", "()V", (void*)dmb_release},
        {"native_init", "(II)V", (void*)dmb_init},
        {"native_exit", "()V", (void*)dmb_exit},
        {"native_find", "(I)V", (void*)dmb_find},
        {"native_select", "()V", (void*)dmb_select},
        {"native_play_v3", "(ILjava/lang/String;JJJ)V", (void*)dmb_play_v3},
        {"native_stopPlay", "()V", (void*)dmb_stopPlay},
        {"native_record", "(ILjava/lang/String;J)I", (void*)dmb_record},
        {"native_record_fd", "(ILjava/io/FileDescriptor;J)I", (void*)dmb_record_fd},
        {"native_cancelRecord", "()V", (void*)dmb_cancelRecord},
        {"native_stopRecord", "()V", (void*)dmb_stopRecord},
        {"native_data", "(II)V", (void*)dmb_data},
        {"getDuration", "()I", (void*)dmb_getDuration},
        {"native_setVideoDimension", "(IIIIILandroid/view/Surface;)V", (void*)dmb_setVideoDimension},
        {"native_setAudio", "(III)V", (void*)dmb_setAudio},
        {"native_setMuteStatus", "(I)I", (void*)dmb_setMuteStatus},
        {"native_pause", "()V", (void*)dmb_pause},
        {"native_resume", "(I)I", (void*)dmb_resume},
        {"native_pauseForBackgroundRecord", "(I)I", (void*)dmb_pauseForRecord},
        {"native_resumeForBackgroundRecord", "(I)I", (void*)dmb_resumeForRecord},
        {"native_getPosition", "()I", (void*)dmb_getPosition},
        {"native_getPCR", "()J", (void*)dmb_getPCR},
        {"native_fastForward", "(I)I", (void*)dmb_fastForward},
        {"native_rewind", "(I)I", (void*)dmb_rewind},
        {"native_getCASStatus", "()I", (void*)dmb_getCASStatus},
        {"native_getDeviceID", "(I)V", (void*)dmb_getDeviceID},
        {"native_clearCASDB", "()V", (void*)dmb_clearCASDB},
        {"native_setKd", "(II)V", (void*)dmb_setKd},
        {"native_autoServiceChange", "(JJ)V", (void*)dmb_autoServiceChange},
};

}  // namespace
}  // namespace android

using namespace android;

extern "C" jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        ALOGE("GetEnv failed");
        return JNI_ERR;
    }

    static const char* const kClassName = "com/lge/broadcast/jfullseg/Dmb";
    jclass clazz = env->FindClass(kClassName);
    if (clazz == nullptr) {
        ALOGE("cannot find %s", kClassName);
        return JNI_ERR;
    }

    gFields.clazz = (jclass)env->NewGlobalRef(clazz);
    gFields.postEvent = env->GetStaticMethodID(clazz, "postEventFromNative",
                                               "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    gFields.context = env->GetFieldID(clazz, "mNativeContext", "I");
    gFields.surface = env->GetFieldID(clazz, "mSurface", "Landroid/view/Surface;");
    for (size_t i = 0; i < 14; i++) {
        gFields.select[i] = env->GetFieldID(clazz, kSelectFields[i], "I");
        if (gFields.select[i] == nullptr) {
            ALOGE("cannot find field %s", kSelectFields[i]);
            return JNI_ERR;
        }
    }
    if (gFields.postEvent == nullptr || gFields.context == nullptr) {
        ALOGE("cannot find postEventFromNative or mNativeContext");
        return JNI_ERR;
    }

    if (env->RegisterNatives(clazz, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) < 0) {
        ALOGE("RegisterNatives failed");
        return JNI_ERR;
    }

    ALOGI("registered %zu native methods", sizeof(kMethods) / sizeof(kMethods[0]));
    return JNI_VERSION_1_6;
}
