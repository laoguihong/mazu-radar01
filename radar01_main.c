#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include "radar01_http.h"
#include "radar01_io.h"
#include "radar01_tlv.h"
#include "radar01_utils.h"
#include "ringbuffer.h"
#include "vender/dpif_pointcloud.h"
#include "vender/mmw_output.h"


#define EPOLL_SIZE 256
#define MAX_EVENTS 256
#define BUF_SIZE 512
#define CONCURRENCY 1
#define EPOLL_RUN_TIMEOUT 1000
#define RINGBUFF_SIZE 4

#define MSG_HEADER_LENS (sizeof(MmwDemo_output_message_header))

static void server_err(const char *str)
{
    perror(str);
    exit(-1);
}

static char pointcloud_target[] = "/2020test/2020test";
static char vitalsign_target[] = "/2020test/sendVitalData";
static char host_port[] = "49.159.114.50:10003";
static char dss_devif[] = "/dev/ttyACM1";
struct radar01_io_info_t *iwr1642_dss_blk;
struct radar01_io_info_t *iwr1642_mss_blk;
struct radar01_http_user_t *arstu_server_blk;
static struct ringbuffer_t dss2http_ring = {0};

static int exit_i = 0;
static void signal_exit(int signal)
{
    (void) signal;
    exit_i++;
}

static int radar01_receive_process(int fd,
                                   uint8_t *rx_buff,
                                   int max_size,
                                   int epoll_fd,
                                   struct epoll_event *ev_recv)
{
    int offset = 0;
    MmwDemo_output_message_header msg_header = {0};
    static uint16_t magicWord[4] = {0x0102, 0x0304, 0x0506, 0x0708};
    offset = radar01_data_recv(fd, rx_buff, MSG_HEADER_LENS);
    debug_hex_dump("DSS Header ", rx_buff, offset);
    if (memcmp(rx_buff, magicWord, 8) != 0) {
        printf("[%s] Header Magic number not match Drop it !!\n", __FUNCTION__);
        return -1;
    }
    memcpy(&msg_header, rx_buff, MSG_HEADER_LENS);
    max_size = (int) msg_header.totalPacketLen < max_size
                   ? (int) msg_header.totalPacketLen
                   : max_size;
    while (offset < max_size) {
        int rdlen = 0;
        int epoll_events_count;
        if ((epoll_events_count =
                 epoll_wait(epoll_fd, ev_recv, 1024, EPOLL_RUN_TIMEOUT)) < 0)
            printf("[%s:%d] Remain packet received fail. [ERROR]: %s\n",
                   __FUNCTION__, __LINE__, strerror(errno));

        for (int i = 0; i < epoll_events_count; i++) {
            if (ev_recv[i].data.fd != fd)
                continue;
            rdlen = radar01_data_recv(fd, rx_buff + offset, max_size - offset);
            offset += rdlen;
        }
    }  // while
    debug_hex_dump("DSS Header+Content ", rx_buff, max_size);
    if (offset != max_size)
        printf(
            "[%s:%d] [WARNING] Received pkt size not patch. %d, expected %d \n",
            __FUNCTION__, __LINE__, offset, max_size);
    return offset;
}

struct device_worker_info {
    int epoll_fd;
    struct epoll_event ev_recv[EPOLL_SIZE];
    int dss_fd;
    uint8_t data_buff[1024];
    int (*process_msg_func_)(uint8_t *, int, void *);
    void (*data_dumper_func_)(void *);
    void (*create_json_func_)(void *, struct radar01_json_entry_t *, size_t);
    void *real_frame;
    int real_frame_size;
    // struct radar01_pointcloud_data_t Cartesian;
    // struct radar01_vitalsign_data_t VitalSign;
    struct ringbuffer_t *rbuf;
};

