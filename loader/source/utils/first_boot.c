#include "utils/first_boot.h"

#include "utils/logger.h"
#include "utils/setup_data.h"
#include "utils/utils.h"

#include <psp2/appmgr.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>

#include <stdio.h>
#include <string.h>

#include <vitaGL.h>

#define OBB_NAME "main.1906." PKG_NAME ".obb"
#define OBB_DIR DATA_PATH PKG_NAME
#define DONE_PATH DATA_PATH ".setup_done"
#define SKIP_PATH DATA_PATH ".skip_setup"
#define SKIP_ONCE_PATH DATA_PATH ".skip_setup_once"
#define SPARE_MB 64

static int vgl_up = 0;

static void swap_frame(void) {
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_TRUE);
}

static int ask(const char *msg, int yes_no) {
    if (!vgl_up) {
        vglInit(0);
        vgl_up = 1;
    }
    SceMsgDialogUserMessageParam user;
    sceClibMemset(&user, 0, sizeof(user));
    user.buttonType = yes_no ? SCE_MSG_DIALOG_BUTTON_TYPE_YESNO : SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = (const SceChar8 *)msg;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    if (sceMsgDialogInit(&param) < 0)
        return 0;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        swap_frame();
    SceMsgDialogResult result;
    sceClibMemset(&result, 0, sizeof(result));
    sceMsgDialogGetResult(&result);
    sceMsgDialogTerm();
    return result.buttonId == SCE_MSG_DIALOG_BUTTON_ID_YES;
}

static int write_marker(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

__attribute__((noreturn)) static void relaunch(void) {
    sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    ask("Start the game again.", 0);
    sceKernelExitProcess(0);
    while (1);
}

__attribute__((noreturn)) static void skip_once(void) {
    if (write_marker(SKIP_ONCE_PATH, "Skip setup at the next launch\n") < 0) {
        ask("Could not write ux0:data/mmcoh/.skip_setup_once.\n\nCheck that the memory "
            "card has free space, then start the game again.", 0);
        sceKernelExitProcess(0);
    }
    relaunch();
}

__attribute__((noreturn)) static void fail_exit(const char *error) {
    char msg[512];
    l_fatal("first boot: %s", error);
    snprintf(msg, sizeof(msg), "%s\n\nStart the game anyway? Only if ux0:data/mmcoh/ is "
             "already set up.", error);
    if (ask(msg, 1))
        skip_once();
    sceKernelExitProcess(0);
    while (1);
}

static int ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static const char *find_obb(void) {
    static const char *const paths[] = {
        DATA_PATH OBB_NAME,
        OBB_DIR "/" OBB_NAME,
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (file_exists(paths[i]))
            return paths[i];
    return NULL;
}

__attribute__((noreturn)) static void missing_files(int have_apk, int have_obb) {
    char msg[400], other_apk[128] = "", other_obb[128] = "";
    SceUID d = sceIoDopen(DATA_PATH);
    if (d >= 0) {
        SceIoDirent e;
        while (sceIoDread(d, &e) > 0) {
            if (!have_apk && !other_apk[0] && ends_with(e.d_name, ".apk"))
                snprintf(other_apk, sizeof(other_apk), "%.100s", e.d_name);
            if (!have_obb && !other_obb[0] && ends_with(e.d_name, ".obb"))
                snprintf(other_obb, sizeof(other_obb), "%.100s", e.d_name);
        }
        sceIoDclose(d);
    }

    int n = snprintf(msg, sizeof(msg),
                     "Copy these two files from the Android v1.4 release to "
                     "ux0:data/mmcoh/:\n\n%s ClashOfHeroes.apk\n%s " OBB_NAME "\n",
                     have_apk ? "[found]" : "[MISSING]", have_obb ? "[found]" : "[MISSING]");
    if (d < 0)
        n += snprintf(msg + n, sizeof(msg) - n, "\nThe folder ux0:data/mmcoh/ does not exist yet.\n");
    if (other_apk[0])
        n += snprintf(msg + n, sizeof(msg) - n, "\nFound %s: rename it to ClashOfHeroes.apk.\n",
                      other_apk);
    if (other_obb[0] && strncmp(other_obb, "main.", 5) == 0 && strstr(other_obb, PKG_NAME))
        snprintf(msg + n, sizeof(msg) - n, "\nFound %s: another version of the game. "
                 "Only v1.4 (1906) runs.\n", other_obb);
    else if (other_obb[0])
        snprintf(msg + n, sizeof(msg) - n, "\nFound %s: the OBB must keep its "
                 "original name.\n", other_obb);
    fail_exit(msg);
}

static void check_free_space(uint32_t bytes) {
    SceIoDevInfo info;
    sceClibMemset(&info, 0, sizeof(info));
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) < 0)
        return;
    uint32_t need_mb = bytes / (1024 * 1024) + SPARE_MB;
    uint32_t free_mb = (uint32_t)(info.free_size / (1024 * 1024));
    if (free_mb >= need_mb)
        return;
    char msg[256];
    snprintf(msg, sizeof(msg), "Setup needs %u MB free on ux0:, and %u MB are free.\n\n"
             "Free up %u MB and start the game again.",
             (unsigned)need_mb, (unsigned)free_mb, (unsigned)(need_mb - free_mb));
    fail_exit(msg);
}

