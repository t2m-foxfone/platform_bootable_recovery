/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "bootloader.h"
#ifdef MSM_FIRST_FORMAT
#include "bootselect.h"
#endif
#include "common.h"
#include "cutils/properties.h"
#include "cutils/android_reboot.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "ui.h"
#include "screen_ui.h"
#include "device.h"
#include "adb_install.h"
extern "C" {
#include "minadbd/adb.h"
}

//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, begin
#include "private/android_filesystem_config.h"
//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, end

#define RESERVED_MEMORY_SIZE (sysconf(_SC_PAGESIZE) * 1024 * 5)
#define GET_AVPHYS_MEM() ((long long)sysconf(_SC_AVPHYS_PAGES) * sysconf(_SC_PAGESIZE))

struct selabel_handle *sehandle;

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
  { "show_text", no_argument, NULL, 't' },
  { "just_exit", no_argument, NULL, 'x' },
  { "first_format", no_argument, NULL, 'f' },
  { "locale", required_argument, NULL, 'l' },
  { NULL, 0, NULL, 0 },
};

#define LAST_LOG_FILE "/cache/recovery/last_log"

static const char *CACHE_LOG_DIR = "/cache/recovery";
static const char *COMMAND_FILE = "/cache/recovery/command";
static const char *INTENT_FILE = "/cache/recovery/intent";
static const char *LOG_FILE = "/cache/recovery/log";
static const char *LAST_INSTALL_FILE = "/cache/recovery/last_install";
static const char *LOCALE_FILE = "/cache/recovery/last_locale";
static const char *CACHE_ROOT = "/cache";
static const char *SDCARD_ROOT = "/sdcard";
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *TEMPORARY_INSTALL_FILE = "/tmp/last_install";
static const char *SIDELOAD_TEMP_DIR = "/tmp/sideload";
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
static const char *BACKUP_LOG_FILE = "/data/recovery.log";
/*Modify by baijian 2014-01-13 Firefox FOTA result path begin*/
//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, begin
static const char *JRD_FOTA_RESULT_FILE = "/data/fota/result.txt";
//static const char *FOTA_STATUS_FILE = "/cache/recovery/fota.status";
//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, end
/*Modify by baijian 2014-01-13 Firefox FOTA result path end*/
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */

/* czb@tcl.com factory reset, do not erase data version info. start*/
static const char *USERDATA_VERSION_INFO="/data/userdata.ver";
static char userdata_info_buf[256] = {0};
/* czb@tcl.com factory reset, do not erase data version info. end*/

RecoveryUI* ui = NULL;
char* locale = NULL;
char recovery_version[PROPERTY_VALUE_MAX+1];

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *   --set_encrypted_filesystem=on|off - enables / diasables encrypted fs
 *   --just_exit - do nothing; exit and reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_volume() reformats /data
 * 6. erase_volume() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=/cache/some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_volume() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

// open a given path, mounting partitions as necessary
FILE*
fopen_path(const char *path, const char *mode) {
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1, sehandle);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

/* czb@tcl.com factory reset, do not erase data version info. start*/
static void save_userdata_ver_info(){
    FILE *info = fopen_path(USERDATA_VERSION_INFO,  "r");
    if (info == NULL) {
		LOGE("Can't open %s\n", USERDATA_VERSION_INFO);
    } else {
		fgets(userdata_info_buf, 256, info);
        fclose(info);
	}
}

static void restore_userdata_ver_info(){
	FILE *info = fopen_path(USERDATA_VERSION_INFO,  "w");
	if (info == NULL) {
		LOGE("Can't open %s\n", USERDATA_VERSION_INFO);
		return;
	}
	if(userdata_info_buf==NULL){
		LOGE("No information to restore \n");
	}else{
		fputs(userdata_info_buf, info);
		fflush(info);
	}
	fclose(info);
	chmod(USERDATA_VERSION_INFO, 0644);
}
/* czb@tcl.com factory reset, do not erase data version info. end*/


// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
#ifdef MSM_FIRST_FORMAT
    struct boot_selection_info bootselect_info;
    memset(&bootselect_info, 0, sizeof(bootselect_info));
#endif
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGE("Bad boot message\n\"%.20s\"\n", boot.recovery);
        }
    }
#ifdef MSM_FIRST_FORMAT
    // --- if that doesn't work, try the /bootselect partition
    // Format /data and /cache partition if format bit is set
    if (*argc <= 1) {
        if (!get_bootselect_info(&bootselect_info)) {
            unsigned int state_info = bootselect_info.state_info;
            if ((bootselect_info.signature == BOOTSELECT_SIGNATURE) &&
                    (bootselect_info.version == BOOTSELECT_VERSION)) {
                if (((bootselect_info.state_info & BOOTSELECT_FORMAT) &&
                            !(bootselect_info.state_info & BOOTSELECT_FACTORY))) {
                    *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
                    *argc = 0;
                    (*argv)[(*argc)++] = strdup("recovery");
                    (*argv)[(*argc)++] = strdup("--first_format");
                }
            } else
                LOGI("Signature:0x%08x or version:0x%08x is mismatched of bootselect partition\n",
                        bootselect_info.signature, bootselect_info.version);
        }
    }
#endif
    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *token;
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                token = strtok(buf, "\r\n");
                if (token != NULL) {
                    (*argv)[*argc] = strdup(token);  // Strip newline.
                } else {
                    --*argc;
                }
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}

static void
set_sdcard_update_bootloader_message() {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    set_bootloader_message(&boot);
}

