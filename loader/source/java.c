#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include <so_util/so_util.h>

#include "utils/utils.h"
#include "video.h"
#include "subtitles.h"

extern so_module so_mod;

#define JNI_FAKE_OBJECT 0x42424242

jobject CCoreNativeInterface_init(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)JNI_FAKE_OBJECT;
}

extern jstring NewStringUTF(JNIEnv* env, const char* bytes);
jobject CCoreNativeInterface_getStorageDirectory(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, DATA_PATH);
}

jobject CCoreNativeInterface_getApplicationName(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "ClashOfHeroes");
}

jobject CCoreNativeInterface_getPackageName(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, PKG_NAME);
}

jobject CCoreNativeInterface_getAPKDirectory(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, APK_PATH);
}

#define ANDROID_ORIENTATION_LANDSCAPE 1
jint CCoreNativeInterface_getOrientation(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return ANDROID_ORIENTATION_LANDSCAPE;
}

jint CCoreNativeInterface_getOrientationLandscapeConstant(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return ANDROID_ORIENTATION_LANDSCAPE;
}

#define ANDROID_ORIENTATION_PORTRAIT 2
jint CCoreNativeInterface_getOrientationPortraitConstant(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return ANDROID_ORIENTATION_PORTRAIT;
}

jint CCoreNativeInterface_getScreenWidth(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return SCREEN_NATIVE_W;
}

jint CCoreNativeInterface_getScreenHeight(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return SCREEN_NATIVE_H;
}

#define SCREEN_DENSITY_NATIVE 1.375f
jfloat CCoreNativeInterface_getScreenDensity(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return SCREEN_DENSITY_NATIVE;
}

void CCoreNativeInterface_setPreferredFPS(jmethodID id, va_list args) {
    (void)id;
    va_arg(args, int);
}

void CCoreNativeInterface_forceQuit(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    sceKernelExitDeleteThread(0);
}

jobject CCoreNativeInterface_getDefaultLocaleCode(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "en_US");
}

jobject CCoreNativeInterface_getDeviceModel(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "PSVita");
}

jobject CCoreNativeInterface_getDeviceModelType(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "PSVita");
}

jobject CCoreNativeInterface_getDeviceManufacturer(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "Sony");
}

jint CCoreNativeInterface_getOSVersionCode(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 19;
}

jint CCoreNativeInterface_getNumberOfCores(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 4;
}

jobject CCoreNativeInterface_getTelephonyDeviceID(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "");
}

jobject CCoreNativeInterface_getMacAddress(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "00:00:00:00:00:00");
}

jobject CCoreNativeInterface_getAndroidID(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "0000000000000000");
}

