/*
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *  * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *  * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <linux/if.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define LOG_TAG "QCLDR-"

#include "cutils/log.h"
#include "cutils/memory.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "private/android_filesystem_config.h"

#include "qsap_api.h"
#include "qsap.h"
#include "libwpa_client/wpa_ctrl.h"

#include <sys/system_properties.h>

#ifndef WIFI_DRIVER_MODULE_PATH
#define WIFI_DRIVER_MODULE_PATH         "/system/lib/modules/wlan.ko"
#endif

#ifndef WIFI_DRIVER_MODULE_NAME
#define WIFI_DRIVER_MODULE_NAME         "wlan"
#endif

#ifdef WIFI_DRIVER_MODULE_ARG
#undef WIFI_DRIVER_MODULE_ARG
#endif

#define WIFI_DRIVER_MODULE_ARG          ""

/* WIFI_SDIO_IF_DRIVER_MODULE_NAME must be defined if sdioif driver required */
#ifdef WIFI_SDIO_IF_DRIVER_MODULE_NAME

#ifndef WIFI_SDIO_IF_DRIVER_MODULE_PATH
#define WIFI_SDIO_IF_DRIVER_MODULE_PATH "/system/lib/modules/librasdioif.ko"
#endif


#ifndef WIFI_SDIO_IF_DRIVER_MODULE_ARG 
#define WIFI_SDIO_IF_DRIVER_MODULE_ARG  ""
#endif

#endif


extern int init_module(const char *name, u32, const s8 *);
extern int delete_module(const char *name, int);

extern struct Command qsap_str[];

static s32 check_driver_loaded( const s8 * tag)
{
    FILE *proc;
    s8   line[126];

    if ((proc = fopen("/proc/modules", "r")) == NULL) {
        LOGW("Could not open %s: %s", "/proc/modules", strerror(errno));
        return 0;
    }

    while ((fgets(line, sizeof(line), proc)) != NULL) {
        if (strncmp(line, tag, strlen(tag)) == 0) {
            fclose(proc);
            return 1;
        }
    }

    fclose(proc);

    return 0;
}

static s32 insmod(const s8 *filename, const s8 *args, const s8 * tag)
{
#ifndef SDK_TEST
    void *module;
    s32 size;
    s32 ret = 0;

    if ( check_driver_loaded(tag) ) {
        LOGE("Driver: %s already loaded\n", filename);
        return ret;
    }

    LOGD("Loading Driver: %s %s\n", filename, args);

    module = (void*)load_file(filename, (unsigned int*)&size);

    if (!module) {
        LOGE("Cannot load file: %s\n", filename);
        return -1;
    }

    ret = init_module(module, size, args);

    if ( ret ) {
        LOGE("init_module (%s:%d) failed\n", filename, (int)size);
    }

    free(module);

    return ret;
#else
    return 0;
#endif
}

static s32 rmmod(const s8 *modname)
{
#ifndef SDK_TEST
    s32 ret = 0;
    s32 maxtry = 10;

    while (maxtry-- > 0) {
        ret = delete_module(modname, O_NONBLOCK | O_EXCL);

        if (ret < 0 && errno == EAGAIN){
            usleep(50000);
        } else {
            break;
        }
    }

    if (ret != 0) {
        LOGD("Unable to unload driver module \"%s\": %s\n",
                    modname, strerror(errno));
    }

    return ret;
#else
    return 0;
#endif
}

static const s8 SDIO_POLLING_ON[]     = "/etc/init.qcom.sdio.sh 1";
static const s8 SDIO_POLLING_OFF[]    = "/etc/init.qcom.sdio.sh 0";

s32 wifi_qsap_load_driver(void)
{
    s32    size;
    s32        ret = 0;
    s32        retry;


    if (system(SDIO_POLLING_ON)) {
        LOGE("Could not turn on the polling...");
    }

#ifdef WIFI_SDIO_IF_DRIVER_MODULE_NAME
    ret = insmod(WIFI_SDIO_IF_DRIVER_MODULE_PATH, WIFI_SDIO_IF_DRIVER_MODULE_ARG, WIFI_SDIO_IF_DRIVER_MODULE_NAME " ");

    if ( ret != 0 ) {
        LOGE("init_module failed sdioif\n");
        ret = eERR_LOAD_FAILED_SDIOIF;
        goto end;
    }

    sched_yield();
#endif

    ret = insmod(WIFI_DRIVER_MODULE_PATH, WIFI_DRIVER_MODULE_ARG, WIFI_DRIVER_MODULE_NAME " ");

    if ( ret != 0 ) {
#ifdef WIFI_SDIO_IF_DRIVER_MODULE_NAME
        if ( check_driver_loaded(WIFI_SDIO_IF_DRIVER_MODULE_NAME " ") ) {
            if ( rmmod(WIFI_SDIO_IF_DRIVER_MODULE_NAME) ) {
                LOGE("Unable to unload the station mode librasdioif driver\n");
            }
        }
#endif
        LOGE("init_module failed libra_softap\n");
        ret = eERR_LOAD_FAILED_SOFTAP;
		goto end;
    }

    sched_yield();
    
	ret = eSUCCESS;

end:
    if(system(SDIO_POLLING_OFF)) {
        LOGE("Could not turn off the polling...");
    }

    return ret;
}