// How much of the temp log we have copied to the copy in cache.
static long tmplog_offset = 0;

static void
copy_log_file(const char* source, const char* destination, int append) {
    FILE *log = fopen_path(destination, append ? "a" : "w");
    if (log == NULL) {
        LOGE("Can't open %s\n", destination);
    } else {
        FILE *tmplog = fopen(source, "r");
        if (tmplog != NULL) {
            if (append) {
                fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            }
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            if (append) {
                tmplog_offset = ftell(tmplog);
            }
            check_and_fclose(tmplog, source);
        }
        check_and_fclose(log, destination);
    }
}

// Rename last_log -> last_log.1 -> last_log.2 -> ... -> last_log.$max
// Overwrites any existing last_log.$max.
static void
rotate_last_logs(int max) {
    char oldfn[256];
    char newfn[256];

    int i;
    for (i = max-1; i >= 0; --i) {
        snprintf(oldfn, sizeof(oldfn), (i==0) ? LAST_LOG_FILE : (LAST_LOG_FILE ".%d"), i);
        snprintf(newfn, sizeof(newfn), LAST_LOG_FILE ".%d", i+1);
        // ignore errors
        rename(oldfn, newfn);
    }
}

static void
copy_logs() {
    // Copy logs to cache so the system can find out what happened.
    copy_log_file(TEMPORARY_LOG_FILE, LOG_FILE, true);
    copy_log_file(TEMPORARY_LOG_FILE, LAST_LOG_FILE, false);
    copy_log_file(TEMPORARY_INSTALL_FILE, LAST_INSTALL_FILE, false);
    chmod(LOG_FILE, 0600);
    chown(LOG_FILE, 1000, 1000);   // system user
    chmod(LAST_LOG_FILE, 0640);
    chmod(LAST_INSTALL_FILE, 0644);
    /*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
    //chmod(FOTA_STATUS_FILE, 0644);//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18
    //keeps logs to system for investigation after factory reset
    copy_log_file(TEMPORARY_LOG_FILE, BACKUP_LOG_FILE, false); // save log to system
    chmod(BACKUP_LOG_FILE, 0644);
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */
    sync();
}

// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent) {
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Save the locale to cache, so if recovery is next started up
    // without a --locale argument (eg, directly from the bootloader)
    // it will use the last-known locale.
    if (locale != NULL) {
        LOGI("Saving locale \"%s\"\n", locale);
        FILE* fp = fopen_path(LOCALE_FILE, "w");
        fwrite(locale, 1, strlen(locale), fp);
        fflush(fp);
        fsync(fileno(fp));
        check_and_fclose(fp, LOCALE_FILE);
    }

    copy_logs();

    // Reset to normal system boot so recovery won't cycle indefinitely.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    if (ensure_path_mounted(COMMAND_FILE) != 0 ||
        (unlink(COMMAND_FILE) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    ensure_path_unmounted(CACHE_ROOT);
    sync();  // For good measure.
}

typedef struct _saved_log_file {
    char* name;
    struct stat st;
    unsigned char* data;
    struct _saved_log_file* next;
} saved_log_file;

static int
erase_volume(const char *volume) {
    bool is_cache = (strcmp(volume, CACHE_ROOT) == 0);

    ui->SetBackground(RecoveryUI::ERASING);
    ui->SetProgressType(RecoveryUI::INDETERMINATE);

    saved_log_file* head = NULL;

    if (is_cache) {
        // If we're reformatting /cache, we load any
        // "/cache/recovery/last*" files into memory, so we can restore
        // them after the reformat.

        ensure_path_mounted(volume);

        DIR* d;
        struct dirent* de;
        d = opendir(CACHE_LOG_DIR);
        if (d) {
            char path[PATH_MAX];
            strcpy(path, CACHE_LOG_DIR);
            strcat(path, "/");
            int path_len = strlen(path);
            while ((de = readdir(d)) != NULL) {
                if (strncmp(de->d_name, "last", 4) == 0) {
                    saved_log_file* p = (saved_log_file*) malloc(sizeof(saved_log_file));
                    strcpy(path+path_len, de->d_name);
                    p->name = strdup(path);
                    if (stat(path, &(p->st)) == 0) {
                        // truncate files to 512kb
                        if (p->st.st_size > (1 << 19)) {
                            p->st.st_size = 1 << 19;
                        }
                        p->data = (unsigned char*) malloc(p->st.st_size);
                        FILE* f = fopen(path, "rb");
                        fread(p->data, 1, p->st.st_size, f);
                        fclose(f);
                        p->next = head;
                        head = p;
                    } else {
                        free(p);
                    }
                }
            }
            closedir(d);
        } else {
            if (errno != ENOENT) {
                printf("opendir failed: %s\n", strerror(errno));
            }
        }
    }

    ui->Print("Formatting %s...\n", volume);

    ensure_path_unmounted(volume);
    int result = format_volume(volume);

    if (is_cache) {
        while (head) {
            FILE* f = fopen_path(head->name, "wb");
            if (f) {
                fwrite(head->data, 1, head->st.st_size, f);
                fclose(f);
                chmod(head->name, head->st.st_mode);
                chown(head->name, head->st.st_uid, head->st.st_gid);
            }
            free(head->name);
            free(head->data);
            saved_log_file* temp = head->next;
            free(head);
            head = temp;
        }

        // Any part of the log we'd copied to cache is now gone.
        // Reset the pointer so we copy from the beginning of the temp
        // log.
        tmplog_offset = 0;
        copy_logs();
    }

    return result;
}