void CCoreNativeInterface_makeToast(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CCoreNativeInterface_showSystemConfirmDialog(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CCoreNativeInterface_showSystemDialog(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jlong CCoreNativeInterface_getSystemTimeInMilliseconds(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jlong)current_timestamp_ms();
}

jfloat CCoreNativeInterface_getPhysicalScreenSize(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 5.0f;
}

jobject CLocalNotificationNativeInterface_init(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)JNI_FAKE_OBJECT;
}

void CLocalNotificationNativeInterface_scheduleNotification(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CLocalNotificationNativeInterface_cancelByID(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CLocalNotificationNativeInterface_cancelAll(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jobject CRemoteNotificationNativeInterface_init(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)JNI_FAKE_OBJECT;
}

void CRemoteNotificationNativeInterface_register(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jobject CUplayNativeInterface_init(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)JNI_FAKE_OBJECT;
}

void CUplayNativeInterface_open(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CUplayNativeInterface_completeAction(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jboolean CUplayNativeInterface_isRewardUnlocked(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_FALSE;
}

void CUplayNativeInterface_tryAutoLogin(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

void CUplayNativeInterface_submitCachedActions(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jboolean CUplayNativeInterface_isUplayLoggedIn(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_FALSE;
}

jobject CUplayNativeInterface_getUserName(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "");
}

jobject jni_stub_new_object(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return (jobject)JNI_FAKE_OBJECT;
}

void jni_stub_void(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

jboolean jni_stub_false(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_FALSE;
}

void CVideoPlayer_present(jmethodID id, va_list args) {
    (void)id;
    int in_apk = va_arg(args, int);
    jstring jpath = va_arg(args, jstring);
    int can_dismiss = va_arg(args, int);
    int has_subtitles = va_arg(args, int);

    char path[512] = "";
    if (jpath) {
        const char *chars = jni->GetStringUTFChars(&jni, jpath, NULL);
        if (chars) {
            strncpy(path, chars, sizeof(path) - 1);
            jni->ReleaseStringUTFChars(&jni, jpath, (char *)chars);
        }
    }
    video_present(path, in_apk != 0, can_dismiss != 0, has_subtitles != 0);
}

void CVideoPlayer_dismiss(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    video_dismiss();
}

jboolean CVideoPlayer_isPlaying(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return video_is_playing() ? JNI_TRUE : JNI_FALSE;
}

jfloat CVideoPlayer_getDuration(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return video_duration_s();
}

jfloat CVideoPlayer_getTime(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return video_time_s();
}

static void jstring_copy(jstring js, char *out, size_t cap) {
    out[0] = 0;
    if (!js)
        return;
    const char *chars = jni->GetStringUTFChars(&jni, js, NULL);
    if (chars) {
        strncpy(out, chars, cap - 1);
        out[cap - 1] = 0;
        jni->ReleaseStringUTFChars(&jni, js, (char *)chars);
    }
}

jlong CVideoPlayer_createSubtitle(jmethodID id, va_list args) {
    (void)id;
    jstring jtext = va_arg(args, jstring);
    jstring jfont = va_arg(args, jstring);
    unsigned size = va_arg(args, unsigned);
    jstring jalign = va_arg(args, jstring);
    double x = va_arg(args, double);
    double y = va_arg(args, double);
    double w = va_arg(args, double);
    double h = va_arg(args, double);

    char text[1024], font[64], align[32];
    jstring_copy(jtext, text, sizeof(text));
    jstring_copy(jfont, font, sizeof(font));
    jstring_copy(jalign, align, sizeof(align));
    return subtitles_create(text, font, size, align, (float)x, (float)y, (float)w, (float)h);
}

void CVideoPlayer_setSubtitleColour(jmethodID id, va_list args) {
    (void)id;
    jlong sid = va_arg(args, jlong);
    double r = va_arg(args, double);
    double g = va_arg(args, double);
    double b = va_arg(args, double);
    double a = va_arg(args, double);
    subtitles_set_colour(sid, (float)r, (float)g, (float)b, (float)a);
}

void CVideoPlayer_removeSubtitle(jmethodID id, va_list args) {
    (void)id;
    jlong sid = va_arg(args, jlong);
    subtitles_remove(sid);
}

jboolean CExpansionDownloader_filesDelivered(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_TRUE;
}

jboolean CExpansionDownloader_beginDownloading(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_TRUE;
}

jobject CExpansionDownloader_noArchivePath(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return NewStringUTF(&jni, "");
}

jint CExpansionDownloader_mainVersion(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 1906;
}

jint CExpansionDownloader_patchVersion(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return 0;
}

NameToMethodID nameToMethodId[] = {
		{ 100, "com/taggames/moflow/nativeinterface/CCoreNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 101, "GetExternalStorageDirectory", METHOD_TYPE_OBJECT },
		{ 102, "GetInternalStorageDirectory", METHOD_TYPE_OBJECT },
		{ 103, "GetApplicationName", METHOD_TYPE_OBJECT },
		{ 104, "GetPackageName", METHOD_TYPE_OBJECT },
		{ 105, "GetAPKDirectory", METHOD_TYPE_OBJECT },
		{ 106, "GetOrientation", METHOD_TYPE_INT },
		{ 107, "GetOrientationLandscapeConstant", METHOD_TYPE_INT },
		{ 108, "GetOrientationPortraitConstant", METHOD_TYPE_INT },
		{ 109, "GetScreenWidth", METHOD_TYPE_INT },
		{ 110, "GetScreenHeight", METHOD_TYPE_INT },
		{ 111, "GetScreenDensity", METHOD_TYPE_FLOAT },
		{ 112, "SetPreferredFPS", METHOD_TYPE_VOID },
		{ 113, "ForceQuit", METHOD_TYPE_VOID },
		{ 114, "GetDefaultLocaleCode", METHOD_TYPE_OBJECT },
		{ 115, "GetDeviceModel", METHOD_TYPE_OBJECT },
		{ 116, "GetDeviceModelType", METHOD_TYPE_OBJECT },
		{ 117, "GetDeviceManufacturer", METHOD_TYPE_OBJECT },
		{ 118, "GetOSVersion", METHOD_TYPE_INT },
		{ 119, "GetNumberOfCores", METHOD_TYPE_INT },
		{ 120, "GetTelephonyDeviceID", METHOD_TYPE_OBJECT },
		{ 121, "GetMacAddress", METHOD_TYPE_OBJECT },
		{ 122, "GetAndroidID", METHOD_TYPE_OBJECT },
		{ 123, "MakeToast", METHOD_TYPE_VOID },
		{ 124, "ShowSystemConfirmDialog", METHOD_TYPE_VOID },
		{ 125, "ShowSystemDialog", METHOD_TYPE_VOID },
		{ 126, "GetSystemTimeInMilliseconds", METHOD_TYPE_LONG },
		{ 127, "GetPhysicalScreenSize", METHOD_TYPE_FLOAT },
		{ 128, "com/taggames/moflow/nativeinterface/CLocalNotificationNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 129, "ScheduleNotification", METHOD_TYPE_VOID },
		{ 130, "CancelByID", METHOD_TYPE_VOID },
		{ 131, "CancelAll", METHOD_TYPE_VOID },
		{ 132, "com/taggames/moflow/nativeinterface/CRemoteNotificationNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 133, "Register", METHOD_TYPE_VOID },
		{ 134, "com/taggames/mmcoh/uplay/CUplayNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 135, "Open", METHOD_TYPE_VOID },
		{ 136, "CompleteAction", METHOD_TYPE_VOID },
		{ 137, "IsRewardUnlocked", METHOD_TYPE_BOOLEAN },
		{ 138, "TryAutoLogin", METHOD_TYPE_VOID },
		{ 139, "SubmitCachedActions", METHOD_TYPE_VOID },
		{ 140, "IsUplayLoggedIn", METHOD_TYPE_BOOLEAN },
		{ 141, "GetUserName", METHOD_TYPE_OBJECT },
		{ 142, "com/taggames/mmcoh/nativeinterface/CActivityLifeCycleEventsNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 143, "com/taggames/mmcoh/appirater/CAppiraterNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 144, "OnAppLaunched", METHOD_TYPE_VOID },
		{ 145, "ShowRaterNow", METHOD_TYPE_VOID },
		{ 146, "SetMarketplace", METHOD_TYPE_VOID },
		{ 147, "com/taggames/mmcoh/flurry/CFlurryNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 148, "StartSession", METHOD_TYPE_VOID },
		{ 149, "LogEventA", METHOD_TYPE_VOID },
		{ 150, "LogEventB", METHOD_TYPE_VOID },
		{ 151, "LogEventC", METHOD_TYPE_VOID },
		{ 152, "LogEventD", METHOD_TYPE_VOID },
		{ 153, "EndTimedEvent", METHOD_TYPE_VOID },
		{ 154, "EndSession", METHOD_TYPE_VOID },
		{ 155, "com/taggames/moflow/nativeinterface/CEmailCompositionNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 156, "Present([Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V", METHOD_TYPE_VOID },
		{ 157, "com/taggames/moflow/nativeinterface/CVideoPlayerNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 191, "Present(ZLjava/lang/String;ZZ)V", METHOD_TYPE_VOID },
		{ 192, "Present(ILjava/lang/String;II)V", METHOD_TYPE_VOID },
		{ 158, "IsPlaying", METHOD_TYPE_BOOLEAN },
		{ 159, "GetDuration", METHOD_TYPE_FLOAT },
		{ 160, "GetTime", METHOD_TYPE_FLOAT },
		{ 161, "Dismiss", METHOD_TYPE_VOID },
		{ 162, "CreateSubtitle", METHOD_TYPE_LONG },
		{ 163, "SetSubtitleColour", METHOD_TYPE_VOID },
		{ 164, "RemoveSubtitle", METHOD_TYPE_VOID },
		{ 165, "com/taggames/moflow/nativeinterface/CFacebookNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 166, "Authenticate", METHOD_TYPE_VOID },
		{ 167, "IsSignedIn", METHOD_TYPE_BOOLEAN },
		{ 168, "HasPermission", METHOD_TYPE_BOOLEAN },
		{ 169, "AuthoriseReadPermissions", METHOD_TYPE_VOID },
		{ 170, "AuthoriseWritePermissions", METHOD_TYPE_VOID },
		{ 171, "SignOut", METHOD_TYPE_VOID },
		{ 172, "MakePostToFeedRequest", METHOD_TYPE_VOID },
		{ 173, "PublishInstall", METHOD_TYPE_VOID },
		{ 174, "com/taggames/moflow/nativeinterface/CMarketbillingNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 175, "CheckBillingSupported", METHOD_TYPE_VOID },
		{ 176, "StartTransaction", METHOD_TYPE_VOID },
		{ 177, "ConfirmTransaction", METHOD_TYPE_VOID },
		{ 178, "RestoreTransactions", METHOD_TYPE_VOID },
		{ 179, "com/taggames/mmcoh/nativeinterface/CExpansionDownloaderNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 180, "com/taggames/moflow/nativeinterface/CExpansionDownloaderNativeInterface/<init>", METHOD_TYPE_OBJECT },
		{ 181, "ExpansionFilesDelivered", METHOD_TYPE_BOOLEAN },
		{ 182, "SetMarketPublicKey", METHOD_TYPE_VOID },
		{ 183, "BeginDownloadingExpansionFiles", METHOD_TYPE_BOOLEAN },
		{ 184, "GetPathToMainExpansion", METHOD_TYPE_OBJECT },
		{ 185, "GetPathToPatchExpansion", METHOD_TYPE_OBJECT },
		{ 186, "ExpansionDownloaderInitialise", METHOD_TYPE_VOID },
		{ 187, "OnSystemBeginUnpacking", METHOD_TYPE_VOID },
		{ 188, "OnSystemStopsUnpacking", METHOD_TYPE_VOID },
		{ 189, "GetMainExpansionVersionNumber", METHOD_TYPE_INT },
		{ 190, "GetPatchExpansionVersionNumber", METHOD_TYPE_INT },
};

MethodsBoolean methodsBoolean[] = {
		{ 137, CUplayNativeInterface_isRewardUnlocked },
		{ 140, CUplayNativeInterface_isUplayLoggedIn },
		{ 158, CVideoPlayer_isPlaying },
		{ 167, jni_stub_false },
		{ 168, jni_stub_false },
		{ 181, CExpansionDownloader_filesDelivered },
		{ 183, CExpansionDownloader_beginDownloading },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {
		{ 111, CCoreNativeInterface_getScreenDensity },
		{ 127, CCoreNativeInterface_getPhysicalScreenSize },
		{ 159, CVideoPlayer_getDuration },
		{ 160, CVideoPlayer_getTime },
};
MethodsInt methodsInt[] = {
		{ 106, CCoreNativeInterface_getOrientation },
		{ 107, CCoreNativeInterface_getOrientationLandscapeConstant },
		{ 108, CCoreNativeInterface_getOrientationPortraitConstant },
		{ 109, CCoreNativeInterface_getScreenWidth },
		{ 110, CCoreNativeInterface_getScreenHeight },
		{ 118, CCoreNativeInterface_getOSVersionCode },
		{ 119, CCoreNativeInterface_getNumberOfCores },
		{ 189, CExpansionDownloader_mainVersion },
		{ 190, CExpansionDownloader_patchVersion },
};
MethodsLong methodsLong[] = {
		{ 126, CCoreNativeInterface_getSystemTimeInMilliseconds },
		{ 162, CVideoPlayer_createSubtitle },
};
MethodsObject methodsObject[] = {
		{ 100, CCoreNativeInterface_init },
		{ 101, CCoreNativeInterface_getStorageDirectory },
		{ 102, CCoreNativeInterface_getStorageDirectory },
		{ 103, CCoreNativeInterface_getApplicationName },
		{ 104, CCoreNativeInterface_getPackageName },
		{ 105, CCoreNativeInterface_getAPKDirectory },
		{ 114, CCoreNativeInterface_getDefaultLocaleCode },
		{ 115, CCoreNativeInterface_getDeviceModel },
		{ 116, CCoreNativeInterface_getDeviceModelType },
		{ 117, CCoreNativeInterface_getDeviceManufacturer },
		{ 120, CCoreNativeInterface_getTelephonyDeviceID },
		{ 121, CCoreNativeInterface_getMacAddress },
		{ 122, CCoreNativeInterface_getAndroidID },
		{ 128, CLocalNotificationNativeInterface_init },
		{ 132, CRemoteNotificationNativeInterface_init },
		{ 134, CUplayNativeInterface_init },
		{ 141, CUplayNativeInterface_getUserName },
		{ 142, jni_stub_new_object },
		{ 143, jni_stub_new_object },
		{ 147, jni_stub_new_object },
		{ 155, jni_stub_new_object },
		{ 157, jni_stub_new_object },
		{ 165, jni_stub_new_object },
		{ 174, jni_stub_new_object },
		{ 179, jni_stub_new_object },
		{ 180, jni_stub_new_object },
		{ 184, CExpansionDownloader_noArchivePath },
		{ 185, CExpansionDownloader_noArchivePath },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
		{ 112, CCoreNativeInterface_setPreferredFPS },
		{ 113, CCoreNativeInterface_forceQuit },
		{ 123, CCoreNativeInterface_makeToast },
		{ 124, CCoreNativeInterface_showSystemConfirmDialog },
		{ 125, CCoreNativeInterface_showSystemDialog },
		{ 129, CLocalNotificationNativeInterface_scheduleNotification },
		{ 130, CLocalNotificationNativeInterface_cancelByID },
		{ 131, CLocalNotificationNativeInterface_cancelAll },
		{ 133, CRemoteNotificationNativeInterface_register },
		{ 135, CUplayNativeInterface_open },
		{ 136, CUplayNativeInterface_completeAction },
		{ 138, CUplayNativeInterface_tryAutoLogin },
		{ 139, CUplayNativeInterface_submitCachedActions },
		{ 144, jni_stub_void }, { 145, jni_stub_void }, { 146, jni_stub_void },
		{ 148, jni_stub_void }, { 149, jni_stub_void }, { 150, jni_stub_void },
		{ 151, jni_stub_void }, { 152, jni_stub_void }, { 153, jni_stub_void },
		{ 154, jni_stub_void }, { 156, jni_stub_void }, { 161, CVideoPlayer_dismiss },
		{ 163, CVideoPlayer_setSubtitleColour }, { 164, CVideoPlayer_removeSubtitle },
		{ 166, jni_stub_void },
		{ 169, jni_stub_void }, { 170, jni_stub_void }, { 171, jni_stub_void },
		{ 172, jni_stub_void }, { 173, jni_stub_void }, { 175, jni_stub_void },
		{ 176, jni_stub_void }, { 177, jni_stub_void }, { 178, jni_stub_void },
		{ 182, jni_stub_void }, { 186, jni_stub_void }, { 187, jni_stub_void },
		{ 188, jni_stub_void }, { 191, CVideoPlayer_present }, { 192, jni_stub_void },
};

char WINDOW_SERVICE[] = "window";

const int SDK_INT = 19;

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