static void confirm_setup(void) {
    if (ask("Setup will now unpack ClashOfHeroes.apk and the OBB into ux0:data/mmcoh/ "
            "and apply the required patches. This happens once, takes a few minutes, and "
            "deletes the OBB afterwards.\n\nSet up now?\n\nChoose No to skip setup, for "
            "example if the folder was already set up using the repo's python script.", 1))
        return;
    if (ask("Skip setup on every launch from now on?\n\nYes: never run it again. Delete "
            "ux0:data/mmcoh/.skip_setup to undo.\nNo: skip it this time only.", 1)) {
        if (write_marker(SKIP_PATH, "Never run the first-boot setup\n") < 0)
            fail_exit("Could not write ux0:data/mmcoh/.skip_setup.");
        relaunch();
    }
    skip_once();
}

static setup_job job;
static volatile int job_rc, job_finished;

static int setup_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    job_rc = setup_extract(&job);
    job_finished = 1;
    return sceKernelExitDeleteThread(0);
}

static void progress_text(char *msg, size_t n) {
    static const char head[] =
        "Setting up Clash of Heroes.\n\n"
        "Keep the Vita on.\n\n";
    switch (job.stage) {
    case SETUP_STAGE_APK:
        snprintf(msg, n, "%sUnpacking ClashOfHeroes.apk...", head);
        break;
    case SETUP_STAGE_OBB:
        snprintf(msg, n, "%sUnpacking the OBB: %d of %d files...", head, job.files_done,
                 job.files_total);
        break;
    default:
        snprintf(msg, n, "%sApplying the patches...", head);
        break;
    }
}

static void close_progress(void) {
    sceMsgDialogClose();
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        swap_frame();
    sceMsgDialogTerm();
}

__attribute__((noreturn)) static void run_setup(const char *obb) {
    l_info("first boot: setting up from %s and %s", APK_PATH, obb);
    if (setup_open(&job, APK_PATH, obb, DATA_PATH) < 0)
        fail_exit(job.error);
    check_free_space(job.bytes_total);

    static char msg[512];
    progress_text(msg, sizeof(msg));
    SceMsgDialogProgressBarParam bar;
    sceClibMemset(&bar, 0, sizeof(bar));
    bar.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
    bar.msg = (const SceChar8 *)msg;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    _sceCommonDialogSetMagicNumber(&param.commonParam);
    param.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
    param.progBarParam = &bar;
    sceMsgDialogInit(&param);

    SceUID tid = sceKernelCreateThread("first_boot_setup", setup_thread, 0x10000100,
                                       256 * 1024, 0, 0, NULL);
    if (tid < 0 || sceKernelStartThread(tid, 0, NULL) < 0) {
        close_progress();
        fail_exit("Could not start the setup thread.");
    }

    uint32_t shown_pct = 0;
    int frame = 0;
    while (!job_finished) {
        uint32_t pct = job.bytes_total
            ? (uint32_t)((uint64_t)job.bytes_done * 100 / job.bytes_total) : 0;
        if (pct != shown_pct) {
            sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, pct);
            shown_pct = pct;
        }
        if (++frame % 15 == 0) {
            progress_text(msg, sizeof(msg));
            sceMsgDialogProgressBarSetMsg(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT,
                                          (const SceChar8 *)msg);
        }
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        swap_frame();
    }
    close_progress();
    if (job_rc < 0)
        fail_exit(job.error);
    setup_close(&job);
    l_info("first boot: %d files, %u bytes", job.files_done, (unsigned)job.bytes_done);

    if (write_marker(DONE_PATH, "Set up from ClashOfHeroes.apk and " OBB_NAME "\n") < 0)
        fail_exit("Setup finished, but could not write ux0:data/mmcoh/.setup_done.\n\n"
                  "Check that the memory card has free space.");
    if (sceIoRemove(obb) < 0)
        l_fatal("first boot: could not delete %s", obb);
    sceIoRmdir(OBB_DIR);
    relaunch();
}

void first_boot_run(void) {
    if (file_exists(DONE_PATH) || file_exists(SKIP_PATH))
        return;
    if (file_exists(SKIP_ONCE_PATH)) {
        sceIoRemove(SKIP_ONCE_PATH);
        return;
    }
    const char *obb = find_obb();
    int have_apk = file_exists(APK_PATH);
    if (!obb && file_exists(SO_PATH))
        return;
    if (!obb || !have_apk)
        missing_files(have_apk, obb != NULL);
    confirm_setup();
    run_setup(obb);
}
