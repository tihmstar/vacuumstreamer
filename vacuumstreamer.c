/*
Author: tihmstar <tihmstar(at)gmail.com>
With support of: dgiese <dennis(at)dontvacuum.me>
*/
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <dlfcn.h>

#define bool int

#define PORT 6969
// #define UDP_TARGET "127.0.0.1"

/*
    This website is super useful: https://api-ref.agora.io/en/iot-sdk/linux/1.x/agora__rtc__api_8h.html
*/

//taken from libgeneral ;)
#      define info(a ...) ({printf(a),printf("\n");})
#      define warning(a ...) ({printf("[WARNING] "), printf(a),printf("\n");})
#      define error(a ...) ({printf("[Error] "),printf(a),printf("\n");})
#       ifdef DEBUG
#           define debug(a ...) ({printf("[DEBUG] "),printf(a),printf("\n");})
#       else
#           define debug(a ...)
#       endif
#define cassure(a) do{ if ((a) == 0){err=__LINE__; goto error;} }while(0)
#define cretassure(cond, errstr ...) do{ if ((cond) == 0){err=__LINE__;error(errstr); goto error;} }while(0)
#define creterror(errstr ...) do{error(errstr);err=__LINE__; goto error; }while(0)


#pragma mark types
#define AGORA_RTC_PRODUCT_ID_MAX_LEN (63)
#define AGORA_LICENSE_VALUE_LEN 32

typedef uint32_t connection_id_t;
typedef void audio_frame_info_t;
typedef void video_frame_info_t;

typedef enum { 
    VIDEO_STREAM_HIGH = 0,
    VIDEO_STREAM_LOW = 1
} video_stream_type_e;

typedef enum {
    RTC_LOG_DEFAULT = 0, // the same as RTC_LOG_NOTICE
    RTC_LOG_EMERG,
    RTC_LOG_ALERT, // action must be taken immediately
    RTC_LOG_CRIT, // critical conditions
    RTC_LOG_ERROR, // error conditions
    RTC_LOG_WARNING, // warning conditions
    RTC_LOG_NOTICE, // normal but significant condition, default level
    RTC_LOG_INFO, // informational
    RTC_LOG_DEBUG, // debug-level messages
} rtc_log_level_e;

typedef struct {
    bool log_disable;
    bool log_disable_desensitize;
    rtc_log_level_e log_level;
    const char *log_path;
} log_config_t;

typedef struct {
    uint32_t area_code;
    char product_id[AGORA_RTC_PRODUCT_ID_MAX_LEN + 1];
    log_config_t log_cfg;
    char license_value[AGORA_LICENSE_VALUE_LEN + 1];
} rtc_service_option_t;

typedef struct{
void (*on_join_channel_success)(void *isnull, int elapsed_ms);
void (*on_connection_lost)(void *isnull);
void (*on_rejoin_channel_success)(connection_id_t conn_id, uint32_t uid, int elapsed_ms);
void (*on_error)(connection_id_t conn_id, int code, const char *msg);
void (*on_user_joined)(connection_id_t conn_id, uint32_t uid, int elapsed_ms);
void (*on_user_offline)(connection_id_t conn_id, uint32_t uid, int reason);
void (*on_user_mute_audio)(connection_id_t conn_id, uint32_t uid, bool muted);
#ifndef CONFIG_AUDIO_ONLY
void (*on_user_mute_video)(connection_id_t conn_id, uint32_t uid, bool muted);
#endif
void (*on_audio_data)(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts, const void *data_ptr, size_t data_len, const audio_frame_info_t *info_ptr);
void (*on_mixed_audio_data)(connection_id_t conn_id, const void *data_ptr, size_t data_len, const audio_frame_info_t *info_ptr);
#ifndef CONFIG_AUDIO_ONLY
void (*on_video_data)(connection_id_t conn_id, uint32_t uid, uint16_t sent_ts, const void *data_ptr, size_t data_len, const video_frame_info_t *info_ptr);
void (*on_target_bitrate_changed)(connection_id_t conn_id, uint32_t target_bps);
void (*on_key_frame_gen_req)(connection_id_t conn_id, uint32_t uid, video_stream_type_e stream_type);
#endif
#ifdef CONFIG_RTC_STRING_UID
void (*on_local_user_registered)(const char *uname, uint32_t uid);
void (*on_remote_user_registered)(const char *uname, uint32_t uid);
#endif // End of CONFIG_RTC_STRING_UID
void (*on_token_privilege_will_expire)(connection_id_t conn_id, const char *token);
#ifdef CONFIG_RTCM
void (*on_media_ctrl_receive)(connection_id_t conn_id, uint32_t uid, const void *payload, size_t length);
#endif
} agora_rtc_event_handler_t;

#pragma mark globals
static agora_rtc_event_handler_t gEventHandler = {};
static pthread_t gServerThread = (pthread_t)NULL;
static int *gClients = NULL;
static uint gClientsCnt = 0;
static pthread_mutex_t gClientsLck = {};

int (*my_startMonitor)(void) = NULL;
int (*my_endMonitor)(void)= NULL;

#pragma mark myfunctions
void *serverThread(void *arg){
    int err = 0;
    int fd_server = -1;
    //
    info("[*] Started Server thread!");

#ifdef UDP_TARGET
    cretassure(fd_server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), "Failed to create TCP socket errno=%d (%s)",errno, strerror(errno));
#else
    cretassure(fd_server = socket(AF_INET, SOCK_STREAM, 0), "Failed to create TCP socket errno=%d (%s)",errno, strerror(errno));
#endif
    if (setsockopt(fd_server, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) error("setsockopt(SO_REUSEADDR) failed");
    {
        struct sockaddr_in servaddr = {};
        servaddr.sin_family = AF_INET;
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        servaddr.sin_port = htons(PORT);
        cretassure(!bind(fd_server, (struct sockaddr *)&servaddr, sizeof(servaddr)),"Failed to bind socket errno=%d (%s)",errno, strerror(errno));
    }
#ifndef UDP_TARGET
    cretassure(!listen(fd_server, 5),"Failed to listen on socket errno=%d (%s)",errno, strerror(errno));
#endif

    cretassure(gClients = (int*)calloc(sizeof(int),1),"Failed to alloc clients size");
    gClientsCnt = 0;
    cretassure(!pthread_mutex_init(&gClientsLck, NULL), "Failed to init mutex errno=%d (%s)",errno, strerror(errno));
#ifdef UDP_TARGET
    gClientsCnt++;
    *gClients = fd_server;
#endif

    if (!my_startMonitor || !my_endMonitor){
        void * handle = dlopen(NULL, RTLD_NOW|RTLD_GLOBAL);
        if (handle){
            debug("got handle!");
            my_startMonitor = (int(*)(void))dlsym(handle, "_Z12startMonitorv");
            my_endMonitor = (int(*)(void))dlsym(handle, "_Z10endMonitorv");
        }
    }

#ifndef UDP_TARGET
    while (1){
        struct sockaddr_in cli = {};
        socklen_t cliSize = sizeof(cli);
        int fd_cli = -1;
        //
        fd_cli = accept(fd_server, (struct sockaddr*)&cli, &cliSize);
        if (fd_cli == -1){
            error("Failed to accept client!");
        }else{
            debug("[+] Got client connection. curr=%d",gClientsCnt);
            pthread_mutex_lock(&gClientsLck);            
            gClientsCnt++;
            gClients = realloc(gClients, sizeof(*gClients)*gClientsCnt);
            gClients[gClientsCnt-1] = fd_cli;
            gEventHandler.on_join_channel_success(NULL, 0);
            if (gClientsCnt == 1){
                info("[*] First client connected, starting video monitor!");
                if (my_startMonitor){
                    int rt = my_startMonitor();
                    info("startMonitor() retured %d",rt);
                }else{
                    error("startMonitor() not available!!!!");
                }
            }
            pthread_mutex_unlock(&gClientsLck);
            info("[!] Client added!");
        }
    }
#else
    {
        info("[*] starting video monitor!");
        gEventHandler.on_join_channel_success(NULL, 0);
        if (my_startMonitor){
            int rt = my_startMonitor();
            info("startMonitor() retured %d",rt);
        }else{
            error("startMonitor() not available!!!!");
        }
        return NULL;
    }
#endif

error:
    if (err){
        error("Function %s failed with err:%d",__PRETTY_FUNCTION__,err);
    }
    if (fd_server != -1){
        close(fd_server); fd_server = -1;
    }
    return NULL;
}