void qsap_send_module_down_indication(void)
{
    int s, ret;
    struct iwreq wrq;

    /*
     * If the driver is loaded, ask it to broadcast a netlink message
     * that it will be closing, so listeners can close their sockets.
     *
     */


     /* Equivalent to: iwpriv wlan0 sendModuleInd */
     if ((s = socket(PF_INET, SOCK_DGRAM, 0)) >= 0) {
        strncpy(wrq.ifr_name, "wlan0", IFNAMSIZ);
        wrq.u.data.length = 0; /* No Set arguments */
        wrq.u.mode = 5; /* WE_MODULE_DOWN_IND sub-command */
        ret = ioctl(s, (SIOCIWFIRSTPRIV + 1), &wrq);
        if (ret < 0 ) {
            strncpy(wrq.ifr_name, "softap.0", IFNAMSIZ);
            ret = ioctl(s, (SIOCIWFIRSTPRIV + 1), &wrq);
            if (ret < 0 ) {
               LOGE("ioctl failed: %s", strerror(errno));
            }
        }
        close(s);
        sched_yield();
     }
     else {
        LOGE("Socket open failed: %s", strerror(errno));
     }
}


s32 qsap_send_init_ap(void)
{
    int s, ret;
    struct iwreq wrq;
    s32 status = eSUCCESS;
    u32 *params = (u32 *)&wrq.u;

     /* Equivalent to: iwpriv wlan0 initAP */
     if ((s = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
        strncpy(wrq.ifr_name, "wlan0", IFNAMSIZ);
        wrq.u.data.length = 0; /* No Set arguments */
        wrq.u.data.flags = 2; /* WE_INIT_AP sub-command */
        ret = ioctl(s, (SIOCIWFIRSTPRIV + 6), &wrq);
        if (ret < 0 ) {
           LOGE("ioctl failed: %s", strerror(errno));
           status = eERR_START_SAP;
        }
        close(s);
        sched_yield();
     }
     else {
        LOGE("Socket open failed: %s", strerror(errno));
        status = eERR_START_SAP;
     }
     return status;
}


s32 qsap_send_exit_ap(void)
{
    int s, ret;
    struct iwreq wrq;
    s32 status = eSUCCESS;
    u32 *params = (u32 *)&wrq.u;

     /* Equivalent to: iwpriv wlan0 exitAP */
     if ((s = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) {
        strncpy(wrq.ifr_name, "wlan0", IFNAMSIZ);
        wrq.u.data.length = 0; /* No Set arguments */
        wrq.u.data.flags = 3;  /*WE_EXIT_AP sub-command */

        ret = ioctl(s, (SIOCIWFIRSTPRIV + 6), &wrq);
        if (ret < 0 ) {
            LOGE("ioctl failed: %s", strerror(errno));
            status = eERR_STOP_SAP;
        }
        close(s);
        sched_yield();
     }
     else {
        LOGE("Socket open failed: %s", strerror(errno));
        status = eERR_STOP_SAP;
     }
     return status;
}

s32 wifi_qsap_unload_driver()
{
    s32 ret = eSUCCESS;

    if(system(SDIO_POLLING_ON)) {
        LOGE("Could not turn on the polling...");
    }

    if ( check_driver_loaded(WIFI_DRIVER_MODULE_NAME " ") ) {
        qsap_send_module_down_indication();
        if ( rmmod(WIFI_DRIVER_MODULE_NAME) ) {
            LOGE("Unable to unload the libra_softap driver\n");
            ret = eERR_UNLOAD_FAILED_SOFTAP;
            goto end;
        }
    }

    sched_yield();

#ifdef WIFI_SDIO_IF_DRIVER_MODULE_NAME
    if ( check_driver_loaded(WIFI_SDIO_IF_DRIVER_MODULE_NAME " ") ) {
        if ( rmmod(WIFI_SDIO_IF_DRIVER_MODULE_NAME) ) {
            LOGE("Unable to unload the librasdioif driver\n");
            ret = eERR_UNLOAD_FAILED_SDIO;
            goto end;
        }
    }
#endif

end:
    if(system(SDIO_POLLING_OFF)) {
        LOGE("Could not turn off the polling...");
    }

    return ret;
}

s32 wifi_qsap_stop_bss(void)
{
#define QCIEEE80211_IOCTL_STOPBSS   (SIOCIWFIRSTPRIV + 6)
    s32 sock;
    s32 ret = eERR_STOP_BSS;
    s8  cmd[] = "stopbss";
    s8  interface[128];
    s8  *iface;
    s32 len = 128;
    struct iwreq wrq;
    struct iw_priv_args *priv_ptr;

    if(ENABLE != is_softap_enabled()) {
        ret = eERR_BSS_NOT_STARTED;
        return ret;
    }

    if(NULL == (iface = qsap_get_config_value(CONFIG_FILE, &qsap_str[STR_INTERFACE], interface, (u32*)&len))) {
        LOGE("%s :interface error \n", __func__);
        return ret;
    }

    /* Issue the stopbss command to driver */
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        LOGE("Failed to open socket");
        return eERR_STOP_BSS;
    }

    strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
    wrq.u.data.length = sizeof(cmd);
    wrq.u.data.pointer = cmd;
    wrq.u.data.flags = 0;

    ret = ioctl(sock, QCIEEE80211_IOCTL_STOPBSS, &wrq);

    /* Here IOCTL is always returning non Zero: temporary fix untill driver is fixed*/
    ret = 0;
    close(sock);

    if (ret) {
        LOGE("IOCTL stopbss failed: %ld", ret);
        ret = eERR_STOP_BSS;
    } else {
        LOGD("STOP BSS ISSUED");
        ret = eSUCCESS;
    }

    sched_yield();
    return ret;
}

s32 is_softap_enabled(void)
{
    s8    stat[32] = {0};

    if ( property_get("wifi.hostapd", stat, NULL) &&
                            (strcmp(stat, "1") == 0)) {
        LOGD("HOSTAPD enabled \n");
        return ENABLE;
    }

    LOGD("HOSTAPD disabled \n");
    return DISABLE;
}

s32 commit(void)
{
#ifndef SDK_TEST
    s32 ret = eERR_COMMIT;

    if ( is_softap_enabled() ) {
        /** Stop BSS */
        if(eSUCCESS != (ret = wifi_qsap_stop_bss())) {
            LOGE("%s: stop bss failed \n", __func__);
            return ret;
        }
        sleep(1);
    }

    ret = wifi_qsap_start_softap();

    if( eSUCCESS != ret )
        wifi_qsap_unload_driver();

    return ret;
#else
    return eSUCCESS;
#endif
}

s32 wifi_qsap_start_softap()
{
    s32    retry = 4;
    FILE * fp;

    LOGD("Starting Soft AP...\n");

    /* Check if configuration files are present, if not create the default files */
    check_for_configuration_files();

    /* Delete control interface if it was left over because of previous crash */
    if ( !is_softap_enabled() ) {
        qsap_del_ctrl_iface();
    }

    /* Ensure correct path for ini file name */
    qsap_set_ini_filename();

    while(retry--) {
        /* May be the configuration file is corrupted or not available, */
        /* copy the default configuration file                          */
        if ( retry == 1 )
            wifi_qsap_reset_to_default(CONFIG_FILE, DEFAULT_CONFIG_FILE_PATH);

        /** Stop hostapd */
        if(0 != property_set("ctl.start", "hostapd")) {
            LOGE("failed \n");
            continue;
        }

        sleep(1);

        if ( is_softap_enabled() ) {
            LOGD("success \n");
            return eSUCCESS;
        }
    }

    LOGE("Unable to start the SoftAP\n");
    return eERR_START_SAP;
}

#ifdef QCOM_WLAN_CONCURRENCY
s32 wifi_qsap_start_softap_in_concurrency()
{
    s32 status;
    /*Send initAP IOCTL to Driver. Hostapd start is done by Netd.*/
    status = qsap_send_init_ap();
    return status;
}

s32 wifi_qsap_stop_softap_in_concurrency()
{
    s32 status;
    /*Send exitAP IOCTL to Driver. Hostapd stop is done by Netd.*/
    status = qsap_send_exit_ap();
    return status;
}
#endif


s32 wifi_qsap_stop_softap()
{
    s32 ret = eSUCCESS;

    if ( is_softap_enabled() ) {
        LOGD("Stopping BSS ..... ");

        /** Stop the BSS */
        if (eSUCCESS != (ret = wifi_qsap_stop_bss()) ) {
            LOGE("failed \n");
            return ret;
        }
        sleep(1);
    }

    return ret;
}

s32 wifi_qsap_reload_softap()
{
    s32 ret = eERR_RELOAD_SAP;

    /** SDK API to reload the firmware */
    if (eSUCCESS != (ret = wifi_qsap_stop_softap())) {
        return ret;
    }

    if (eSUCCESS != (ret = wifi_qsap_unload_driver())) {
        return ret;
    }

    usleep(500000);

    if (eSUCCESS != (ret = wifi_qsap_load_driver())) {
        return ret;
    }

    sleep(1);

    if (eSUCCESS != (ret = wifi_qsap_start_softap())) {
        wifi_qsap_unload_driver();
        return ret;
    }

    return eSUCCESS;
}