void *device_worker(void *v_param)
{
    struct device_worker_info *winfo;
    winfo = (struct device_worker_info *) v_param;
    winfo->rbuf = &dss2http_ring;
    while (!exit_i) {
        int epoll_events_count;
        if ((epoll_events_count =
                 epoll_wait(winfo->epoll_fd, &winfo->ev_recv[0], EPOLL_SIZE,
                            EPOLL_RUN_TIMEOUT)) < 0)
            server_err("Fail to wait epoll");
        // printf("epoll event count: %d\n", epoll_events_count);
        //        clock_t start_time = clock();
        for (int i = 0; i < epoll_events_count; i++) {
            /* EPOLLIN event for listener (new client connection) */
            if (winfo->ev_recv[i].data.fd == winfo->dss_fd) {
                int size = radar01_receive_process(
                    winfo->dss_fd, &winfo->data_buff[0], 1024, winfo->epoll_fd,
                    &winfo->ev_recv[0]);
                if (size > 0) {
                    winfo->process_msg_func_(&winfo->data_buff[0], size,
                                             winfo->real_frame);
                    winfo->data_dumper_func_(winfo->real_frame);
                    struct radar01_json_entry_t dss_share = {};
                    winfo->create_json_func_(winfo->real_frame, &dss_share,
                                             JSON_SZ);
                    dss_ring_enqueue(winfo->rbuf, (void *) &dss_share,
                                     sizeof(dss_share));
                }

            } else {
                // /* EPOLLIN event for others (new incoming message from
                // client)
                //  */
                // if (handle_message_from_client(events[i].data.fd, &list) < 0)
                //     server_err("Handle message from client", &list);
            }
        }
    }
    return NULL;
}

struct http_worker_info {
    int epoll_fd;
    pthread_t tid;
    char *target;
    struct radar01_http_user_t hu;
    struct ringbuffer_t *rbuf;
};

/* ToDo: Use epoll socket */
void *http_worker(void *v_param)
{
    struct http_worker_info *winfo;
    struct epoll_event ev_recv[MAX_EVENTS];
    struct radar01_http_conn_t hconn[CONCURRENCY], *ehc;
    int nevts = 0;
    int ret = 0;
    winfo = (struct http_worker_info *) v_param;
    if ((winfo->epoll_fd = epoll_create1(0)) < 0) {
        printf("Fail to create epoll\n");
        goto thread_exit;
    }
    /*Bind the Ringbuffer*/
    winfo->rbuf = &dss2http_ring;
    /* Connect to same Host*/
    for (int i = 0; i < CONCURRENCY; ++i)
        http_connect_server(winfo->epoll_fd, hconn + i, &winfo->hu.http_addr);
    static struct radar01_json_entry_t http_datapub = {};

    static char outbuf[2048] = {0};
    char inbuf[1024] = {0};
    while (!exit_i) {
        do {
            nevts = epoll_wait(winfo->epoll_fd, ev_recv, MAX_EVENTS, 5000);
        } while (!exit_i && nevts < 0 && errno == EINTR);

        if (exit_i != 0) {
            for (int i = 0; i < CONCURRENCY; ++i)
                close(hconn[i].sockfd);
            close(winfo->epoll_fd);
            printf("Closing the epoll_fd and HTTP socket !!\n");
            goto thread_exit;
        }
        int error = 0;
        socklen_t errlen = sizeof(error);
        for (int i = 0; i < CONCURRENCY; ++i) {
            if (getsockopt(hconn[i].sockfd, SOL_SOCKET, SO_ERROR,
                           (void *) &error, &errlen) == 0) {
                if (!error)
                    break;
                fprintf(stderr, "[Warning] cause = %s\n", strerror(error));
                nevts = 0;
                ret = http_connect_server(winfo->epoll_fd, hconn + i,
                                          &winfo->hu.http_addr);
            }
        }
        if (!nevts || ret < 0)
            continue;
        for (int n = 0; n < nevts; ++n) {
            ehc = (struct radar01_http_conn_t *) ev_recv[n].data.ptr;
            if (ev_recv[n].events & EPOLLOUT) {
                /*Dequeue data from the dss first*/
                memset(&http_datapub, 0, sizeof(struct radar01_json_entry_t));
                http_ring_dequeue(winfo->rbuf, (void *) &http_datapub,
                                  sizeof(http_datapub));
                memset(&outbuf, 0, 2048);
                int size = create_http_request_msg(
                    winfo->target, &http_datapub.payload[0],
                    &(winfo->hu.server_url[0]), outbuf, 2048);
                if (RADAR01_HTTP_DEBUG_ENABLE == 1)
                    printf("HTTP total request len: %d\n%s\n", size, outbuf);
                /* Send the http request */
                int ret = radar01_http_send(ehc->sockfd, outbuf, size);

                if (ret > 0) {
                    /* write done? schedule read */
                    ev_recv[n].events = EPOLLIN;
                    if (epoll_ctl(winfo->epoll_fd, EPOLL_CTL_MOD, ehc->sockfd,
                                  ev_recv + n)) {
                        perror("epoll_ctl");
                        exit(1);
                    }

                } else {
                    fprintf(stderr,
                            "[%s:%d] Something Wrong at send http packet\n",
                            __FUNCTION__, __LINE__);
                }

            } else if (ev_recv[n].events & EPOLLIN) {
                int len = radar01_http_recv(ehc->sockfd, inbuf, 1023);
                if (len > 0) {
                    inbuf[len] = '\0';
                    if (RADAR01_HTTP_DEBUG_ENABLE == 1)
                        printf("%s\n", inbuf);
                    ev_recv[n].events = EPOLLOUT;
                    epoll_ctl(winfo->epoll_fd, EPOLL_CTL_MOD, ehc->sockfd,
                              ev_recv + n);
                }
            }
        }  // recv event poll
    }      // Thread main loop
thread_exit:
    for (int i = 0; i < CONCURRENCY; ++i)
        shutdown(hconn[i].sockfd, SHUT_WR);
    return NULL;
}