#pragma mark custom implementations
int agora_rtc_license_verify (const char *certificate, int certificate_len, const char *credential, int credential_len){
    debug("agora_rtc_license_verify=0");
    return 0;
}

int agora_rtc_init(const char *app_id, const agora_rtc_event_handler_t *event_handler, rtc_service_option_t *option){
    int err = 0;
    //
    gEventHandler = *event_handler;
    cretassure(!pthread_create(&gServerThread, NULL, serverThread, NULL),"Failed to create server thread");

error:
    if (err){
        error("Function %s failed with err:%d",__PRETTY_FUNCTION__,err);
    }
    return 0;
}

int agora_rtc_send_video_data(void* isnull, int streamid, const void *data_ptr, size_t data_len, video_frame_info_t *info_ptr){
    // debug("-------------------- agora_rtc_send_video_data(%zu) --------------------\n",data_len);
#ifdef UDP_TARGET
    struct sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = inet_addr(UDP_TARGET);
    target.sin_port = htons(PORT);
    ssize_t didSend = sendto(*gClients,data_ptr,data_len, 0,(struct sockaddr*)&target,sizeof(target));
    return (int)didSend;
#else
    int lostClients = 0;
    pthread_mutex_lock(&gClientsLck);            
    for (size_t i = 0; i < gClientsCnt; i++){
        ssize_t didSend = send(gClients[i], data_ptr, data_len, 0);
        if (didSend <= 0){
            close(gClients[i]);
            gClients[i] = -1;
            lostClients++;
        }
    }
    if (lostClients){
        for (size_t i = 0; i < gClientsCnt; i++){
            if (gClients[i] == -1){
                memmove(&gClients[i],&gClients[i+1],(gClientsCnt-i)*sizeof(*gClients));
                gClientsCnt--;
            }
        }
    }
    if (gClientsCnt == 0){
        gEventHandler.on_connection_lost(NULL);
        if (my_endMonitor){
            int rt = my_endMonitor();
            info("endMonitor() retured %d",rt);
        }else{
            error("endMonitor() not available!!!!");
        }
    }
    pthread_mutex_unlock(&gClientsLck);   
#endif        
    return 0;
}

int agora_rtc_set_log_level(rtc_log_level_e level){
    debug("-------------------- agora_rtc_set_log_level(%d) --------------------\n",level);
    return 0;
}

int agora_rtc_config_log(int size_per_file, int max_file_count){
    debug("-------------------- agora_rtc_config_log(%d,%d) --------------------\n",size_per_file,max_file_count);
    return 0;
}

#pragma mark autogenerated
void ahpl_rb_root_init(void) {
    printf("-------------------- ahpl_rb_root_init --------------------\n");
    assert(0);
}

void ahpl_mpqp_shrink_all(void) {
    printf("-------------------- ahpl_mpqp_shrink_all --------------------\n");
    assert(0);
}

void ahpl_decode_int16(void) {
    printf("-------------------- ahpl_decode_int16 --------------------\n");
    assert(0);
}

void ahpl_file_close(void) {
    printf("-------------------- ahpl_file_close --------------------\n");
    assert(0);
}

void ahpl_refobj_read_args(void) {
    printf("-------------------- ahpl_refobj_read_args --------------------\n");
    assert(0);
}

void ahpl_find_rb_node(void) {
    printf("-------------------- ahpl_find_rb_node --------------------\n");
    assert(0);
}

void ahpl_refobj_read_argv(void) {
    printf("-------------------- ahpl_refobj_read_argv --------------------\n");
    assert(0);
}

void ahpl_write(void) {
    printf("-------------------- ahpl_write --------------------\n");
    assert(0);
}

void ahpl_mpqp_call_args(void) {
    printf("-------------------- ahpl_mpqp_call_args --------------------\n");
    assert(0);
}

void ahpl_mpqp_call_argv(void) {
    printf("-------------------- ahpl_mpqp_call_argv --------------------\n");
    assert(0);
}

void ahpl_us_from_ts(void) {
    printf("-------------------- ahpl_us_from_ts --------------------\n");
    assert(0);
}

void ahpl_ref_hold(void) {
    printf("-------------------- ahpl_ref_hold --------------------\n");
    assert(0);
}

void ahpl_us_from_tv(void) {
    printf("-------------------- ahpl_us_from_tv --------------------\n");
    assert(0);
}

void ahpl_alloc_psb(void) {
    printf("-------------------- ahpl_alloc_psb --------------------\n");
    assert(0);
}

void ahpl_file_open(void) {
    printf("-------------------- ahpl_file_open --------------------\n");
    assert(0);
}

void ahpl_tls_key_get(void) {
    printf("-------------------- ahpl_tls_key_get --------------------\n");
    assert(0);
}

void ahpl_init_typed_obj(void) {
    printf("-------------------- ahpl_init_typed_obj --------------------\n");
    assert(0);
}

void ahpl_ref_read_args(void) {
    printf("-------------------- ahpl_ref_read_args --------------------\n");
    assert(0);
}

void ahpl_ref_read_argv(void) {
    printf("-------------------- ahpl_ref_read_argv --------------------\n");
    assert(0);
}

void ahpl_rwlock_rdlock(void) {
    printf("-------------------- ahpl_rwlock_rdlock --------------------\n");
    assert(0);
}

void ahpl_mpqp_call_data(void) {
    printf("-------------------- ahpl_mpqp_call_data --------------------\n");
    assert(0);
}

void ahpl_module_unregister(void) {
    printf("-------------------- ahpl_module_unregister --------------------\n");
    assert(0);
}

void ahpl_psb_total_len(void) {
    printf("-------------------- ahpl_psb_total_len --------------------\n");
    assert(0);
}

void agora_rtc_set_bwe_param(void) {
    printf("-------------------- agora_rtc_set_bwe_param --------------------\n");
    assert(0);
}

void agora_rtc_set_cloud_proxy(void) {
    printf("-------------------- agora_rtc_set_cloud_proxy --------------------\n");
    assert(0);
}

void ahpl_rwlock_wrlock(void) {
    printf("-------------------- ahpl_rwlock_wrlock --------------------\n");
    assert(0);
}

void agora_rtc_fini(void) {
    printf("-------------------- agora_rtc_fini --------------------\n");
    assert(0);
}

void ahpl_processors_count(void) {
    printf("-------------------- ahpl_processors_count --------------------\n");
    assert(0);
}

void ahpl_wmb(void) {
    printf("-------------------- ahpl_wmb --------------------\n");
    assert(0);
}

void ahpl_mpq_run_func_arg(void) {
    printf("-------------------- ahpl_mpq_run_func_arg --------------------\n");
    assert(0);
}

void ahpl_sk_addr_ip_equal(void) {
    printf("-------------------- ahpl_sk_addr_ip_equal --------------------\n");
    assert(0);
}