static char*
copy_sideloaded_package(const char* original_path) {
  if (ensure_path_mounted(original_path) != 0) {
    LOGE("Can't mount %s\n", original_path);
    return NULL;
  }

  if (ensure_path_mounted(SIDELOAD_TEMP_DIR) != 0) {
    LOGE("Can't mount %s\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }

  if (mkdir(SIDELOAD_TEMP_DIR, 0700) != 0) {
    if (errno != EEXIST) {
      LOGE("Can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
      return NULL;
    }
  }

  // verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
  // directory, owned by root, readable and writable only by root.
  struct stat st;
  if (stat(SIDELOAD_TEMP_DIR, &st) != 0) {
    LOGE("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
    return NULL;
  }
  if (!S_ISDIR(st.st_mode)) {
    LOGE("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
    return NULL;
  }
  if ((st.st_mode & 0777) != 0700) {
    LOGE("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
    return NULL;
  }
  if (st.st_uid != 0) {
    LOGE("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR, st.st_uid);
    return NULL;
  }

  char copy_path[PATH_MAX];
  strcpy(copy_path, SIDELOAD_TEMP_DIR);
  strcat(copy_path, "/package.zip");

  char* buffer = (char*)malloc(BUFSIZ);
  if (buffer == NULL) {
    LOGE("Failed to allocate buffer\n");
    return NULL;
  }

  size_t read;
  FILE* fin = fopen(original_path, "rb");
  if (fin == NULL) {
    LOGE("Failed to open %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }
  FILE* fout = fopen(copy_path, "wb");
  if (fout == NULL) {
    LOGE("Failed to open %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) {
    if (fwrite(buffer, 1, read, fout) != read) {
      LOGE("Short write of %s (%s)\n", copy_path, strerror(errno));
      return NULL;
    }
  }

  free(buffer);

  if (fclose(fout) != 0) {
    LOGE("Failed to close %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  if (fclose(fin) != 0) {
    LOGE("Failed to close %s (%s)\n", original_path, strerror(errno));
    return NULL;
  }

  // "adb push" is happy to overwrite read-only files when it's
  // running as root, but we'll try anyway.
  if (chmod(copy_path, 0400) != 0) {
    LOGE("Failed to chmod %s (%s)\n", copy_path, strerror(errno));
    return NULL;
  }

  return strdup(copy_path);
}

static const char**
prepend_title(const char* const* headers) {
    // count the number of lines in our title, plus the
    // caller-provided headers.
    int count = 3;   // our title has 3 lines
    const char* const* p;
    for (p = headers; *p; ++p, ++count);

    const char** new_headers = (const char**)malloc((count+1) * sizeof(char*));
    const char** h = new_headers;
    *(h++) = "System recovery <" EXPAND(RECOVERY_API_VERSION) "e>";
    *(h++) = recovery_version;
    *(h++) = "";
    for (p = headers; *p; ++p, ++h) *h = *p;
    *h = NULL;

    return new_headers;
}

static int
get_menu_selection(const char* const * headers, const char* const * items,
                   int menu_only, int initial_selection, Device* device) {
    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui->FlushKeys();

    ui->StartMenu(headers, items, initial_selection);
    int selected = initial_selection;
    int chosen_item = -1;

    while (chosen_item < 0) {
        int key = ui->WaitKey();
        int visible = ui->IsTextVisible();

        if (key == -1) {   // ui_wait_key() timed out
            if (ui->WasTextEverVisible()) {
                continue;
            } else {
                LOGI("timed out waiting for key input; rebooting.\n");
                ui->EndMenu();
                return 0; // XXX fixme
            }
        }

        int action = device->HandleMenuKey(key, visible);

        if (action < 0) {
            switch (action) {
                case Device::kHighlightUp:
                    --selected;
                    selected = ui->SelectMenu(selected);
                    break;
                case Device::kHighlightDown:
                    ++selected;
                    selected = ui->SelectMenu(selected);
                    break;
                case Device::kInvokeItem:
                    chosen_item = selected;
                    break;
                case Device::kNoAction:
                    break;
            }
        } else if (!menu_only) {
            chosen_item = action;
        }
    }

    ui->EndMenu();
    return chosen_item;
}
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
static int select_update(Device* device, const char *path, char *new_path) {
    ensure_path_mounted(path);
    static const char** title_headers = NULL;

    if (title_headers == NULL) {
        const char* headers[] = { "Packages stored in root folder,",
                                  "choose one to install",
                                  "",
                                  NULL };
        title_headers = prepend_title(headers);
    }

    DIR* d;
    struct dirent* de;
    int i = 0;
    char** updates = (char**)malloc(11 * sizeof(char*));//not need so many options, 10 is enough
    updates[i++] = strdup("Cancel");

    d = opendir(path);
    if (d != NULL) {
        while ((de = readdir(d)) != NULL &&  i < 10) {
            if (de->d_type == DT_REG && strstr(de->d_name, "JSU")
                  && strstr(de->d_name,".zip"))//"JSU" + ".zip"
            updates[i++] = strdup(de->d_name);
        }

        closedir(d);
    }
    updates[i] = NULL;//termination

    int chosen_item = get_menu_selection(title_headers, updates, 1, 0, device);

    if(chosen_item == 0){
        free(updates);
        return -1;
    }else{
        strlcpy(new_path, SDCARD_ROOT, PATH_MAX);
        strlcat(new_path, "/", PATH_MAX);
        strlcat(new_path, updates[chosen_item], PATH_MAX);
        free(updates);
        return 0;
    }
}

static struct fota_cookie fota_status;
static int select_fota_status_operation(Device* device) {
  int chosen_item=-1;
    static const char** title_headers = NULL;

    if (title_headers == NULL) {
        const char* headers[] = { "Select the update.zip ",
                                    NULL };
        title_headers = prepend_title(headers);
    }

    const char* items[] = { " 1 - read fota cookie",
                            " 2 - set fota cookie",
                            " 3 - reset fota cookie",
                            " 4 - cancel",
                            NULL };

    chosen_item = get_menu_selection(title_headers, items, 1, 0, device);
    if(chosen_item==0){
        if(read_fota_cookie_mmc(&fota_status))
        {
          ui->Print("read fota cookie fail");
        }else
        {
          ui->Print("read fota cookie = 0x%x%x%x%x", fota_status.value[0],fota_status.value[1],fota_status.value[2],fota_status.value[3]);
        }
     }else if(chosen_item==1){
        if(set_fota_cookie_mmc())
        {
          ui->Print("set fota cookie fail");
        }else
        {
          ui->Print("set fota cookie success");
        }
    }else if(chosen_item==2){
        if(reset_fota_cookie_mmc())
        {
          ui->Print("reset fota cookie fail");
        }else
        {
          ui->Print("reset fota cookie success");
        }
    }else{
          return 0;
    }

    return 0;
}
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */
static int compare_string(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int
check_avphys_mem(const char *path) {
    int fd;
    int res;
    struct stat buf;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    res = fstat(fd, &buf);
    if (res) {
        close(fd);
        return -1;
    }

    if (GET_AVPHYS_MEM() > (buf.st_size + RESERVED_MEMORY_SIZE)) {
        res = 0;
    } else {
        res = -1;
    }

    close(fd);
    return res;
}

static int
update_directory(const char* path, const char* unmount_when_done,
                 int* wipe_cache, Device* device) {
    ensure_path_mounted(path);

    const char* MENU_HEADERS[] = { "Choose a package to install:",
                                   path,
                                   "",
                                   NULL };
    DIR* d;
    struct dirent* de;
    d = opendir(path);
    if (d == NULL) {
        LOGE("error opening %s: %s\n", path, strerror(errno));
        if (unmount_when_done != NULL) {
            ensure_path_unmounted(unmount_when_done);
        }
        return 0;
    }

    const char** headers = prepend_title(MENU_HEADERS);

    int d_size = 0;
    int d_alloc = 10;
    char** dirs = (char**)malloc(d_alloc * sizeof(char*));
    int z_size = 1;
    int z_alloc = 10;
    char** zips = (char**)malloc(z_alloc * sizeof(char*));
    zips[0] = strdup("../");

    while ((de = readdir(d)) != NULL) {
        int name_len = strlen(de->d_name);

        if (de->d_type == DT_DIR) {
            // skip "." and ".." entries
            if (name_len == 1 && de->d_name[0] == '.') continue;
            if (name_len == 2 && de->d_name[0] == '.' &&
                de->d_name[1] == '.') continue;

            if (d_size >= d_alloc) {
                d_alloc *= 2;
                dirs = (char**)realloc(dirs, d_alloc * sizeof(char*));
            }
            dirs[d_size] = (char*)malloc(name_len + 2);
            strcpy(dirs[d_size], de->d_name);
            dirs[d_size][name_len] = '/';
            dirs[d_size][name_len+1] = '\0';
            ++d_size;
        } else if (de->d_type == DT_REG &&
                   name_len >= 4 &&
                   strncasecmp(de->d_name + (name_len-4), ".zip", 4) == 0) {
            if (z_size >= z_alloc) {
                z_alloc *= 2;
                zips = (char**)realloc(zips, z_alloc * sizeof(char*));
            }
            zips[z_size++] = strdup(de->d_name);
        }
    }
    closedir(d);

    qsort(dirs, d_size, sizeof(char*), compare_string);
    qsort(zips, z_size, sizeof(char*), compare_string);

    // append dirs to the zips list
    if (d_size + z_size + 1 > z_alloc) {
        z_alloc = d_size + z_size + 1;
        zips = (char**)realloc(zips, z_alloc * sizeof(char*));
    }
    memcpy(zips + z_size, dirs, d_size * sizeof(char*));
    free(dirs);
    z_size += d_size;
    zips[z_size] = NULL;

    int result;
    int chosen_item = 0;
    do {
        chosen_item = get_menu_selection(headers, zips, 1, chosen_item, device);

        char* item = zips[chosen_item];
        int item_len = strlen(item);
        if (chosen_item == 0) {          // item 0 is always "../"
            // go up but continue browsing (if the caller is update_directory)
            result = -1;
            break;
        } else if (item[item_len-1] == '/') {
            // recurse down into a subdirectory
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);
            new_path[strlen(new_path)-1] = '\0';  // truncate the trailing '/'
            result = update_directory(new_path, unmount_when_done, wipe_cache, device);
            if (result >= 0) break;
        } else {
            // selected a zip file:  attempt to install it, and return
            // the status to the caller.
            char new_path[PATH_MAX];
            strlcpy(new_path, path, PATH_MAX);
            strlcat(new_path, "/", PATH_MAX);
            strlcat(new_path, item, PATH_MAX);

            ui->Print("\n-- Install %s ...\n", path);
            set_sdcard_update_bootloader_message();

            if (!check_avphys_mem(new_path)) {
                char* copy = copy_sideloaded_package(new_path);
                if (unmount_when_done != NULL) {
                    ensure_path_unmounted(unmount_when_done);
                }
                if (copy) {
                    result = install_package(copy, wipe_cache, TEMPORARY_INSTALL_FILE);
                    free(copy);
                } else {
                    result = INSTALL_ERROR;
                }
            } else {
                result = install_package(new_path, wipe_cache, TEMPORARY_INSTALL_FILE);
                if (unmount_when_done != NULL) {
                    ensure_path_unmounted(unmount_when_done);
                }
            }
            break;
        }
    } while (true);

    int i;
    for (i = 0; i < z_size; ++i) free(zips[i]);
    free(zips);
    free(headers);

    if (unmount_when_done != NULL) {
        ensure_path_unmounted(unmount_when_done);
    }
    return result;
}

static void
wipe_data(int confirm, Device* device) {
    if (confirm) {
        static const char** title_headers = NULL;

        if (title_headers == NULL) {
            const char* headers[] = { "Confirm wipe of all user data?",
                                      "  THIS CAN NOT BE UNDONE.",
                                      "",
                                      NULL };
            title_headers = prepend_title((const char**)headers);
        }

        const char* items[] = { " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " No",
                                " Yes -- delete all user data",   // [7]
                                " No",
                                " No",
                                " No",
                                NULL };

        int chosen_item = get_menu_selection(title_headers, items, 1, 0, device);
        if (chosen_item != 7) {
            return;
        }
    }

    ui->Print("\n-- Wiping data...\n");
    device->WipeData();
	save_userdata_ver_info();//czb@tcl.com save userdata.ver
    erase_volume("/data");
    erase_volume("/cache");
	restore_userdata_ver_info();//czb@tcl.com restore userdata.ver
    ui->Print("Data wipe complete.\n");
}

static void
prompt_and_wait(Device* device, int status) {
    const char* const* headers = prepend_title(device->GetMenuHeaders());

    for (;;) {
        finish_recovery(NULL);
        switch (status) {
            case INSTALL_SUCCESS:
            case INSTALL_NONE:
                ui->SetBackground(RecoveryUI::NO_COMMAND);
                break;

            case INSTALL_ERROR:
            case INSTALL_CORRUPT:
            //FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, begin
            case INSTALL_NO_SDCARD:
            case INSTALL_NO_UPDATE_PACKAGE:
            case INSTALL_NO_KEY:
            case INSTALL_SIGNATURE_ERROR:
            //FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, end
                ui->SetBackground(RecoveryUI::ERROR);
                break;
        }
        ui->SetProgressType(RecoveryUI::EMPTY);

        int chosen_item = get_menu_selection(headers, device->GetMenuItems(), 0, 0, device);

        // device-specific code may take some action here.  It may
        // return one of the core actions handled in the switch
        // statement below.
        chosen_item = device->InvokeMenuItem(chosen_item);

        int wipe_cache;
        switch (chosen_item) {
            case Device::REBOOT:
                return;

            case Device::WIPE_DATA:
                wipe_data(ui->IsTextVisible(), device);
                if (!ui->IsTextVisible()) return;
                break;

            case Device::WIPE_CACHE:
                ui->Print("\n-- Wiping cache...\n");
                erase_volume("/cache");
                ui->Print("Cache wipe complete.\n");
                if (!ui->IsTextVisible()) return;
                break;

            case Device::APPLY_EXT:
                status = update_directory(SDCARD_ROOT, SDCARD_ROOT, &wipe_cache, device);
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui->Print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui->Print("Cache wipe failed.\n");
                    } else {
                        ui->Print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from sdcard complete.\n");
                    }
                }
                break;

            case Device::APPLY_CACHE:
                // Don't unmount cache at the end of this.
                status = update_directory(CACHE_ROOT, NULL, &wipe_cache, device);
                if (status == INSTALL_SUCCESS && wipe_cache) {
                    ui->Print("\n-- Wiping cache (at package request)...\n");
                    if (erase_volume("/cache")) {
                        ui->Print("Cache wipe failed.\n");
                    } else {
                        ui->Print("Cache wipe complete.\n");
                    }
                }
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from cache complete.\n");
                    }
                }
                break;

            case Device::APPLY_ADB_SIDELOAD:
                status = apply_from_adb(ui, &wipe_cache, TEMPORARY_INSTALL_FILE);
                if (status >= 0) {
                    if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                        copy_logs();
                    } else if (!ui->IsTextVisible()) {
                        return;  // reboot if logs aren't visible
                    } else {
                        ui->Print("\nInstall from ADB complete.\n");
                    }
                }
                break;
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
            case Device::APPLY_UPDATE:
                  char* new_path = (char*)malloc(PATH_MAX);
                  memset(new_path, 0, sizeof(new_path));
                  if(!select_update(device, SDCARD_ROOT,new_path)) {
                     set_sdcard_update_bootloader_message();
                     status = install_package(new_path, &wipe_cache, TEMPORARY_INSTALL_FILE);
                     if (status >= 0) {
                        if (status != INSTALL_SUCCESS) {
                        ui->SetBackground(RecoveryUI::ERROR);
                        ui->Print("Installation aborted.\n");
                        } else if (!ui->IsTextVisible()) {
                          return;  // reboot if logs aren't visible
                        } else {
                          ui->Print("\nInstall from SD card complete.\n");
                        }
                     }
                  }
                  free(new_path);
                break;
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */
        }
    }
}

static void
print_property(const char *key, const char *name, void *cookie) {
    printf("%s=%s\n", key, name);
}

static void
load_locale_from_cache() {
    FILE* fp = fopen_path(LOCALE_FILE, "r");
    char buffer[80];
    if (fp != NULL) {
        fgets(buffer, sizeof(buffer), fp);
        int j = 0;
        unsigned int i;
        for (i = 0; i < sizeof(buffer) && buffer[i]; ++i) {
            if (!isspace(buffer[i])) {
                buffer[j++] = buffer[i];
            }
        }
        buffer[j] = 0;
        locale = strdup(buffer);
        check_and_fclose(fp, LOCALE_FILE);
    }
}
//[FEATURE]-ADD-BEGIN by Xiaoting.He, 2013/03/13, FR-400298,FOTA porting
//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, begin
#ifdef FEATURE_TCT_FOTA
//#define FOTA_STATUS_FAILED "FAILED"
//#define FOTA_STATUS_SUCCESS "SUCCESS"

//#define fota_set_status_success() fota_set_status(FOTA_STATUS_SUCCESS)
//#define fota_set_status_failed() fota_set_status(FOTA_STATUS_FAILED)

static void fota_reset_status(void) {
    //if(unlink(FOTA_STATUS_FILE)){
    if(unlink(JRD_FOTA_RESULT_FILE)){
        LOGE("erasing fota.status failed errno=%d\n",errno);
    }
}

/*static void fota_set_status(char* status) {
    FILE *fp;
    int expected = strlen("[APPS_UPDATE_STATUS]=")+strlen(status)+strlen("\n");

    fp=fopen_path(FOTA_STATUS_FILE, "w");
    if(fp==NULL) {
        LOGE("can't open fota.status for writing errno=%d\n",errno);
        return;
    }

    if (fprintf(fp,"[APPS_UPDATE_STATUS]=%s\n",status)!=expected) {
        LOGE("error while writing fota.status errno=%d\n",errno);
        fclose(fp);
        return;
    }

    fclose(fp);
}*/
// JRD
#define INS_ABORT "Installation aborted"
#define SCR_ABORT "script aborted"
#define ASSERT_FAIL "assert failed"

#define ERR_UNDEF "Undefine"

char *err_mesg[] = {
    "ro.build.fingerprint",
    "ro.product.device",
    "ro.build.date.utc",
    "apply_patch_check",
    "apply_patch_space",
    "sha1_check",
    "run_program",
    "package_extract_file",
    NULL
};

#include "minzip/SysUtil.h"
#include "minzip/Zip.h"

#define SCRIPT_NAME "META-INF/com/google/android/updater-script"

static char *fota_incr_comment = "# ---- start making changes here ----";

enum
{
    FOTA_UN,
    FOTA_INCR,
    FOTA_FULL,
};

static char *parse_err(char *buf)
{
    char *ret = NULL;

    ret = strstr(buf, SCR_ABORT);

    if (ret == NULL) {
        ret = ERR_UNDEF;
    } else {
        int i;
        for (i = 0; err_mesg[i] != NULL; i++) {
            if (strstr(ret, err_mesg[i])) {
                ret = err_mesg[i];
                break;
            }
        }

        if (err_mesg[i] == NULL)
            ret = ERR_UNDEF;
    }

    return ret;
}

static char *get_update_error()
{
    char *ret = "NULL";
    char *buf = NULL;
    int len;

    FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
    if (tmplog != NULL) {
        fseek(tmplog, 0, SEEK_END);
        len = ftell(tmplog);
        buf =(char *) malloc(len);
        if (buf != NULL) {
            fseek(tmplog, 0, SEEK_SET);
            len = fread(buf, 1, len, tmplog);
            ret = parse_err(buf);
            free(buf);
        }

        fclose(tmplog);
    }

    return ret;
}

static int kind_of_update = FOTA_UN;
void get_update_kind(const char *update_package)
{
    int k = FOTA_UN;
    ZipArchive za;
    int err;

    err = mzOpenZipArchive(update_package, &za);
    if (err != 0) {
        fprintf(stderr, "Can't open %s\n(%s)\n", update_package, err != -1 ? strerror(err) : "bad");
        return;
    }

    const ZipEntry* script_entry = mzFindZipEntry(&za, SCRIPT_NAME);
    if (script_entry == NULL) {
        fprintf(stderr, "Can't find %s\n", SCRIPT_NAME);
        return;
    }

    char* script = (char *)malloc(script_entry->uncompLen+1);
    if (!mzReadZipEntry(&za, script_entry, script, script_entry->uncompLen)) {
        fprintf(stderr, "Can't read %s\n", SCRIPT_NAME);
        return;
    }
    script[script_entry->uncompLen] = '\0';

    char* p;
    for (p = script; *p != '\0'; p++) {
        if (*p == '#') {
            if (strcmp(p, fota_incr_comment) >= 0) {
                k = FOTA_INCR;
                break;
            }
        }
    }

    // not find incremental ota comment, it's full ota
    if (*p == '\0')
        k = FOTA_FULL;

    free(script);
    mzCloseZipArchive(&za);

    kind_of_update = k;
}

static void write_file(const char *file_name, int reason, char *result)
{
    char  dir_name[256];

    ensure_path_mounted("/data");

    strcpy(dir_name, file_name);
    char *p = strrchr(dir_name, '/');
    *p = 0;

    fprintf(stdout, "dir_name = %s\n", dir_name);
    mode_t proc_mask = umask(0);

    if (opendir(dir_name) == NULL)  {
        fprintf(stdout, "dir_name = '%s' does not exist, create it.\n", dir_name);
        if (mkdir(dir_name, 0775))  {
            fprintf(stdout, "can not create '%s' : %s\n", dir_name, strerror(errno));
            umask(proc_mask);
            return;
        }
        chown(dir_name, AID_SYSTEM, AID_SYSTEM);
    }

    int result_fd = open(file_name, O_RDWR | O_CREAT | O_TRUNC, 0666);
    fchown(result_fd, AID_SYSTEM, AID_SYSTEM);

    if (result_fd < 0) {
        fprintf(stdout, "cannot open '%s' for output : %s\n", file_name, strerror(errno));
        umask(proc_mask);
        return;
    }

    //LOG_INFO("[%s] %s %d\n", __func__, file_name, result);

    char buf[256];
    strcpy(buf, "package install result:");
    strcat(buf, result);

    if (reason == INSTALL_ERROR) {
        // strcat(buf, "package install error: ");
        strcat(buf, get_update_error());
        strcat(buf, "\n");
    }

    if (kind_of_update == FOTA_FULL) {
        strcat(buf, "package install kind:full package\n");
    }
    write(result_fd, buf, strlen(buf));
    close(result_fd);
    umask(proc_mask);
}

static char *install_result = NULL;
static void set_install_result(char *s)
{
    install_result = s;
}

static void write_result(int reason)
{
    switch (reason) {
    case INSTALL_NO_SDCARD:
        set_install_result("Update.zip is not correct:No SD-Card.\n");
        break;
    case INSTALL_NO_UPDATE_PACKAGE:
        set_install_result("Update.zip is not correct:Can not find update.zip.\n");
        break;
    case INSTALL_NO_KEY:
        set_install_result("Update.zip is not correct:Failed to load keys\n");
        break;
    case INSTALL_SIGNATURE_ERROR:
        set_install_result("Update.zip is not correct:Signature verification failed\n");
        break;
    case INSTALL_CORRUPT:
        set_install_result("Update.zip is not correct:The update.zip is corrupted\n");
        break;
    //case INSTALL_FILE_SYSTEM_ERROR:
        //set_install_result("Update.zip is not correct:Can't create/copy file\n");
        //break;
    case INSTALL_ERROR:
        set_install_result("Update.zip is not correct:");
        break;
    }

    if (reason == INSTALL_SUCCESS) {
        set_install_result("INSTALL SUCCESS\n");
    }

    fprintf(stdout, "package install result:%s", install_result);

    write_file(JRD_FOTA_RESULT_FILE, reason, install_result);
}
// JRD end
//FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, end
//[FEATURE]-ADD-BEGIN by WRX, 2013/05/27, CR-447694, FULL UPDATE DEV
static int full_update(char const *name) {
    const char* key = "JSU";

    if(strstr(name, key)) {
       return 1;
    }else {
       return 0;
    }

}
//[FEATURE]-ADD-END by WRX

#endif
//[FEATURE]-ADD-END by Xiaoting.He, 2013/03/13, FR-400298,FOTA porting

static RecoveryUI* gCurrentUI = NULL;

void
ui_print(const char* format, ...) {
    char buffer[256];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    if (gCurrentUI != NULL) {
        gCurrentUI->Print("%s", buffer);
    } else {
        fputs(buffer, stdout);
    }
}

int
main(int argc, char **argv) {
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);

    // If this binary is started with the single argument "--adbd",
    // instead of being the normal recovery binary, it turns into kind
    // of a stripped-down version of adbd that only supports the
    // 'sideload' command.  Note this must be a real argument, not
    // anything in the command file or bootloader control block; the
    // only way recovery should be run with this argument is when it
    // starts a copy of itself from the apply_from_adb() function.
    if (argc == 2 && strcmp(argv[1], "--adbd") == 0) {
        adb_main();
        return 0;
    }

    printf("Starting recovery on %s", ctime(&start));

    load_volume_table();
    ensure_path_mounted(LAST_LOG_FILE);
    rotate_last_logs(10);
    get_args(&argc, &argv);

    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0, show_text = 0;
    int clr_format_bit = 0;
    bool just_exit = false;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'f': wipe_data = wipe_cache = clr_format_bit = 1; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case 't': show_text = 1; break;
        case 'x': just_exit = true; break;
        case 'l': locale = optarg; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    if (locale == NULL) {
        load_locale_from_cache();
    }
    printf("locale is [%s]\n", locale);

    Device* device = make_device();
    ui = device->GetUI();
    gCurrentUI = ui;

    ui->Init();
    ui->SetLocale(locale);
    ui->SetBackground(RecoveryUI::NONE);
    if (show_text) ui->ShowText(true);

    struct selinux_opt seopts[] = {
      { SELABEL_OPT_PATH, "/file_contexts" }
    };

    sehandle = selabel_open(SELABEL_CTX_FILE, seopts, 1);

    if (!sehandle) {
        ui->Print("Warning: No file_contexts\n");
    }

    device->StartRecovery();

    printf("Command:");
    for (arg = 0; arg < argc; arg++) {
        printf(" \"%s\"", argv[arg]);
    }
    printf("\n");

    if (update_package) {
        // For backwards compatibility on the cache partition only, if
        // we're given an old 'root' path "CACHE:foo", change it to
        // "/cache/foo".
        if (strncmp(update_package, "CACHE:", 6) == 0) {
            int len = strlen(update_package) + 10;
            char* modified_path = (char*)malloc(len);
            strlcpy(modified_path, "/cache/", len);
            strlcat(modified_path, update_package+6, len);
            printf("(replacing path \"%s\" with \"%s\")\n",
                   update_package, modified_path);
            update_package = modified_path;
        }
         /*
         *Modified by baijian 2014-02-12
         *extern sdcard location is /storage/sdcard1 in normal model,
         *but in recovery model,it should be locate in the /sdcard.
         *begin
         */
        else if (strncmp(update_package, "/storage/sdcard1", 16) == 0) {
            int len = strlen(update_package) + 20;
            char* modified_path = (char*)malloc(len);
            memset(modified_path, 0, len);

            if(! ensure_path_mounted("/sdcard")) {
                strlcpy(modified_path, "/sdcard", len);
                ensure_path_unmounted("/sdcard");
            }

            strlcat(modified_path, update_package+16, len-16+1);
            fprintf(stdout, "(replacing path \"%s\" with \"%s\")\n",
                   update_package, modified_path);
            update_package = modified_path;
            ensure_path_mounted("/data");
        }
        /*
         *Modified by baijian 2014-02-12
         *extern sdcard location is /storage/sdcard1 in normal model,
         *but in recovery model,it should be located in the /sdcard
         *end
         */
    }
    printf("\n");

    property_list(print_property, NULL);
    property_get("ro.build.display.id", recovery_version, "");
    printf("\n");

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
#ifdef FEATURE_TCT_FOTA
    fota_reset_status();
#endif
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */
   /*Added by baijian 2014-01-13 before update ensure the cache,system and so on is unmounted begin*/
        ensure_path_unmounted(CACHE_ROOT);
        ensure_path_unmounted("/system");
   /*Added by baijian 2014-01-13 before update ensure the cache,system and so on is unmounted end*/
        /*Added by tcl_baijian fixed bug#616327 free cache space 2014-03-19 begin*/
        //erase_volume(CACHE_ROOT);/*case is enought, need not to erase*/
        /*Added by tcl_baijian fixed bug#616327 free cache space 2014-03-19 end*/
        status = install_package(update_package, &wipe_cache, TEMPORARY_INSTALL_FILE);
        if (status == INSTALL_SUCCESS && wipe_cache) {
            if (erase_volume("/cache")) {
                LOGE("Cache wipe (requested by package) failed.");
            }
        }
        if (status != INSTALL_SUCCESS) {
            ui->Print("Installation aborted.\n");

            // If this is an eng or userdebug build, then automatically
            // turn the text display on if the script fails so the error
            // message is visible.
            char buffer[PROPERTY_VALUE_MAX+1];
            property_get("ro.build.fingerprint", buffer, "");
            if (strstr(buffer, ":userdebug/") || strstr(buffer, ":eng/")) {
                ui->ShowText(true);
            }
       }
/*[FEATURE]-MOD-BEGIN by TCTNB.XLJ, 2012/12/13, PR-314522,FOTA porting*/
#ifdef FEATURE_TCT_FOTA
        if (status != INSTALL_SUCCESS)
        {
            ui->Print("Installation aborted.\n");
            //fota_set_status_failed();
        }else
        {
            ui->Print("Installation success.\n");
            //fota_set_status_success();
        }
        //FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, begin
        //JRD need upload the error code to GOTU server
        get_update_kind(update_package);
        write_result(status);
        //FR-550496, Add by changmei.chen@tcl.com for JrdFota upload error code to GOTU server , 2013-11-18, end
//JRD end
//[FEATURE]-ADD-BEGIN by WRX, 2013/05/27, CR-447694, FULL UPDATE DEV
//Different from FOTA, full update doesn't need fota.statu file
    if (full_update(update_package)) {
            fota_reset_status();
    }
//[FEATURE]-ADD-END by WRX
#endif


    } else if (wipe_data) {
        if (device->WipeData()) status = INSTALL_ERROR;
		save_userdata_ver_info();//czb@tcl.com save userdata.ver
        if (erase_volume("/data")) status = INSTALL_ERROR;
		restore_userdata_ver_info();//czb@tcl.com restore userdata.ver
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui->Print("Data wipe failed.\n");
#ifdef MSM_FIRST_FORMAT
        if (status == INSTALL_SUCCESS && clr_format_bit) {
            if(clear_format_bit())
                ui->Print("clear first format bit failed\n");
        }
#endif
    } else if (wipe_cache) {
        if (wipe_cache && erase_volume("/cache")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui->Print("Cache wipe failed.\n");
    } else if (!just_exit) {
        status = INSTALL_NONE;  // No command specified
        ui->SetBackground(RecoveryUI::NO_COMMAND);
    }

    if (status == INSTALL_ERROR || status == INSTALL_CORRUPT) {
        copy_logs();
        ui->SetBackground(RecoveryUI::ERROR);
    }
    if (status != INSTALL_SUCCESS || ui->IsTextVisible()) {
        ui->ShowText(true);
        prompt_and_wait(device, status);
    }

    // Otherwise, get ready to boot the main system...
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting begin */
FINISHRECOVERY:
/*[FEATURE]-ADD by ling.yi@jrdcom.com, 2013/11/08, Bug 550459, FOTA porting end */

    finish_recovery(send_intent);
    ui->Print("Rebooting...\n");
    property_set(ANDROID_RB_PROPERTY, "reboot,");
    return EXIT_SUCCESS;
}