static const char short_options[] = "d:vp";

static const struct option long_options[] = {{"help", 0, NULL, '%'},
                                             {"vitalsign", 0, NULL, 'v'},
                                             {"pointclout", 0, NULL, 'p'},
                                             {"device", 1, NULL, 'd'},
                                             {NULL, 0, NULL, 0}};

static void print_usage()
{
    printf(
        "Usage: radar01 [options] [http://]hostname[:port]/path\n"
        "Options:\n"
        "   -v, --vitalsign     firmware type: vitalsign\n"
        "   -p, --pointclouud   firmware type: pointclouud\n"
        "   -d, --device       radar char device file\n"
        "   --help             display this message\n");
    exit(0);
}

enum { POINTCLOUD_E = 0, VITALSIGN_E };

int main(int argc, char const *argv[])
{
    int rc = 0;
    /* Argument options */
    int next_option;
    char *arg_radar_dev = NULL;
    int found_pointcloud = 0;
    int found_vitalsign = 0;
    if (argc == 1)
        print_usage();
    do {
        next_option = getopt_long(argc, (char **) argv, short_options,
                                  long_options, NULL);
        switch (next_option) {
        case '%':
            print_usage();
            break;
        case 'd':
            arg_radar_dev = optarg;
            break;
        case 'p':
            found_pointcloud++;
            break;
        case 'v':
            found_vitalsign++;
            break;
        case -1:
            break;
        default:
            printf("Unexpected argument: '%c'\n", next_option);
            return 1;
        }
    } while (next_option != -1);
    /*Signal Handleer*/
    __sighandler_t ret = signal(SIGINT, signal_exit);
    if (ret == SIG_ERR) {
        perror("signal(SIGINT, handler)");
        exit(0);
    }

    ret = signal(SIGTERM, signal_exit);
    if (ret == SIG_ERR) {
        perror("signal(SIGTERM, handler)");
        exit(0);
    }
    /* TODO: Move to long options*/
    int ep_event = 0;
    if (argc > 1)
        if (strcmp(argv[1], "-et") == 0) {
            ep_event = EPOLLET;
        }
    /* Init ringbuffer first */
    rb_init(&dss2http_ring, RINGBUFF_SIZE);
    /* code */
    struct device_worker_info *dev_worker;
    dev_worker = calloc(1, sizeof(struct device_worker_info));
    if (!dev_worker)
        return -1;
    static char csv_title[256] = {0};
    if (found_vitalsign) {
        dev_worker->process_msg_func_ = process_vitalsign_msg;  // default
        dev_worker->data_dumper_func_ = vitalsign_stats_dump;
        dev_worker->create_json_func_ = vitalsign_create_json_msg;
        dev_worker->real_frame_size = sizeof(struct radar01_vitalsign_data_t);
        dev_worker->real_frame =
            calloc(1, sizeof(struct radar01_vitalsign_data_t));
        strncpy(
            csv_title,
            "Frame Seq, OF_HeartOut, BR_Est_FFT, BR_Est_xCorr, BR_peakCount",
            256);
    } else {
        dev_worker->process_msg_func_ = process_pointcloud_msg;  // default
        dev_worker->data_dumper_func_ = pointcloud_Cartesian_info_dump;
        dev_worker->create_json_func_ = pointcloud_create_json_msg;
        dev_worker->real_frame_size = sizeof(struct radar01_pointcloud_data_t);
        dev_worker->real_frame =
            calloc(1, sizeof(struct radar01_pointcloud_data_t));
        strncpy(csv_title,
                "Frame Seq, Obj_index, x, y, z, velocity, snr, noise", 256);
    }
    if (!dev_worker->real_frame) {
        free(dev_worker->real_frame);
        printf("[%s:%d] Failed to allocate real_frame buffer, Exit!!\n",
               __FUNCTION__, __LINE__);
        goto failed_allloc_real_frame;
    }
    if (arg_radar_dev == NULL) {
        arg_radar_dev = dss_devif;
    }

    rc = radar01_io_init(arg_radar_dev, (void *) &iwr1642_dss_blk);
    uint8_t is_device_worker_ready = 0;
    if (rc < 0) {
        printf("[Warning] Fail to radar01_io_init. skipped\n");
        iwr1642_dss_blk = NULL;
        radar01_free_mem((void **) &dev_worker);
        is_device_worker_ready = 0;
    } else {
        is_device_worker_ready = 1;
    }
    /*Device IO init */
    int epoll_fd;
    static struct epoll_event ev;
    if ((epoll_fd = epoll_create(EPOLL_SIZE)) < 0)
        server_err("Fail to create epoll");
    if (is_device_worker_ready) {
        ev.events = EPOLLIN | ep_event;
        ev.data.fd = iwr1642_dss_blk->dss_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, iwr1642_dss_blk->dss_fd, &ev) <
            0)
            server_err("Fail to control epoll");
        printf("Listener (fd=%d) was added to epoll.\n", epoll_fd);
        dev_worker->epoll_fd = epoll_fd;
        dev_worker->dss_fd = iwr1642_dss_blk->dss_fd;
    }

    /* HTTP Client Init*/
    struct http_worker_info *http_winfo;
    uint8_t is_hp_worker_ready = 0;
    http_winfo = calloc(1, sizeof(struct http_worker_info));
    if (!http_winfo) {
        is_hp_worker_ready = 1;
        printf("Not enough HTTP worker memory.\n");
        goto exit_0;
    }
    rc = radar01_http_user_init(host_port, (void *) &http_winfo->hu);
    if (rc < 0)
        is_hp_worker_ready = 0;
    else {
        is_hp_worker_ready = 1;
    }
    http_winfo->target = pointcloud_target;
    if (found_vitalsign) {
        http_winfo->target = vitalsign_target;
    }
    if (is_device_worker_ready) {
        printf("%s\n", csv_title);
    }
    pthread_t dev_tid0;
    if (is_device_worker_ready) {
        rc = pthread_create(&dev_tid0, 0, &device_worker, (void *) dev_worker);
        if (rc < 0) {
            printf("[ERROR] Device Thread create fail rc = %d\n", rc);
            goto exit_0;
        }
    }

    pthread_t hp_tid1;
    if (is_hp_worker_ready) {
        rc = pthread_create(&hp_tid1, 0, &http_worker, (void *) http_winfo);
        if (rc < 0) {
            printf("[ERROR] HTTP Thread create fail rc = %d\n", rc);
            goto exit_0;
        }
    }
    void *cancel_hook = NULL;
    if (is_device_worker_ready)
        pthread_join(dev_tid0, &cancel_hook);
    if (is_hp_worker_ready)
        pthread_join(hp_tid1, &cancel_hook);

exit_0:
    radar01_io_deinit((void *) &iwr1642_dss_blk);
    rb_deinit(&dss2http_ring);
    close(epoll_fd);
    printf("Free the dev_worker memory.\n");
    if (dev_worker->real_frame)
        free(dev_worker->real_frame);
failed_allloc_real_frame:
    if (dev_worker)
        free(dev_worker);
}