void ahpl_mpq_this_destroyed(void) {
    printf("-------------------- ahpl_mpq_this_destroyed --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_set(void) {
    printf("-------------------- ahpl_atomic_intptr_set --------------------\n");
    assert(0);
}

void agora_rtc_login_rtm(void) {
    printf("-------------------- agora_rtc_login_rtm --------------------\n");
    assert(0);
}

void ahpl_lock_lock(void) {
    printf("-------------------- ahpl_lock_lock --------------------\n");
    assert(0);
}

void ahpl_dynamic_bytes_compare(void) {
    printf("-------------------- ahpl_dynamic_bytes_compare --------------------\n");
    assert(0);
}

void ahpl_decode_int32(void) {
    printf("-------------------- ahpl_decode_int32 --------------------\n");
    assert(0);
}

void ahpl_platform_obj_get(void) {
    printf("-------------------- ahpl_platform_obj_get --------------------\n");
    assert(0);
}

void ahpl_mb(void) {
    printf("-------------------- ahpl_mb --------------------\n");
    assert(0);
}

void ahpl_mpqp_call(void) {
    printf("-------------------- ahpl_mpqp_call --------------------\n");
    assert(0);
}

void ahpl_mpq_run(void) {
    printf("-------------------- ahpl_mpq_run --------------------\n");
    assert(0);
}

void AgoraRtcOpus_EnableDtx(void) {
    printf("-------------------- AgoraRtcOpus_EnableDtx --------------------\n");
    assert(0);
}

void ahpl_mpq_connect_on_q(void) {
    printf("-------------------- ahpl_mpq_connect_on_q --------------------\n");
    assert(0);
}

void ahpl_mpq_resched_timer(void) {
    printf("-------------------- ahpl_mpq_resched_timer --------------------\n");
    assert(0);
}

void ahpl_rb_insert_node(void) {
    printf("-------------------- ahpl_rb_insert_node --------------------\n");
    assert(0);
}

void agora_rtc_renew_token(void) {
    printf("-------------------- agora_rtc_renew_token --------------------\n");
    assert(0);
}

void agora_rtc_leave_channel(void) {
    printf("-------------------- agora_rtc_leave_channel --------------------\n");
    assert(0);
}

void ahpl_psb_data(void) {
    printf("-------------------- ahpl_psb_data --------------------\n");
    assert(0);
}

void ahpl_ipv6_addr_v4_compatible(void) {
    printf("-------------------- ahpl_ipv6_addr_v4_compatible --------------------\n");
    assert(0);
}

void ahpl_cond_signal(void) {
    printf("-------------------- ahpl_cond_signal --------------------\n");
    assert(0);
}

void ahpl_mpq_timer_interval(void) {
    printf("-------------------- ahpl_mpq_timer_interval --------------------\n");
    assert(0);
}

void ahpl_event_reset(void) {
    printf("-------------------- ahpl_event_reset --------------------\n");
    assert(0);
}

void ahpl_mpq_run_args(void) {
    printf("-------------------- ahpl_mpq_run_args --------------------\n");
    assert(0);
}

void ahpl_mpq_run_argv(void) {
    printf("-------------------- ahpl_mpq_run_argv --------------------\n");
    assert(0);
}

void ahpl_panic(void) {
    printf("-------------------- ahpl_panic --------------------\n");
    assert(0);
}

void ahpl_mpq_run_func_done_qid(void) {
    printf("-------------------- ahpl_mpq_run_func_done_qid --------------------\n");
    assert(0);
}

void ahpl_rb_traverse_dlr(void) {
    printf("-------------------- ahpl_rb_traverse_dlr --------------------\n");
    assert(0);
}

void ahpl_rb_traverse_lrd(void) {
    printf("-------------------- ahpl_rb_traverse_lrd --------------------\n");
    assert(0);
}

void AgoraRtcOpus_EnableFec(void) {
    printf("-------------------- AgoraRtcOpus_EnableFec --------------------\n");
    assert(0);
}

void ahpl_mpq_run_data(void) {
    printf("-------------------- ahpl_mpq_run_data --------------------\n");
    assert(0);
}

void agora_rtc_request_video_key_frame(void) {
    printf("-------------------- agora_rtc_request_video_key_frame --------------------\n");
    assert(0);
}

void ahpl_get_uuid(void) {
    printf("-------------------- ahpl_get_uuid --------------------\n");
    assert(0);
}

void ahpl_time_us(void) {
    printf("-------------------- ahpl_time_us --------------------\n");
    assert(0);
}

void ahpl_mpq_listen_on_q(void) {
    printf("-------------------- ahpl_mpq_listen_on_q --------------------\n");
    assert(0);
}

void ahpl_event_destroy(void) {
    printf("-------------------- ahpl_event_destroy --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecoderFree(void) {
    printf("-------------------- AgoraRtcOpus_DecoderFree --------------------\n");
    assert(0);
}

void ahpl_task_prepare(void) {
    printf("-------------------- ahpl_task_prepare --------------------\n");
    assert(0);
}

void ahpl_vfind_rb_links(void) {
    printf("-------------------- ahpl_vfind_rb_links --------------------\n");
    assert(0);
}

void agora_rtc_set_params(void) {
    printf("-------------------- agora_rtc_set_params --------------------\n");
    assert(0);
}

void ahpl_realloc(void) {
    printf("-------------------- ahpl_realloc --------------------\n");
    assert(0);
}

void ahpl_file_alseek(void) {
    printf("-------------------- ahpl_file_alseek --------------------\n");
    assert(0);
}

void ahpl_psb_get(void) {
    printf("-------------------- ahpl_psb_get --------------------\n");
    assert(0);
}

void ahpl_os_rmdir(void) {
    printf("-------------------- ahpl_os_rmdir --------------------\n");
    assert(0);
}

void ahpl_file_aread_args(void) {
    printf("-------------------- ahpl_file_aread_args --------------------\n");
    assert(0);
}

void ahpl_file_alseek_args(void) {
    printf("-------------------- ahpl_file_alseek_args --------------------\n");
    assert(0);
}

void AgoraRtcOpus_Decode(void) {
    printf("-------------------- AgoraRtcOpus_Decode --------------------\n");
    assert(0);
}

void ahpl_main_wait(void) {
    printf("-------------------- ahpl_main_wait --------------------\n");
    assert(0);
}

void ahpl_file_aread_argv(void) {
    printf("-------------------- ahpl_file_aread_argv --------------------\n");
    assert(0);
}

void ahpl_file_alseek_argv(void) {
    printf("-------------------- ahpl_file_alseek_argv --------------------\n");
    assert(0);
}

void ahpl_file_aread(void) {
    printf("-------------------- ahpl_file_aread --------------------\n");
    assert(0);
}

void ahpl_tls_key_set(void) {
    printf("-------------------- ahpl_tls_key_set --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecodeFec(void) {
    printf("-------------------- AgoraRtcOpus_DecodeFec --------------------\n");
    assert(0);
}

void ahpl_rwlock_tryrdlock(void) {
    printf("-------------------- ahpl_rwlock_tryrdlock --------------------\n");
    assert(0);
}

void ahpl_decode_int64(void) {
    printf("-------------------- ahpl_decode_int64 --------------------\n");
    assert(0);
}

void agora_rtc_logout_rtm(void) {
    printf("-------------------- agora_rtc_logout_rtm --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DisableDtx(void) {
    printf("-------------------- AgoraRtcOpus_DisableDtx --------------------\n");
    assert(0);
}

void ahpl_mpq_set_ipv6_prefix_on_q(void) {
    printf("-------------------- ahpl_mpq_set_ipv6_prefix_on_q --------------------\n");
    assert(0);
}

void ahpl_cond_wait(void) {
    printf("-------------------- ahpl_cond_wait --------------------\n");
    assert(0);
}

void ahpl_mpq_add_dgram_socket(void) {
    printf("-------------------- ahpl_mpq_add_dgram_socket --------------------\n");
    assert(0);
}

void agora_rtc_send_rtm_data(void) {
    printf("-------------------- agora_rtc_send_rtm_data --------------------\n");
    assert(0);
}

void ahpl_start_time_us(void) {
    printf("-------------------- ahpl_start_time_us --------------------\n");
    assert(0);
}

void ahpl_set_log_level(void) {
    printf("-------------------- ahpl_set_log_level --------------------\n");
    assert(0);
}

void ahpl_task_async_done_opaque(void) {
    printf("-------------------- ahpl_task_async_done_opaque --------------------\n");
    assert(0);
}

void ahpl_encode_int16(void) {
    printf("-------------------- ahpl_encode_int16 --------------------\n");
    assert(0);
}

void ahpl_vlog(void) {
    printf("-------------------- ahpl_vlog --------------------\n");
    assert(0);
}

void ahpl_mpq_ip_sk_connect(void) {
    printf("-------------------- ahpl_mpq_ip_sk_connect --------------------\n");
    assert(0);
}

void ahpl_atomic_cmpxchg(void) {
    printf("-------------------- ahpl_atomic_cmpxchg --------------------\n");
    assert(0);
}

void ahpl_malloc(void) {
    printf("-------------------- ahpl_malloc --------------------\n");
    assert(0);
}

void ahpl_mpq_set_timer(void) {
    printf("-------------------- ahpl_mpq_set_timer --------------------\n");
    assert(0);
}

void ahpl_mpq_call(void) {
    printf("-------------------- ahpl_mpq_call --------------------\n");
    assert(0);
}

void ahpl_udp_resolve_host_async(void) {
    printf("-------------------- ahpl_udp_resolve_host_async --------------------\n");
    assert(0);
}

void ahpl_lock_unlock(void) {
    printf("-------------------- ahpl_lock_unlock --------------------\n");
    assert(0);
}

void ahpl_task_async_done(void) {
    printf("-------------------- ahpl_task_async_done --------------------\n");
    assert(0);
}

void ahpl_fini_typed_obj(void) {
    printf("-------------------- ahpl_fini_typed_obj --------------------\n");
    assert(0);
}

void ahpl_psb_single(void) {
    printf("-------------------- ahpl_psb_single --------------------\n");
    assert(0);
}

void ahpl_net_route_changed(void) {
    printf("-------------------- ahpl_net_route_changed --------------------\n");
    assert(0);
}

void ahpl_ip_addr_init(void) {
    printf("-------------------- ahpl_ip_addr_init --------------------\n");
    assert(0);
}

void ahpl_udp_resolve_host_asyncv(void) {
    printf("-------------------- ahpl_udp_resolve_host_asyncv --------------------\n");
    assert(0);
}

void ahpl_ms_from_ts(void) {
    printf("-------------------- ahpl_ms_from_ts --------------------\n");
    assert(0);
}

void ahpl_ms_from_tv(void) {
    printf("-------------------- ahpl_ms_from_tv --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_add_return(void) {
    printf("-------------------- ahpl_atomic_intptr_add_return --------------------\n");
    assert(0);
}

void ahpl_tls_key_create(void) {
    printf("-------------------- ahpl_tls_key_create --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DisableFec(void) {
    printf("-------------------- AgoraRtcOpus_DisableFec --------------------\n");
    assert(0);
}

void ahpl_rwlock_create(void) {
    printf("-------------------- ahpl_rwlock_create --------------------\n");
    assert(0);
}

void ahpl_mpq_create_timer(void) {
    printf("-------------------- ahpl_mpq_create_timer --------------------\n");
    assert(0);
}

void ahpl_mpq_queue(void) {
    printf("-------------------- ahpl_mpq_queue --------------------\n");
    assert(0);
}

void ahpl_task_exclusive_exec_args(void) {
    printf("-------------------- ahpl_task_exclusive_exec_args --------------------\n");
    assert(0);
}

void ahpl_mpq_is_main(void) {
    printf("-------------------- ahpl_mpq_is_main --------------------\n");
    assert(0);
}

void ahpl_cond_destroy(void) {
    printf("-------------------- ahpl_cond_destroy --------------------\n");
    assert(0);
}

void ahpl_task_exclusive_exec_argv(void) {
    printf("-------------------- ahpl_task_exclusive_exec_argv --------------------\n");
    assert(0);
}

void agora_rtc_mute_local_video(void) {
    printf("-------------------- agora_rtc_mute_local_video --------------------\n");
    assert(0);
}

void ahpl_ipv6_sk_addr_from_ipv4(void) {
    printf("-------------------- ahpl_ipv6_sk_addr_from_ipv4 --------------------\n");
    assert(0);
}

void ahpl_free(void) {
    printf("-------------------- ahpl_free --------------------\n");
    assert(0);
}

void ahpl_find_rb_links(void) {
    printf("-------------------- ahpl_find_rb_links --------------------\n");
    assert(0);
}

void ahpl_rt_valid(void) {
    printf("-------------------- ahpl_rt_valid --------------------\n");
    assert(0);
}

void ahpl_mpq_free(void) {
    printf("-------------------- ahpl_mpq_free --------------------\n");
    assert(0);
}

void ahpl_mpq_current(void) {
    printf("-------------------- ahpl_mpq_current --------------------\n");
    assert(0);
}

void ahpl_file_write(void) {
    printf("-------------------- ahpl_file_write --------------------\n");
    assert(0);
}

void ahpl_mpqp_pool_tail_queue(void) {
    printf("-------------------- ahpl_mpqp_pool_tail_queue --------------------\n");
    assert(0);
}

void ahpl_module_put(void) {
    printf("-------------------- ahpl_module_put --------------------\n");
    assert(0);
}

void ahpl_get_git_commit(void) {
    printf("-------------------- ahpl_get_git_commit --------------------\n");
    assert(0);
}

void ahpl_ip_sk_addr_port(void) {
    printf("-------------------- ahpl_ip_sk_addr_port --------------------\n");
    assert(0);
}

void ahpl_dynamic_bytes_add_data(void) {
    printf("-------------------- ahpl_dynamic_bytes_add_data --------------------\n");
    assert(0);
}

void ahpl_encode_int32(void) {
    printf("-------------------- ahpl_encode_int32 --------------------\n");
    assert(0);
}

void ahpl_mpq_call_args(void) {
    printf("-------------------- ahpl_mpq_call_args --------------------\n");
    assert(0);
}

void ahpl_mpq_call_argv(void) {
    printf("-------------------- ahpl_mpq_call_argv --------------------\n");
    assert(0);
}

void ahpl_os_open(void) {
    printf("-------------------- ahpl_os_open --------------------\n");
    assert(0);
}

void ahpl_calloc(void) {
    printf("-------------------- ahpl_calloc --------------------\n");
    assert(0);
}

void ahpl_get_log_level(void) {
    printf("-------------------- ahpl_get_log_level --------------------\n");
    assert(0);
}

void ahpl_rmb(void) {
    printf("-------------------- ahpl_rmb --------------------\n");
    assert(0);
}

void ahpl_log(void) {
    printf("-------------------- ahpl_log --------------------\n");
    assert(0);
}

void ahpl_send(void) {
    printf("-------------------- ahpl_send --------------------\n");
    assert(0);
}

void ahpl_ref_write(void) {
    printf("-------------------- ahpl_ref_write --------------------\n");
    assert(0);
}

void ahpl_atomic_dec(void) {
    printf("-------------------- ahpl_atomic_dec --------------------\n");
    assert(0);
}

void ahpl_atomic_read(void) {
    printf("-------------------- ahpl_atomic_read --------------------\n");
    assert(0);
}

void ahpl_mpq_call_data(void) {
    printf("-------------------- ahpl_mpq_call_data --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_inc_and_test(void) {
    printf("-------------------- ahpl_atomic_intptr_inc_and_test --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_read(void) {
    printf("-------------------- ahpl_atomic_intptr_read --------------------\n");
    assert(0);
}

void ahpl_file_size(void) {
    printf("-------------------- ahpl_file_size --------------------\n");
    assert(0);
}

void ahpl_mpq_timer_active(void) {
    printf("-------------------- ahpl_mpq_timer_active --------------------\n");
    assert(0);
}

void ahpl_subscribe_net_events(void) {
    printf("-------------------- ahpl_subscribe_net_events --------------------\n");
    assert(0);
}

void ahpl_psb_detach_buf(void) {
    printf("-------------------- ahpl_psb_detach_buf --------------------\n");
    assert(0);
}

void ahpl_mpq_create(void) {
    printf("-------------------- ahpl_mpq_create --------------------\n");
    assert(0);
}

void ahpl_inet_addr_str(void) {
    printf("-------------------- ahpl_inet_addr_str --------------------\n");
    assert(0);
}

void AgoraRtcOpus_GetNonActivityFrames(void) {
    printf("-------------------- AgoraRtcOpus_GetNonActivityFrames --------------------\n");
    assert(0);
}

void ahpl_mpqp_shrink(void) {
    printf("-------------------- ahpl_mpqp_shrink --------------------\n");
    assert(0);
}

void ahpl_mpq_set_timer_on_q(void) {
    printf("-------------------- ahpl_mpq_set_timer_on_q --------------------\n");
    assert(0);
}

void ahpl_os_version(void) {
    printf("-------------------- ahpl_os_version --------------------\n");
    assert(0);
}

void ahpl_getsockname(void) {
    printf("-------------------- ahpl_getsockname --------------------\n");
    assert(0);
}

void ahpl_rb_remove(void) {
    printf("-------------------- ahpl_rb_remove --------------------\n");
    assert(0);
}

void ahpl_mpq_exec_counters(void) {
    printf("-------------------- ahpl_mpq_exec_counters --------------------\n");
    assert(0);
}

void ahpl_mpq_set_oneshot_timer_on_q(void) {
    printf("-------------------- ahpl_mpq_set_oneshot_timer_on_q --------------------\n");
    assert(0);
}

void ahpl_psb_attach_buf(void) {
    printf("-------------------- ahpl_psb_attach_buf --------------------\n");
    assert(0);
}

void ahpl_task_create(void) {
    printf("-------------------- ahpl_task_create --------------------\n");
    assert(0);
}

void ahpl_refobj_read(void) {
    printf("-------------------- ahpl_refobj_read --------------------\n");
    assert(0);
}

void ahpl_mpq_destroy_wait(void) {
    printf("-------------------- ahpl_mpq_destroy_wait --------------------\n");
    assert(0);
}

void ahpl_dynamic_array_take(void) {
    printf("-------------------- ahpl_dynamic_array_take --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecoderInit(void) {
    printf("-------------------- AgoraRtcOpus_DecoderInit --------------------\n");
    assert(0);
}

void ahpl_task_exclusive_exec(void) {
    printf("-------------------- ahpl_task_exclusive_exec --------------------\n");
    assert(0);
}

void ahpl_task_exec(void) {
    printf("-------------------- ahpl_task_exec --------------------\n");
    assert(0);
}

void ahpl_vprintf(void) {
    printf("-------------------- ahpl_vprintf --------------------\n");
    assert(0);
}

void ahpl_get_default_rt(void) {
    printf("-------------------- ahpl_get_default_rt --------------------\n");
    assert(0);
}

void ahpl_thread_self_id(void) {
    printf("-------------------- ahpl_thread_self_id --------------------\n");
    assert(0);
}

void ahpl_event_pulse(void) {
    printf("-------------------- ahpl_event_pulse --------------------\n");
    assert(0);
}

void ahpl_rwlock_wrunlock(void) {
    printf("-------------------- ahpl_rwlock_wrunlock --------------------\n");
    assert(0);
}

void ahpl_start_time_sec(void) {
    printf("-------------------- ahpl_start_time_sec --------------------\n");
    assert(0);
}

void ahpl_vfind_rb_node(void) {
    printf("-------------------- ahpl_vfind_rb_node --------------------\n");
    assert(0);
}

void ahpl_mpq_set_oneshot_timer(void) {
    printf("-------------------- ahpl_mpq_set_oneshot_timer --------------------\n");
    assert(0);
}

void agora_rtc_mute_remote_audio(void) {
    printf("-------------------- agora_rtc_mute_remote_audio --------------------\n");
    assert(0);
}

void ahpl_lock_trylock(void) {
    printf("-------------------- ahpl_lock_trylock --------------------\n");
    assert(0);
}

void ahpl_time_sec(void) {
    printf("-------------------- ahpl_time_sec --------------------\n");
    assert(0);
}

void ahpl_encode_int64(void) {
    printf("-------------------- ahpl_encode_int64 --------------------\n");
    assert(0);
}

void ahpl_ip_sk_close(void) {
    printf("-------------------- ahpl_ip_sk_close --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_sub_return(void) {
    printf("-------------------- ahpl_atomic_intptr_sub_return --------------------\n");
    assert(0);
}

void ahpl_atomic_inc(void) {
    printf("-------------------- ahpl_atomic_inc --------------------\n");
    assert(0);
}

void ahpl_ipv6_addr_v4_mapped(void) {
    printf("-------------------- ahpl_ipv6_addr_v4_mapped --------------------\n");
    assert(0);
}

void ahpl_mpq_queue_args(void) {
    printf("-------------------- ahpl_mpq_queue_args --------------------\n");
    assert(0);
}

void ahpl_msleep(void) {
    printf("-------------------- ahpl_msleep --------------------\n");
    assert(0);
}

void ahpl_mpq_get_ipv6_prefix(void) {
    printf("-------------------- ahpl_mpq_get_ipv6_prefix --------------------\n");
    assert(0);
}

void ahpl_mpq_queue_argv(void) {
    printf("-------------------- ahpl_mpq_queue_argv --------------------\n");
    assert(0);
}

void ahpl_same_rt(void) {
    printf("-------------------- ahpl_same_rt --------------------\n");
    assert(0);
}

void ahpl_file_read(void) {
    printf("-------------------- ahpl_file_read --------------------\n");
    assert(0);
}

void ahpl_os_stat(void) {
    printf("-------------------- ahpl_os_stat --------------------\n");
    assert(0);
}

void ahpl_dynamic_bytes_copy_data(void) {
    printf("-------------------- ahpl_dynamic_bytes_copy_data --------------------\n");
    assert(0);
}

void ahpl_tls_key_delete(void) {
    printf("-------------------- ahpl_tls_key_delete --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetComplexity(void) {
    printf("-------------------- AgoraRtcOpus_SetComplexity --------------------\n");
    assert(0);
}

void ahpl_psb_pull(void) {
    printf("-------------------- ahpl_psb_pull --------------------\n");
    assert(0);
}

void ahpl_mpq_queue_data(void) {
    printf("-------------------- ahpl_mpq_queue_data --------------------\n");
    assert(0);
}

void ahpl_mpq_create_flags(void) {
    printf("-------------------- ahpl_mpq_create_flags --------------------\n");
    assert(0);
}

void ahpl_invalidate_rt(void) {
    printf("-------------------- ahpl_invalidate_rt --------------------\n");
    assert(0);
}

void ahpl_psb_reserve(void) {
    printf("-------------------- ahpl_psb_reserve --------------------\n");
    assert(0);
}

void ahpl_is_mobile_net(void) {
    printf("-------------------- ahpl_is_mobile_net --------------------\n");
    assert(0);
}

void ahpl_ip_sk_bind(void) {
    printf("-------------------- ahpl_ip_sk_bind --------------------\n");
    assert(0);
}

void ahpl_getsockopt(void) {
    printf("-------------------- ahpl_getsockopt --------------------\n");
    assert(0);
}

void ahpl_tick_ns(void) {
    printf("-------------------- ahpl_tick_ns --------------------\n");
    assert(0);
}

void ahpl_task_resume_args(void) {
    printf("-------------------- ahpl_task_resume_args --------------------\n");
    assert(0);
}

void ahpl_task_resume_argv(void) {
    printf("-------------------- ahpl_task_resume_argv --------------------\n");
    assert(0);
}

void ahpl_dynamic_array_is_empty(void) {
    printf("-------------------- ahpl_dynamic_array_is_empty --------------------\n");
    assert(0);
}

void AgoraRtcOpus_PacketHasFec(void) {
    printf("-------------------- AgoraRtcOpus_PacketHasFec --------------------\n");
    assert(0);
}

void ahpl_module_get(void) {
    printf("-------------------- ahpl_module_get --------------------\n");
    assert(0);
}

void ahpl_event_set(void) {
    printf("-------------------- ahpl_event_set --------------------\n");
    assert(0);
}

void ahpl_start_tick_ms(void) {
    printf("-------------------- ahpl_start_tick_ms --------------------\n");
    assert(0);
}

void ahpl_mpq_ip_sk_connect_on_q(void) {
    printf("-------------------- ahpl_mpq_ip_sk_connect_on_q --------------------\n");
    assert(0);
}

void ahpl_inet_addr_from_string(void) {
    printf("-------------------- ahpl_inet_addr_from_string --------------------\n");
    assert(0);
}

void ahpl_module_call_args(void) {
    printf("-------------------- ahpl_module_call_args --------------------\n");
    assert(0);
}

void ahpl_mpq_loop(void) {
    printf("-------------------- ahpl_mpq_loop --------------------\n");
    assert(0);
}

void ahpl_module_call_argv(void) {
    printf("-------------------- ahpl_module_call_argv --------------------\n");
    assert(0);
}

void ahpl_same_def_rt(void) {
    printf("-------------------- ahpl_same_def_rt --------------------\n");
    assert(0);
}

void ahpl_free_psb_list(void) {
    printf("-------------------- ahpl_free_psb_list --------------------\n");
    assert(0);
}

void ahpl_ip_sk_addr_init_with_port(void) {
    printf("-------------------- ahpl_ip_sk_addr_init_with_port --------------------\n");
    assert(0);
}

void ahpl_task_prepare_args(void) {
    printf("-------------------- ahpl_task_prepare_args --------------------\n");
    assert(0);
}

void ahpl_task_prepare_argv(void) {
    printf("-------------------- ahpl_task_prepare_argv --------------------\n");
    assert(0);
}

void ahpl_os_rename(void) {
    printf("-------------------- ahpl_os_rename --------------------\n");
    assert(0);
}

void ahpl_rb_erase(void) {
    printf("-------------------- ahpl_rb_erase --------------------\n");
    assert(0);
}

void ahpl_tick_now(void) {
    printf("-------------------- ahpl_tick_now --------------------\n");
    assert(0);
}

void ahpl_dynamic_string_strcpy(void) {
    printf("-------------------- ahpl_dynamic_string_strcpy --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetBitRate(void) {
    printf("-------------------- AgoraRtcOpus_SetBitRate --------------------\n");
    assert(0);
}

void ahpl_mpq_listen(void) {
    printf("-------------------- ahpl_mpq_listen --------------------\n");
    assert(0);
}

void ahpl_mpq_queued_count(void) {
    printf("-------------------- ahpl_mpq_queued_count --------------------\n");
    assert(0);
}

void ahpl_start_tick_ns(void) {
    printf("-------------------- ahpl_start_tick_ns --------------------\n");
    assert(0);
}

void ahpl_psb_headroom(void) {
    printf("-------------------- ahpl_psb_headroom --------------------\n");
    assert(0);
}

void ahpl_mpq_add_dgram_socket_on_q(void) {
    printf("-------------------- ahpl_mpq_add_dgram_socket_on_q --------------------\n");
    assert(0);
}

void ahpl_lock_create(void) {
    printf("-------------------- ahpl_lock_create --------------------\n");
    assert(0);
}

void ahpl_dynamic_bytes_copy(void) {
    printf("-------------------- ahpl_dynamic_bytes_copy --------------------\n");
    assert(0);
}

void ahpl_atomic_set(void) {
    printf("-------------------- ahpl_atomic_set --------------------\n");
    assert(0);
}

void ahpl_refobj_arg(void) {
    printf("-------------------- ahpl_refobj_arg --------------------\n");
    assert(0);
}

void agora_rtc_err_2_str(void) {
    printf("-------------------- agora_rtc_err_2_str --------------------\n");
    assert(0);
}

void agora_rtc_get_version(void) {
    printf("-------------------- agora_rtc_get_version --------------------\n");
    assert(0);
}

void ahpl_usleep(void) {
    printf("-------------------- ahpl_usleep --------------------\n");
    assert(0);
}

void ahpl_ltwp(void) {
    printf("-------------------- ahpl_ltwp --------------------\n");
    assert(0);
}

void ahpl_mpq_change_flags(void) {
    printf("-------------------- ahpl_mpq_change_flags --------------------\n");
    assert(0);
}

void AgoraRtcOpus_EncoderFree(void) {
    printf("-------------------- AgoraRtcOpus_EncoderFree --------------------\n");
    assert(0);
}

void ahpl_mpq_alloc(void) {
    printf("-------------------- ahpl_mpq_alloc --------------------\n");
    assert(0);
}

void ahpl_rb_traverse_ldr(void) {
    printf("-------------------- ahpl_rb_traverse_ldr --------------------\n");
    assert(0);
}

void ahpl_mpqp_create(void) {
    printf("-------------------- ahpl_mpqp_create --------------------\n");
    assert(0);
}

void ahpl_os_fstat(void) {
    printf("-------------------- ahpl_os_fstat --------------------\n");
    assert(0);
}

void ahpl_rwlock_destroy(void) {
    printf("-------------------- ahpl_rwlock_destroy --------------------\n");
    assert(0);
}

void AgoraRtcOpus_Encode(void) {
    printf("-------------------- AgoraRtcOpus_Encode --------------------\n");
    assert(0);
}

void ahpl_dynamic_array_init(void) {
    printf("-------------------- ahpl_dynamic_array_init --------------------\n");
    assert(0);
}

void ahpl_os_unlink(void) {
    printf("-------------------- ahpl_os_unlink --------------------\n");
    assert(0);
}

void ahpl_ipv6_addr_nat64(void) {
    printf("-------------------- ahpl_ipv6_addr_nat64 --------------------\n");
    assert(0);
}

void ahpl_bind(void) {
    printf("-------------------- ahpl_bind --------------------\n");
    assert(0);
}

void ahpl_main_exit(void) {
    printf("-------------------- ahpl_main_exit --------------------\n");
    assert(0);
}

void ahpl_os_mkdir(void) {
    printf("-------------------- ahpl_os_mkdir --------------------\n");
    assert(0);
}

void ahpl_psb_peek(void) {
    printf("-------------------- ahpl_psb_peek --------------------\n");
    assert(0);
}

void ahpl_genp(void) {
    printf("-------------------- ahpl_genp --------------------\n");
    assert(0);
}

void ahpl_lock_destroy(void) {
    printf("-------------------- ahpl_lock_destroy --------------------\n");
    assert(0);
}

void ahpl_mpq_add_fd_on_q(void) {
    printf("-------------------- ahpl_mpq_add_fd_on_q --------------------\n");
    assert(0);
}

void ahpl_get_data_path(void) {
    printf("-------------------- ahpl_get_data_path --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetFrameLength(void) {
    printf("-------------------- AgoraRtcOpus_SetFrameLength --------------------\n");
    assert(0);
}

void ahpl_setsockopt(void) {
    printf("-------------------- ahpl_setsockopt --------------------\n");
    assert(0);
}

void ahpl_mpq_get_flags(void) {
    printf("-------------------- ahpl_mpq_get_flags --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetMaxPlaybackRate(void) {
    printf("-------------------- AgoraRtcOpus_SetMaxPlaybackRate --------------------\n");
    assert(0);
}

void ahpl_cond_create(void) {
    printf("-------------------- ahpl_cond_create --------------------\n");
    assert(0);
}

void ahpl_mpq_set_q_arg(void) {
    printf("-------------------- ahpl_mpq_set_q_arg --------------------\n");
    assert(0);
}

void ahpl_mpq_cancel_timer(void) {
    printf("-------------------- ahpl_mpq_cancel_timer --------------------\n");
    assert(0);
}

void ahpl_mpq_last_costs(void) {
    printf("-------------------- ahpl_mpq_last_costs --------------------\n");
    assert(0);
}

void ahpl_marshal(void) {
    printf("-------------------- ahpl_marshal --------------------\n");
    assert(0);
}

void ahpl_mpq_wait(void) {
    printf("-------------------- ahpl_mpq_wait --------------------\n");
    assert(0);
}

void ahpl_rwlock_rd2wrlock(void) {
    printf("-------------------- ahpl_rwlock_rd2wrlock --------------------\n");
    assert(0);
}

void ahpl_refobj_id(void) {
    printf("-------------------- ahpl_refobj_id --------------------\n");
    assert(0);
}

void ahpl_mpq_this(void) {
    printf("-------------------- ahpl_mpq_this --------------------\n");
    assert(0);
}

void ahpl_ip_sk_create(void) {
    printf("-------------------- ahpl_ip_sk_create --------------------\n");
    assert(0);
}

void ahpl_file_awrite(void) {
    printf("-------------------- ahpl_file_awrite --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_cmpxchg(void) {
    printf("-------------------- ahpl_atomic_intptr_cmpxchg --------------------\n");
    assert(0);
}

void ahpl_mpq_connect(void) {
    printf("-------------------- ahpl_mpq_connect --------------------\n");
    assert(0);
}

void ahpl_ipv6_sk_addr_to_ipv4(void) {
    printf("-------------------- ahpl_ipv6_sk_addr_to_ipv4 --------------------\n");
    assert(0);
}

void ahpl_strdup(void) {
    printf("-------------------- ahpl_strdup --------------------\n");
    assert(0);
}

void ahpl_ref_write_args(void) {
    printf("-------------------- ahpl_ref_write_args --------------------\n");
    assert(0);
}

void ahpl_task_resume(void) {
    printf("-------------------- ahpl_task_resume --------------------\n");
    assert(0);
}

void ahpl_tcp_resolve_host_asyncv(void) {
    printf("-------------------- ahpl_tcp_resolve_host_asyncv --------------------\n");
    assert(0);
}

void ahpl_ref_write_argv(void) {
    printf("-------------------- ahpl_ref_write_argv --------------------\n");
    assert(0);
}

void ahpl_mpq_add_fd(void) {
    printf("-------------------- ahpl_mpq_add_fd --------------------\n");
    assert(0);
}

void ahpl_printf(void) {
    printf("-------------------- ahpl_printf --------------------\n");
    assert(0);
}

void ahpl_psb_tailroom(void) {
    printf("-------------------- ahpl_psb_tailroom --------------------\n");
    assert(0);
}

void ahpl_network_is_down(void) {
    printf("-------------------- ahpl_network_is_down --------------------\n");
    assert(0);
}

void agora_rtc_set_module_log_level(void) {
    printf("-------------------- agora_rtc_set_module_log_level --------------------\n");
    assert(0);
}

void ahpl_mpq_destroy(void) {
    printf("-------------------- ahpl_mpq_destroy --------------------\n");
    assert(0);
}

void ahpl_socket(void) {
    printf("-------------------- ahpl_socket --------------------\n");
    assert(0);
}

void ahpl_atomic_add_return(void) {
    printf("-------------------- ahpl_atomic_add_return --------------------\n");
    assert(0);
}

void ahpl_close(void) {
    printf("-------------------- ahpl_close --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_dec_and_test(void) {
    printf("-------------------- ahpl_atomic_intptr_dec_and_test --------------------\n");
    assert(0);
}

void ahpl_mpq_run_func_data(void) {
    printf("-------------------- ahpl_mpq_run_func_data --------------------\n");
    assert(0);
}

void ahpl_sk_addr_len(void) {
    printf("-------------------- ahpl_sk_addr_len --------------------\n");
    assert(0);
}

void ahpl_gpup(void) {
    printf("-------------------- ahpl_gpup --------------------\n");
    assert(0);
}

void ahpl_rwlock_wr2rdlock(void) {
    printf("-------------------- ahpl_rwlock_wr2rdlock --------------------\n");
    assert(0);
}

void agora_rtc_mute_remote_video(void) {
    printf("-------------------- agora_rtc_mute_remote_video --------------------\n");
    assert(0);
}

void ahpl_module_register(void) {
    printf("-------------------- ahpl_module_register --------------------\n");
    assert(0);
}

void ahpl_psb_push(void) {
    printf("-------------------- ahpl_psb_push --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetSignalType(void) {
    printf("-------------------- AgoraRtcOpus_SetSignalType --------------------\n");
    assert(0);
}

void ahpl_task_exec_args(void) {
    printf("-------------------- ahpl_task_exec_args --------------------\n");
    assert(0);
}

void ahpl_task_exec_argv(void) {
    printf("-------------------- ahpl_task_exec_argv --------------------\n");
    assert(0);
}

void ahpl_ref_destroy(void) {
    printf("-------------------- ahpl_ref_destroy --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecoderChannels(void) {
    printf("-------------------- AgoraRtcOpus_DecoderChannels --------------------\n");
    assert(0);
}

void ahpl_mpq_running_refobj(void) {
    printf("-------------------- ahpl_mpq_running_refobj --------------------\n");
    assert(0);
}

void ahpl_atomic_dec_and_test(void) {
    printf("-------------------- ahpl_atomic_dec_and_test --------------------\n");
    assert(0);
}

void ahpl_main_start(void) {
    printf("-------------------- ahpl_main_start --------------------\n");
    assert(0);
}

void ahpl_tick_us(void) {
    printf("-------------------- ahpl_tick_us --------------------\n");
    assert(0);
}

void ahpl_atomic_xchg(void) {
    printf("-------------------- ahpl_atomic_xchg --------------------\n");
    assert(0);
}

void ahpl_thread_self(void) {
    printf("-------------------- ahpl_thread_self --------------------\n");
    assert(0);
}

void ahpl_mpq_create_oneshot_timer_on_q(void) {
    printf("-------------------- ahpl_mpq_create_oneshot_timer_on_q --------------------\n");
    assert(0);
}

void ahpl_event_create(void) {
    printf("-------------------- ahpl_event_create --------------------\n");
    assert(0);
}

void ahpl_mpq_create_timer_on_q(void) {
    printf("-------------------- ahpl_mpq_create_timer_on_q --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_xchg(void) {
    printf("-------------------- ahpl_atomic_intptr_xchg --------------------\n");
    assert(0);
}

void ahpl_mpqp_queue(void) {
    printf("-------------------- ahpl_mpqp_queue --------------------\n");
    assert(0);
}

void ahpl_rwlock_trywrlock(void) {
    printf("-------------------- ahpl_rwlock_trywrlock --------------------\n");
    assert(0);
}

void ahpl_getpeername(void) {
    printf("-------------------- ahpl_getpeername --------------------\n");
    assert(0);
}

void ahpl_dynamic_string_strcpy_out(void) {
    printf("-------------------- ahpl_dynamic_string_strcpy_out --------------------\n");
    assert(0);
}

void ahpl_mpqp_queue_args(void) {
    printf("-------------------- ahpl_mpqp_queue_args --------------------\n");
    assert(0);
}

void ahpl_mpqp_queue_argv(void) {
    printf("-------------------- ahpl_mpqp_queue_argv --------------------\n");
    assert(0);
}

void ahpl_file_lseek(void) {
    printf("-------------------- ahpl_file_lseek --------------------\n");
    assert(0);
}

void ahpl_atomic_inc_and_test(void) {
    printf("-------------------- ahpl_atomic_inc_and_test --------------------\n");
    assert(0);
}

void ahpl_task_get_type(void) {
    printf("-------------------- ahpl_task_get_type --------------------\n");
    assert(0);
}

void ahpl_def_rt_valid(void) {
    printf("-------------------- ahpl_def_rt_valid --------------------\n");
    assert(0);
}

void ahpl_mpqp_destroy(void) {
    printf("-------------------- ahpl_mpqp_destroy --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_dec(void) {
    printf("-------------------- ahpl_atomic_intptr_dec --------------------\n");
    assert(0);
}

void ahpl_cpup(void) {
    printf("-------------------- ahpl_cpup --------------------\n");
    assert(0);
}

void ahpl_dynamic_array_init_with(void) {
    printf("-------------------- ahpl_dynamic_array_init_with --------------------\n");
    assert(0);
}

void ahpl_mpqp_queue_data(void) {
    printf("-------------------- ahpl_mpqp_queue_data --------------------\n");
    assert(0);
}

void ahpl_time_ms(void) {
    printf("-------------------- ahpl_time_ms --------------------\n");
    assert(0);
}

void ahpl_file_awrite_args(void) {
    printf("-------------------- ahpl_file_awrite_args --------------------\n");
    assert(0);
}

void ahpl_event_timedwait(void) {
    printf("-------------------- ahpl_event_timedwait --------------------\n");
    assert(0);
}

void ahpl_file_awrite_argv(void) {
    printf("-------------------- ahpl_file_awrite_argv --------------------\n");
    assert(0);
}

void ahpl_mpq_get_q_arg(void) {
    printf("-------------------- ahpl_mpq_get_q_arg --------------------\n");
    assert(0);
}

void ahpl_start_tick_us(void) {
    printf("-------------------- ahpl_start_tick_us --------------------\n");
    assert(0);
}

void AgoraRtcOpus_EncoderCreate(void) {
    printf("-------------------- AgoraRtcOpus_EncoderCreate --------------------\n");
    assert(0);
}

void ahpl_ns_from_ts(void) {
    printf("-------------------- ahpl_ns_from_ts --------------------\n");
    assert(0);
}

void ahpl_ns_from_tv(void) {
    printf("-------------------- ahpl_ns_from_tv --------------------\n");
    assert(0);
}

void ahpl_ref_read(void) {
    printf("-------------------- ahpl_ref_read --------------------\n");
    assert(0);
}

void ahpl_task_waiting_ops_count(void) {
    printf("-------------------- ahpl_task_waiting_ops_count --------------------\n");
    assert(0);
}

void ahpl_platform_obj_put(void) {
    printf("-------------------- ahpl_platform_obj_put --------------------\n");
    assert(0);
}

void ahpl_mpq_add_stream_socket(void) {
    printf("-------------------- ahpl_mpq_add_stream_socket --------------------\n");
    assert(0);
}

void ahpl_cond_timedwait(void) {
    printf("-------------------- ahpl_cond_timedwait --------------------\n");
    assert(0);
}

void ahpl_ref_hold_args(void) {
    printf("-------------------- ahpl_ref_hold_args --------------------\n");
    assert(0);
}

void ahpl_mpq_create_oneshot_timer(void) {
    printf("-------------------- ahpl_mpq_create_oneshot_timer --------------------\n");
    assert(0);
}

void ahpl_main_exit_wait(void) {
    printf("-------------------- ahpl_main_exit_wait --------------------\n");
    assert(0);
}

void ahpl_ip_sk_sendto(void) {
    printf("-------------------- ahpl_ip_sk_sendto --------------------\n");
    assert(0);
}

void ahpl_ref_hold_argv(void) {
    printf("-------------------- ahpl_ref_hold_argv --------------------\n");
    assert(0);
}

void ahpl_perf_set_callback(void) {
    printf("-------------------- ahpl_perf_set_callback --------------------\n");
    assert(0);
}

void ahpl_def_rt_str(void) {
    printf("-------------------- ahpl_def_rt_str --------------------\n");
    assert(0);
}

void ahpl_sendto(void) {
    printf("-------------------- ahpl_sendto --------------------\n");
    assert(0);
}

void ahpl_mpq_main(void) {
    printf("-------------------- ahpl_mpq_main --------------------\n");
    assert(0);
}

void ahpl_atomic_sub_return(void) {
    printf("-------------------- ahpl_atomic_sub_return --------------------\n");
    assert(0);
}

void ahpl_set_vlog_func(void) {
    printf("-------------------- ahpl_set_vlog_func --------------------\n");
    assert(0);
}

void ahpl_mpqp_pool_tail_queue_args(void) {
    printf("-------------------- ahpl_mpqp_pool_tail_queue_args --------------------\n");
    assert(0);
}

void ahpl_mpqp_pool_tail_queue_argv(void) {
    printf("-------------------- ahpl_mpqp_pool_tail_queue_argv --------------------\n");
    assert(0);
}

void ahpl_start_time_ms(void) {
    printf("-------------------- ahpl_start_time_ms --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DurationEst(void) {
    printf("-------------------- AgoraRtcOpus_DurationEst --------------------\n");
    assert(0);
}

void ahpl_mpq_timer_arg(void) {
    printf("-------------------- ahpl_mpq_timer_arg --------------------\n");
    assert(0);
}

void ahpl_ip_sk_addr_str(void) {
    printf("-------------------- ahpl_ip_sk_addr_str --------------------\n");
    assert(0);
}

void ahpl_ip_sk_addr_from_string(void) {
    printf("-------------------- ahpl_ip_sk_addr_from_string --------------------\n");
    assert(0);
}

void ahpl_mpq_del_fd(void) {
    printf("-------------------- ahpl_mpq_del_fd --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecoderCreate(void) {
    printf("-------------------- AgoraRtcOpus_DecoderCreate --------------------\n");
    assert(0);
}

void ahpl_psb_len(void) {
    printf("-------------------- ahpl_psb_len --------------------\n");
    assert(0);
}

void ahpl_invalidate_def_rt(void) {
    printf("-------------------- ahpl_invalidate_def_rt --------------------\n");
    assert(0);
}

void AgoraRtcOpus_SetPacketLossRate(void) {
    printf("-------------------- AgoraRtcOpus_SetPacketLossRate --------------------\n");
    assert(0);
}

void ahpl_rt_str(void) {
    printf("-------------------- ahpl_rt_str --------------------\n");
    assert(0);
}

void ahpl_init_def_rt(void) {
    printf("-------------------- ahpl_init_def_rt --------------------\n");
    assert(0);
}

void ahpl_dynamic_string_strcat(void) {
    printf("-------------------- ahpl_dynamic_string_strcat --------------------\n");
    assert(0);
}

void ahpl_unmarshal(void) {
    printf("-------------------- ahpl_unmarshal --------------------\n");
    assert(0);
}

void ahpl_atomic_intptr_inc(void) {
    printf("-------------------- ahpl_atomic_intptr_inc --------------------\n");
    assert(0);
}

void ahpl_event_wait(void) {
    printf("-------------------- ahpl_event_wait --------------------\n");
    assert(0);
}

void AgoraRtcOpus_FecDurationEst(void) {
    printf("-------------------- AgoraRtcOpus_FecDurationEst --------------------\n");
    assert(0);
}

void ahpl_mpq_add_stream_socket_on_q(void) {
    printf("-------------------- ahpl_mpq_add_stream_socket_on_q --------------------\n");
    assert(0);
}

void ahpl_psb_reset(void) {
    printf("-------------------- ahpl_psb_reset --------------------\n");
    assert(0);
}

void ahpl_tcp_resolve_host_async(void) {
    printf("-------------------- ahpl_tcp_resolve_host_async --------------------\n");
    assert(0);
}

void agora_rtc_send_audio_data(void) {
    printf("-------------------- agora_rtc_send_audio_data --------------------\n");
    assert(0);
}

void ahpl_so_register(void) {
    printf("-------------------- ahpl_so_register --------------------\n");
    assert(0);
}

void ahpl_get_git_branch(void) {
    printf("-------------------- ahpl_get_git_branch --------------------\n");
    assert(0);
}

void ahpl_mpq_kill_timer(void) {
    printf("-------------------- ahpl_mpq_kill_timer --------------------\n");
    assert(0);
}

void agora_rtc_mute_local_audio(void) {
    printf("-------------------- agora_rtc_mute_local_audio --------------------\n");
    assert(0);
}

void AgoraRtcOpus_DecodePlc(void) {
    printf("-------------------- AgoraRtcOpus_DecodePlc --------------------\n");
    assert(0);
}

void ahpl_mpq_fd_arg(void) {
    printf("-------------------- ahpl_mpq_fd_arg --------------------\n");
    assert(0);
}

void ahpl_ref_create(void) {
    printf("-------------------- ahpl_ref_create --------------------\n");
    assert(0);
}

void ahpl_shrink_resources(void) {
    printf("-------------------- ahpl_shrink_resources --------------------\n");
    assert(0);
}

void ahpl_mpq_resched_oneshot_timer(void) {
    printf("-------------------- ahpl_mpq_resched_oneshot_timer --------------------\n");
    assert(0);
}

void ahpl_psb_put(void) {
    printf("-------------------- ahpl_psb_put --------------------\n");
    assert(0);
}

void ahpl_ip_sk_bind_port_only(void) {
    printf("-------------------- ahpl_ip_sk_bind_port_only --------------------\n");
    assert(0);
}

void agora_rtc_license_gen_credential(void) {
    printf("-------------------- agora_rtc_license_gen_credential --------------------\n");
    assert(0);
}

void ahpl_task_remove_waiting_ops_head(void) {
    printf("-------------------- ahpl_task_remove_waiting_ops_head --------------------\n");
    assert(0);
}

void ahpl_alloc_user_psb(void) {
    printf("-------------------- ahpl_alloc_user_psb --------------------\n");
    assert(0);
}

void agora_rtc_join_channel(void) {
    printf("-------------------- agora_rtc_join_channel --------------------\n");
    assert(0);
}

void ahpl_cond_broadcast(void) {
    printf("-------------------- ahpl_cond_broadcast --------------------\n");
    assert(0);
}

void ahpl_dynamic_array_add_elems(void) {
    printf("-------------------- ahpl_dynamic_array_add_elems --------------------\n");
    assert(0);
}

void ahpl_module_call(void) {
    printf("-------------------- ahpl_module_call --------------------\n");
    assert(0);
}

void ahpl_rwlock_rdunlock(void) {
    printf("-------------------- ahpl_rwlock_rdunlock --------------------\n");
    assert(0);
}
