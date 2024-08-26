/*
 * TinyEMU
 * 
 * Copyright (c) 2016-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#ifndef _WIN32
//#include <termios.h>
#include <sys/ioctl.h>
//#include <net/if.h>
//#include <linux/if_tun.h>
#endif
#include <sys/stat.h>
#include <signal.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"
#ifdef CONFIG_FS_NET
#include "fs_utils.h"
#include "fs_wget.h"
#endif
#ifdef CONFIG_SLIRP
#include "slirp/libslirp.h"
#endif

#ifdef WASI
#include <wizer.h>
#include "wasi.h"
#endif

extern char **environ;

#ifdef ON_BROWSER
#include <emscripten.h>
#endif

#include <sys/socket.h>
#include <arpa/inet.h>
#ifndef ON_BROWSER
// not defined in wasi-libc
#define PF_INET 1
#define PF_INET6 2
#endif

#ifndef _WIN32

typedef struct {
    int stdin_fd;
    int console_esc_state;
    BOOL resize_pending;
} STDIODevice;

//static struct termios oldtty;
//static int old_fd0_flags;
//static STDIODevice *global_stdio_device;
//
//static void term_exit(void)
//{
//    tcsetattr (0, TCSANOW, &oldtty);
//    fcntl(0, F_SETFL, old_fd0_flags);
//}
//
//static void term_init(BOOL allow_ctrlc)
//{
//    struct termios tty;
//
//    memset(&tty, 0, sizeof(tty));
//    tcgetattr (0, &tty);
//    oldtty = tty;
//    old_fd0_flags = fcntl(0, F_GETFL);
//
//    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
//                          |INLCR|IGNCR|ICRNL|IXON);
//    tty.c_oflag |= OPOST;
//    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
//    if (!allow_ctrlc)
//        tty.c_lflag &= ~ISIG;
//    tty.c_cflag &= ~(CSIZE|PARENB);
//    tty.c_cflag |= CS8;
//    tty.c_cc[VMIN] = 1;
//    tty.c_cc[VTIME] = 0;
//
//    tcsetattr (0, TCSANOW, &tty);
//
//    atexit(term_exit);
//}

int init_start = FALSE;
int init_done = FALSE;

#define INIT_STR_POS_MAX 10
int init_str_pos = 0;
char init_str_buf[INIT_STR_POS_MAX];

static void console_write(void *opaque, const uint8_t *buf, int len)
{
    int off = 0;
    if (!init_start) {
      for (int i = 0; i < len; i++) {
        if (buf[i] == '=') {
          init_str_buf[init_str_pos++] = buf[i];
          off++;
          if (init_str_pos == INIT_STR_POS_MAX) {
            // NOTE: init string isn't printed
            init_start = TRUE;
            break;
          }
        } else {
          // flush all and reset counter
          fwrite(init_str_buf, 1, init_str_pos, stdout);
          init_str_pos = 0;
        }
      }
    }
    fwrite(buf+off, 1, len-off, stdout);
    fflush(stdout);
}

static int console_read_stdin(void *opaque, uint8_t *buf, int len)
{
    STDIODevice *s = opaque;
    int ret, i, j;
    uint8_t ch;
    
    if (len <= 0)
        return 0;

    ret = read(s->stdin_fd, buf, len);
    if (ret < 0)
        return 0;
    if (ret == 0) {
        /* EOF */
        exit(1);
    }

    j = 0;
    for(i = 0; i < ret; i++) {
        ch = buf[i];
        if (s->console_esc_state) {
            s->console_esc_state = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                exit(0);
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n"
                       );
                break;
            case 1:
                goto output_char;
            default:
                break;
            }
        } else {
            if (ch == 1) {
                s->console_esc_state = 1;
            } else {
            output_char:
                buf[j++] = ch;
            }
        }
    }
    return j;
}

const char *init_done_str = "=\n";
int init_done_str_pos = 0;

static int console_read(void *opaque, uint8_t *buf, int len)
{
  if ((init_start) && (!init_done)) {
    int l = min_int(strlen(init_done_str) - init_done_str_pos, len);
    memcpy(buf, init_done_str + init_done_str_pos, l);
    init_done_str_pos += l;
    if (init_done_str_pos >= strlen(init_done_str)) {
      init_done = TRUE;
    }
    return l;
  }
  return console_read_stdin(opaque, buf, len);
}

//static void term_resize_handler(int sig)
//{
//    if (global_stdio_device)
//        global_stdio_device->resize_pending = TRUE;
//}

static void console_get_size(STDIODevice *s, int *pw, int *ph)
{
//  struct winsize ws;
   int width, height;
   /* default values */
   width = 80;
   height = 25;
//  if (ioctl(s->stdin_fd, TIOCGWINSZ, &ws) == 0 &&
//      ws.ws_col >= 4 && ws.ws_row >= 4) {
//      width = ws.ws_col;
//      height = ws.ws_row;
//  }
   *pw = width;
   *ph = height;
}

CharacterDevice *console_init(BOOL allow_ctrlc)
{
    CharacterDevice *dev;
    STDIODevice *s;
    /* struct sigaction sig; */

    //term_init(allow_ctrlc);

    dev = mallocz(sizeof(*dev));
    s = mallocz(sizeof(*s));
    s->stdin_fd = 0;
    /* Note: the glibc does not properly tests the return value of
       write() in printf, so some messages on stdout may be lost */
    fcntl(s->stdin_fd, F_SETFL, O_NONBLOCK);

    s->resize_pending = TRUE;
    //global_stdio_device = s;
    
    /* use a signal to get the host terminal resize events */
    //sig.sa_handler = term_resize_handler;
    //sigemptyset(&sig.sa_mask);
    //sig.sa_flags = 0;
    //sigaction(SIGWINCH, &sig, NULL);
    
    dev->opaque = s;
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}

#endif /* !_WIN32 */

typedef enum {
    BF_MODE_RO,
    BF_MODE_RW,
    BF_MODE_SNAPSHOT,
} BlockDeviceModeEnum;

#define SECTOR_SIZE 512

typedef struct BlockDeviceFile {
    FILE *f;
    int64_t nb_sectors;
    BlockDeviceModeEnum mode;
    uint8_t **sector_table;
} BlockDeviceFile;

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceFile *bf = bs->opaque;
    return bf->nb_sectors;
}

//#define DUMP_BLOCK_READ

static int bf_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
                fread(buf, 1, SECTOR_SIZE, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fread(buf, 1, n * SECTOR_SIZE, bf->f);
    }
    /* synchronous read */
    return 0;
}

static int bf_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    int ret;

    switch(bf->mode) {
    case BF_MODE_RO:
        ret = -1; /* error */
        break;
    case BF_MODE_RW:
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f);
        ret = 0;
        break;
    case BF_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num] = malloc(SECTOR_SIZE);
                }
                memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

static BlockDevice *block_device_init(const char *filename,
                                      BlockDeviceModeEnum mode)
{
    BlockDevice *bs;
    BlockDeviceFile *bf;
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BF_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }
    
    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f = f;

    if (mode == BF_MODE_SNAPSHOT) {
        bf->sector_table = mallocz(sizeof(bf->sector_table[0]) *
                                   bf->nb_sectors);
    }
    
    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    return bs;
}

#ifndef _WIN32

//typedef struct {
//    int fd;
//    BOOL select_filled;
//} TunState;
//
//static void tun_write_packet(EthernetDevice *net,
//                             const uint8_t *buf, int len)
//{
//    TunState *s = net->opaque;
//    write(s->fd, buf, len);
//}
//
//static void tun_select_fill(EthernetDevice *net, int *pfd_max,
//                            fd_set *rfds, fd_set *wfds, fd_set *efds,
//                            int *pdelay)
//{
//    TunState *s = net->opaque;
//    int net_fd = s->fd;
//
//    s->select_filled = net->device_can_write_packet(net);
//    if (s->select_filled) {
//        FD_SET(net_fd, rfds);
//        *pfd_max = max_int(*pfd_max, net_fd);
//    }
//}
//
//static void tun_select_poll(EthernetDevice *net, 
//                            fd_set *rfds, fd_set *wfds, fd_set *efds,
//                            int select_ret)
//{
//    TunState *s = net->opaque;
//    int net_fd = s->fd;
//    uint8_t buf[2048];
//    int ret;
//    
//    if (select_ret <= 0)
//        return;
//    if (s->select_filled && FD_ISSET(net_fd, rfds)) {
//        ret = read(net_fd, buf, sizeof(buf));
//        if (ret > 0)
//            net->device_write_packet(net, buf, ret);
//    }
//    
//}

/* configure with:
# bridge configuration (connect tap0 to bridge interface br0)
   ip link add br0 type bridge
   ip tuntap add dev tap0 mode tap [user x] [group x]
   ip link set tap0 master br0
   ip link set dev br0 up
   ip link set dev tap0 up

# NAT configuration (eth1 is the interface connected to internet)
   ifconfig br0 192.168.3.1
   echo 1 > /proc/sys/net/ipv4/ip_forward
   iptables -D FORWARD 1
   iptables -t nat -A POSTROUTING -o eth1 -j MASQUERADE

   In the VM:
   ifconfig eth0 192.168.3.2
   route add -net 0.0.0.0 netmask 0.0.0.0 gw 192.168.3.1
*/
//static EthernetDevice *tun_open(const char *ifname)
//{
//    struct ifreq ifr;
//    int fd, ret;
//    EthernetDevice *net;
//    TunState *s;
//    
//    fd = open("/dev/net/tun", O_RDWR);
//    if (fd < 0) {
//        fprintf(stderr, "Error: could not open /dev/net/tun\n");
//        return NULL;
//    }
//    memset(&ifr, 0, sizeof(ifr));
//    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
//    pstrcpy(ifr.ifr_name, sizeof(ifr.ifr_name), ifname);
//    ret = ioctl(fd, TUNSETIFF, (void *) &ifr);
//    if (ret != 0) {
//        fprintf(stderr, "Error: could not configure /dev/net/tun\n");
//        close(fd);
//        return NULL;
//    }
//    fcntl(fd, F_SETFL, O_NONBLOCK);
//
//    net = mallocz(sizeof(*net));
//    net->mac_addr[0] = 0x02;
//    net->mac_addr[1] = 0x00;
//    net->mac_addr[2] = 0x00;
//    net->mac_addr[3] = 0x00;
//    net->mac_addr[4] = 0x00;
//    net->mac_addr[5] = 0x01;
//    s = mallocz(sizeof(*s));
//    s->fd = fd;
//    net->opaque = s;
//    net->write_packet = tun_write_packet;
//    net->select_fill = tun_select_fill;
//    net->select_poll = tun_select_poll;
//    return net;
//}

#endif /* !_WIN32 */

#ifdef CONFIG_SLIRP

/*******************************************************/
/* slirp */

static Slirp *slirp_state;

static void slirp_write_packet(EthernetDevice *net,
                               const uint8_t *buf, int len)
{
    Slirp *slirp_state = net->opaque;
    slirp_input(slirp_state, buf, len);
}

int slirp_can_output(void *opaque)
{
    EthernetDevice *net = opaque;
    return net->device_can_write_packet(net);
}

void slirp_output(void *opaque, const uint8_t *pkt, int pkt_len)
{
    EthernetDevice *net = opaque;
    return net->device_write_packet(net, pkt, pkt_len);
}

static void slirp_select_fill1(EthernetDevice *net, int *pfd_max,
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int *pdelay)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_fill(slirp_state, pfd_max, rfds, wfds, efds);
}

static void slirp_select_poll1(EthernetDevice *net, 
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int select_ret)
{
    Slirp *slirp_state = net->opaque;
    slirp_select_poll(slirp_state, rfds, wfds, efds, (select_ret <= 0));
}

static EthernetDevice *slirp_open(void)
{
    EthernetDevice *net;
    struct in_addr net_addr  = { .s_addr = htonl(0x0a000200) }; /* 10.0.2.0 */
    struct in_addr mask = { .s_addr = htonl(0xffffff00) }; /* 255.255.255.0 */
    struct in_addr host = { .s_addr = htonl(0x0a000202) }; /* 10.0.2.2 */
    struct in_addr dhcp = { .s_addr = htonl(0x0a00020f) }; /* 10.0.2.15 */
    struct in_addr dns  = { .s_addr = htonl(0x0a000203) }; /* 10.0.2.3 */
    const char *bootfile = NULL;
    const char *vhostname = NULL;
    int restricted = 0;
    
    if (slirp_state) {
        fprintf(stderr, "Only a single slirp instance is allowed\n");
        return NULL;
    }
    net = mallocz(sizeof(*net));

    slirp_state = slirp_init(restricted, net_addr, mask, host, vhostname,
                             "", bootfile, dhcp, dns, net);
    
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    net->opaque = slirp_state;
    net->write_packet = slirp_write_packet;
    net->select_fill = slirp_select_fill1;
    net->select_poll = slirp_select_poll1;
    
    return net;
}

#endif /* CONFIG_SLIRP */

/*******************************************************/
/* socket */

typedef struct {
    int fd;
    BOOL select_filled;
    char *raw_flag;

    int tmpfd;
    BOOL enabled;
    int next_watch;
    int retrynum;

    uint32_t sizebuf;
    int sizeoff;
    int pktsize;
    int pktoff;
    uint8_t *pktbuf;
} SocketState;

static void socket_write_packet(EthernetDevice *net,
                               const uint8_t *buf, int len)
{
    SocketState *s = net->opaque;
    uint32_t size = htonl(len);
    int ret;

    if (s->fd < 0) {
      return;
    }

    ret = send(s->fd, &size, 4, 0); // TODO: check error
    if (ret < 0) {
      close(s->fd);
      s->fd = -1;
      return;
    }
    ret = send(s->fd, buf, len, 0); // TODO: check error
    if (ret < 0) {
      close(s->fd);
      s->fd = -1;
      return;
    }
}

static int try_get_fd(SocketState *s);

static int socket_watch1(EthernetDevice *net)
{
  SocketState *s = (SocketState *)net->opaque;

  if (!s->enabled)
    return -1;

  if (s->fd >= 0)
    return 0;

  s->next_watch--;
  if (s->next_watch > 0)
    return -1;
  if (try_get_fd(s) >= 0)
    return 0;
  if (s->next_watch <= 0) {
    s->next_watch = 500; // TODO: make it configable or should be time interval
  }
  
  return -1;
}

static void socket_select_fill1(EthernetDevice *net, int *pfd_max,
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int *pdelay)
{
   SocketState *s = net->opaque;
   int fd = s->fd;

   if (s->fd < 0) {
     return;
   }

   s->select_filled = net->device_can_write_packet(net);
   if (s->select_filled) {
       FD_SET(fd, rfds);
       *pfd_max = max_int(*pfd_max, fd);
   }
}

static void socket_select_poll1(EthernetDevice *net, 
                               fd_set *rfds, fd_set *wfds, fd_set *efds,
                               int select_ret)
{
   SocketState *s = net->opaque;
   int fd = s->fd;
   uint32_t size = 0;
   int ret;
   
   if (s->fd < 0) {
     return;
   }

   if (select_ret <= 0)
       return;

   if (s->select_filled && FD_ISSET(fd, rfds)) {
     if (s->pktsize <= 0) {
       while (s->sizeoff < 4) {
         ret = recv(fd, &s->sizebuf + s->sizeoff, 4 - s->sizeoff, 0);
         if (ret <= 0) {
           if (errno == EAGAIN)
             return; // try later
           close(s->fd);
           s->fd = -1; // invalid fd. hopefully the watch loop will recover this.
           return;
         }
         s->sizeoff += ret;
       }
       s->pktsize = ntohl(s->sizebuf);
       s->sizeoff = 0;
       s->sizebuf = 0;
     }

     if (s->pktsize > 0) {
       size = s->pktsize;
       if (s->pktbuf == NULL) {
         s->pktoff = 0;
         s->pktbuf = (uint8_t *)mallocz(size); // TODO: make limit
       }
       while (s->pktoff < size) {
         ret = recv(fd, s->pktbuf + s->pktoff, size - s->pktoff, 0);
         if (ret <= 0) {
           if (errno == EAGAIN)
             return; // try later
           close(s->fd);
           s->fd = -1; // invalid fd. hopefully the watch loop will recover this.
           return;
         }
         s->pktoff += ret;
       }
       net->device_write_packet(net, s->pktbuf, size);
       free(s->pktbuf);
       s->pktbuf = NULL;
       s->pktsize = 0;
       s->pktoff = 0;
     }
   }
}

static EthernetDevice *socket_net_init()
{
    EthernetDevice *net;
    SocketState *s;

    net = mallocz(sizeof(*net));
    net->mac_addr[0] = 0x02;
    net->mac_addr[1] = 0x00;
    net->mac_addr[2] = 0x00;
    net->mac_addr[3] = 0x00;
    net->mac_addr[4] = 0x00;
    net->mac_addr[5] = 0x01;
    net->opaque = NULL;
    s = mallocz(sizeof(*s));
    s->fd = -1;
    s->tmpfd = -1;
    s->enabled = FALSE;
    s->next_watch = 0;
    net->opaque = s;
    net->write_packet = socket_write_packet;
    net->select_fill = socket_select_fill1;
    net->select_poll = socket_select_poll1;
    net->watch = socket_watch1;

    return net;
}

static void reset_socket_socket_state(SocketState *s)
{
    s->sizebuf = 0;
    s->sizeoff = 0;
    s->pktsize = 0;
    s->pktoff = 0;
    if (s->pktbuf != NULL) {
      free(s->pktbuf);
    }
    s->pktbuf = NULL;
}

#ifdef ON_BROWSER
static int try_get_fd(SocketState *s)
{
    int sock= s->fd;
    fd_set wfds;
    struct sockaddr_in socketAddr;

    if (s->tmpfd < 0) {
      if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        printf("failed to prepare socket: %s\n", strerror(errno));
        return -1;
      }
      // connect to the network stack
      memset(&socketAddr, 0, sizeof(socketAddr));
      socketAddr.sin_family = AF_INET;
      int cret = connect(sock, (struct sockaddr *) &socketAddr, sizeof(socketAddr));
      BOOL connected = FALSE;
      if ((cret == 0) || (errno == EISCONN)) {
        /* printf("socket connected\n"); */
        s->fd = sock;
        s->tmpfd = -1;
        reset_socket_socket_state(s);
        return 0;
      } else if (errno != EINPROGRESS) {
        printf("failed to connect: %s\n", strerror(errno));
        s->fd = -1;
        s->tmpfd = -1;
        return -1;
      }
      s->retrynum = 0;
      s->tmpfd = sock;
    }

    /* printf("waiting for connection...\n"); */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&wfds);
    FD_SET(s->tmpfd, &wfds);
    int sret = select(s->tmpfd + 1, NULL, &wfds, NULL, &tv);
    if (sret > 0) {
      s->fd = s->tmpfd;
      s->tmpfd = -1;
      reset_socket_socket_state(s);
      return 0;
    } else if (sret < 0) {
      s->fd = -1;
      s->tmpfd = -1;
    } // else, still wait for the connection.
    /* printf("select result: %d\n", sret); */

    s->retrynum++;
    if (s->retrynum > 5) { // TODO: make max retry number configurable
      close(s->tmpfd);
      s->tmpfd = -1; // too many errors. retry connection.
    }

    return -1;
}
#elif defined(WASI)
static int try_get_fd(SocketState *s)
{
  int sock = 3;
  if ((s->raw_flag != NULL) && (!strncmp(s->raw_flag, "socket=", 7))) {
    // TODO: allow specifying more options
    if (!strncmp(s->raw_flag + 7, "listenfd=", 9)) {
      sock = atoi(s->raw_flag + 7 + 9);
    }
  }
  int sock_a = 0;
  // wait for connection
  /* printf("accept trying...(sockfd=%d)\n", sock); */
  sock_a = accept4(sock, NULL, NULL, SOCK_NONBLOCK);
  if (sock_a > 0) {
    /* printf("accepted fd=%d\n", sock_a); */
    s->fd = sock_a;
    reset_socket_socket_state(s);
    return 0;
  }
  /* printf("failed to accept socket: %s\n", strerror(errno)); */
  return -1;
}
#endif

#define MAX_EXEC_CYCLE 500000
#define MAX_SLEEP_TIME 10 /* in ms */

void virt_machine_run(VirtMachine *m, int enable_stdin)
{
    fd_set rfds, wfds, efds;
    int fd_max, ret = 0, delay;
    struct timeval tv;
#ifndef _WIN32
    int stdin_fd;
#endif

    delay = virt_machine_get_sleep_duration(m, MAX_SLEEP_TIME);
    
    /* wait for an event */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    fd_max = -1;
#ifndef _WIN32
    if (enable_stdin && m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        STDIODevice *s = m->console->opaque;
        stdin_fd = s->stdin_fd;
        FD_SET(stdin_fd, &rfds);
        fd_max = stdin_fd;

        if (s->resize_pending) {
            int width, height;
            console_get_size(s, &width, &height);
            virtio_console_resize_event(m->console_dev, width, height);
            s->resize_pending = FALSE;
        }
    }
#endif
    fd_set n_rfds, n_wfds, n_efds;
    FD_ZERO(&n_rfds);
    FD_ZERO(&n_wfds);
    FD_ZERO(&n_efds);
    int n_fd_max = -1;
    if (m->net) {
      m->net->select_fill(m->net, &n_fd_max, &n_rfds, &n_wfds, &n_efds, &delay);
    }
#ifdef CONFIG_FS_NET
    fs_net_set_fdset(&n_fd_max, &n_rfds, &n_wfds, &n_efds, &delay);
#endif
    tv.tv_sec = delay / 1000;
    tv.tv_usec = (delay % 1000) * 1000;
#ifdef ON_BROWSER
    // no timeout; TODO: remove this when sleep supports timeout
    tv.tv_sec = 0;
    tv.tv_usec = 0;
#endif
    if (enable_stdin && init_done)
        ret = select(fd_max + 1, &rfds, &wfds, NULL, &tv);
    if (m->net) {
      if (n_fd_max >= 0) {
        // TODO: select should be unified (stdin + net), but it doesn't work as of now.
        int n_ret = select(n_fd_max + 1, &n_rfds, &n_wfds, NULL, &tv);
        m->net->select_poll(m->net, &n_rfds, &n_wfds, &n_efds, n_ret);
      }
      m->net->watch(m->net);
    }
    if ((ret > 0) || (init_start && !init_done)) {
#ifndef _WIN32
        if (m->console_dev && (FD_ISSET(stdin_fd, &rfds) || (init_start && !init_done))) {
            uint8_t buf[128];
            int ret, len;
            len = virtio_console_get_write_len(m->console_dev);
            len = min_int(len, sizeof(buf));
            ret = m->console->read_data(m->console->opaque, buf, len);
            if (ret > 0) {
              virtio_console_write_data(m->console_dev, buf, ret);
            }
        }
#endif
    }

#ifdef CONFIG_SDL
    sdl_refresh(m);
#endif
    
#ifdef ON_BROWSER
    emscripten_sleep(delay);
#endif
    virt_machine_interp(m, MAX_EXEC_CYCLE);
}

/*******************************************************/

static struct option options[] = {
    { "help", no_argument, NULL, 'h' },
    { "no-stdin", no_argument },
    { "entrypoint", required_argument },
    { "net", required_argument },
    { "mac", required_argument },
    { "external-bundle", required_argument },
// The following flags are unsupported as of now:
//    { "ctrlc", no_argument },
//    { "rw", no_argument },
//    { "ro", no_argument },
//    { "append", required_argument },
//    { "no-accel", no_argument },
//    { "build-preload", required_argument },
    { NULL },
};

void help(void)
{
   printf("USAGE: command [options] [COMMAND] [ARG...] \n"
          "  [COMMAND] [ARG...]: command to run in the container. (default: commands specified in the image config)\n"
          "\n"
          "OPTIONS:\n"
          "  -entrypoint <command>     : entrypoint command. (default: entrypoint specified in the image config)\n"
          "  -no-stdin                 : disable stdin. (default: false)\n"
          "  -net <mode>               : enable networking with the specified mode (default: disabled. supported mode: \"socket\")\n"
          "  -mac <mac address>        : use a custom mac address for the VM\n"
          "  -external-bundle <address>: externally mount container bundle\n"
          "\n"
          "This tool is based on:\n"
          "temu version 2019-12-21, Copyright (c) 2016-2018 Fabrice Bellard\n"
          );
   exit(0);
}

#ifdef CONFIG_FS_NET
static BOOL net_completed;

static void net_start_cb(void *arg)
{
    net_completed = TRUE;
}

static BOOL net_poll_cb(void *arg)
{
    return net_completed;
}

#endif

FSVirtFile *info;
int initialized = FALSE;
VirtMachine *s;
VirtMachineParams p_s, *p = &p_s;

void init_func_args(char *net)
{
    const char *path, *cmdline = NULL;
    int i = 0;
    int ram_size = -1;
    int accel_enable = -1; // TODO: make it overwritable
    BOOL allow_ctrlc = FALSE; // TODO: make it overwritable
    BlockDeviceModeEnum drive_mode = BF_MODE_SNAPSHOT; // TODO: make it overwritable
    FSDevice *fs;

    path = "/pack/config"; // default location

    virt_machine_set_defaults(p);
#ifdef CONFIG_FS_NET
    fs_wget_init();
#endif
    virt_machine_load_config_file(p, path, NULL, NULL);
#ifdef CONFIG_FS_NET
    fs_net_event_loop(NULL, NULL);
#endif

    /* override some config parameters */
    if (ram_size > 0) {
        p->ram_size = (uint64_t)ram_size << 20;
    }
    if (accel_enable != -1)
        p->accel_enable = accel_enable;
    if (cmdline) {
        vm_add_cmdline(p, cmdline);
    }

    /* open the files & devices */
    for(i = 0; i < p->drive_count; i++) {
        BlockDevice *drive;
        char *fname;
        fname = get_file_path(p->cfg_filename, p->tab_drive[i].filename);
#ifdef CONFIG_FS_NET
        if (is_url(fname)) {
            net_completed = FALSE;
            drive = block_device_init_http(fname, 128 * 1024,
                                           net_start_cb, NULL);
            /* wait until the drive is initialized */
            fs_net_event_loop(net_poll_cb, NULL);
        } else
#endif
        {
            drive = block_device_init(fname, drive_mode);
        }
        free(fname);
        p->tab_drive[i].block_dev = drive;
    }

    // fs "wasi0" is root directory (preopend paths are only accessible)
    p->fs_count = 0;
    fs = fs_disk_init("");
    if (!fs) {
      fprintf(stderr, "/: must be a directory\n");
      exit(1);
    }
    p->tab_fs[p->fs_count].fs_dev = fs;
    if (asprintf(&(p->tab_fs[p->fs_count].tag), "wasi%d", p->fs_count) < 0) {
      exit(1);
    }
    p->fs_count++;

    // fs "wasi1" is "pack" directory
    info = malloc(sizeof(FSVirtFile));
    info->contents = mallocz(1024);
    info->len = 0;
    info->lim = 1024;
    fs = fs_disk_init_with_info("pack", "info", info);
    if (!fs) {
      fprintf(stderr, "cannot prepare pack fs %s\n", strerror(errno));
      exit(1);
    }
    p->tab_fs[p->fs_count].fs_dev = fs;
    if (asprintf(&(p->tab_fs[p->fs_count].tag), "wasi%d", p->fs_count) < 0) {
      exit(1);
    }
    p->fs_count++;

    // networking
    for(i = 0; i < p->eth_count; i++) {
#ifdef CONFIG_SLIRP
        if (!strcmp(p->tab_eth[i].driver, "user")) {
            p->tab_eth[i].net = slirp_open();
            if (!p->tab_eth[i].net)
                exit(1);
        } else
#endif
#ifndef _WIN32
        if (!strcmp(p->tab_eth[i].driver, "tap")) {
// TODO
//            p->tab_eth[i].net = tun_open(p->tab_eth[i].ifname);
//            if (!p->tab_eth[i].net)
//                exit(1);
        } else
#endif
        {
            fprintf(stderr, "Unsupported network driver '%s'\n",
                    p->tab_eth[i].driver);
            exit(1);
        }
    }

    if ((p->eth_count == 0) && (net != NULL)) {
      if (!strncmp(net, "socket", 6)) {
           p->tab_eth[0].net = socket_net_init();
           p->eth_count++;
           if (!p->tab_eth[0].net)
               exit(1);
      }
    }
    
#ifdef CONFIG_SDL
    if (p->display_device) {
        sdl_init(p->width, p->height);
    } else
#endif
    {
#ifdef _WIN32
        fprintf(stderr, "Console not supported yet\n");
        exit(1);
#else
        p->console = console_init(allow_ctrlc);
#endif
    }
    p->rtc_real_time = TRUE;

    s = virt_machine_init(p);
    if (!s)
        exit(1);

    if (s->net) {
        s->net->device_set_carrier(s->net, TRUE);
    }

    for(;;) {
        virt_machine_run(s, FALSE);
        if (init_start)
          break;
    }

    for(i = 0; i < p->drive_count; i++) {
      BlockDeviceFile *bf = p->tab_drive[i].block_dev->opaque;
      fclose(bf->f);
    }
    initialized = TRUE;
}

void init_func()
{
    init_func_args("socket");
}

#ifdef WASI
WIZER_INIT(init_func);
#endif

int write_entrypoint(FSVirtFile *f, int pos1, const char *entrypoint)
{
  int p, pos = pos1;

  p = write_info(f, pos, 3, "e: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  for (int j = 0; j < strlen(entrypoint); j++) {
    if (entrypoint[j] == '\n') {
      p = write_info(f, pos, 2, "\\\n");
      if (p != 2) {
        return -1;
      }
      pos += p;
      continue;
    }
    if (putchar_info(f, pos++, entrypoint[j]) != 1) {
      return -1;
    }
  }
  if (putchar_info(f, pos++, '\n') != 1) {
    return -1;
  }
  return pos - pos1;
}

int write_args(FSVirtFile *f, int argc, char **argv, int optind, int pos1)
{
  int p, pos = pos1;

  p = write_info(f, pos, 3, "c: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  int written = FALSE;
  for (int i = optind; i < argc; i++) {
    for (int j = 0; j < strlen(argv[i]); j++) {
      if ((argv[i][j] == ' ') || (argv[i][j] == '\n')) {
        if (putchar_info(f, pos++, '\\') != 1) {
          return -1;
        }
      }
      if (putchar_info(f, pos++, argv[i][j]) != 1) {
        return -1;
      }
    }
    if (putchar_info(f, pos++, ' ') != 1) {
      return -1;
    }
    written = TRUE;
  }
  if (written) {
    // remove the last space and use newline instead
    if (putchar_info(f, pos-1, '\n') != 1) {
      return -1;
    }
  }
  return pos - pos1;
}

int write_env(FSVirtFile *f, int pos1, const char *env)
{
  int p, pos = pos1;

  p = write_info(f, pos, 5, "env: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  for (int j = 0; j < strlen(env); j++) {
    if (env[j] == '\n') {
      p = write_info(f, pos, 2, "\\\n");
      if (p != 2) {
        return -1;
      }
      pos += p;
      continue;
    }
    if (putchar_info(f, pos++, env[j]) != 1) {
      return -1;
    }
  }
  if (putchar_info(f, pos++, '\n') != 1) {
    return -1;
  }
  return pos - pos1;
}

int write_net(FSVirtFile *f, int pos1, const char *mac)
{
  int p, pos = pos1;

  p = write_info(f, pos, 3, "n: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  for (int j = 0; j < strlen(mac); j++) {
    if (putchar_info(f, pos++, mac[j]) != 1) {
      return -1;
    }
  }
  if (putchar_info(f, pos++, '\n') != 1) {
    return -1;
  }
  return pos - pos1;
}

int write_bundle(FSVirtFile *f, int pos1, const char *bundle)
{
  int p, pos = pos1;

  p = write_info(f, pos, 3, "b: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  for (int j = 0; j < strlen(bundle); j++) {
    if (putchar_info(f, pos++, bundle[j]) != 1) {
      return -1;
    }
  }
  if (putchar_info(f, pos++, '\n') != 1) {
    return -1;
  }
  return pos - pos1;
}

int write_time(FSVirtFile *f, int pos1, const char *timestr)
{
  int p, pos = pos1;

  p = write_info(f, pos, 3, "t: ");
  if (p < 0) {
    return -1;
  }
  pos += p;
  for (int j = 0; j < strlen(timestr); j++) {
    if (putchar_info(f, pos++, timestr[j]) != 1) {
      return -1;
    }
  }
  if (putchar_info(f, pos++, '\n') != 1) {
    return -1;
  }
  return pos - pos1;
}

int main(int argc, char **argv)
{
#ifdef WASI
    if (init_wasi() != 0) {
      fprintf(stderr, "failed to initialize for wasi");
      exit(1);
    }
#endif

    /* const char *cmdline, *build_preload_file; */
    char *entrypoint = NULL, *net = NULL, *mac = NULL, *bundle = NULL;
    int pos, c, option_index, i, enable_stdin = TRUE;
    for(;;) {
        c = getopt_long_only(argc, argv, "+h", options, &option_index);
        if (c == -1)
            break;
        switch(c) {
        case 0:
            switch(option_index) {
            case 0: /* help */
                help();
                break;
            case 1: /* no-stdin */
                enable_stdin = FALSE;
                break;
            case 2: /* entrypoint */
                entrypoint = optarg;
                break;
            case 3: /* net */
                net = optarg;
                break;
            case 4: /* mac */
                mac = optarg;
                break;
            case 5: /* external-bundle */
                bundle = optarg;
                break;
            default:
                fprintf(stderr, "unknown option index: %d\n", option_index);
                exit(1);
            }
            break;
        case 'h':
            help();
            break;
        default:
            exit(1);
        }
    }

    if (!initialized) {
      init_func_args(net);
    } else {
      virt_machine_resume(s);
    }

    pos = info->len;
    if (entrypoint) {
      int p = write_entrypoint(info, pos, entrypoint);
      if (p < 0) {
        printf("failed to prepare entrypoint info\n");
        exit(1);
      }
      pos += p;
    }
    
    if (optind < argc) {
      int p = write_args(info, argc, argv, optind, pos);
      if (p < 0) {
        printf("failed to prepare entrypoint info\n");
        exit(1);
      }
      pos += p;
    }

#ifdef WASI
    // TODO: support emscripten; it seems some default variables are passed, which shouldn't be inherited by the container.
    // https://github.com/emscripten-core/emscripten/blob/0566a76b500bd2bbd535e108f657fce1db7f6f75/src/library_wasi.js#L62
    for (char **env = environ; *env; ++env) {
      int p = write_env(info, pos, *env);
      if (p < 0) {
        printf("failed to prepare env info\n");
        exit(1);
      }
      pos += p;
    }
#endif

    if (net) {
      if (!strncmp(net, "socket", 6)) {
        EthernetDevice *netd;
        for(i = 0; i < p->eth_count; i++) {
          netd = p->tab_eth[i].net;
          if (netd != NULL) {
            ((SocketState *)netd->opaque)->enabled = TRUE;
            ((SocketState *)netd->opaque)->raw_flag = net;
          }
        }
      }
      int p = write_net(info, pos, mac);
      if (p < 0) {
        printf("failed to prepare net info\n");
        exit(1);
      }
      pos += p;
    }

    if (bundle != NULL) {
      int p = write_bundle(info, pos, bundle);
      if (p < 0) {
        printf("failed to prepare net info\n");
        exit(1);
      }
      pos += p;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (unsigned)time(NULL));
    int ps = write_time(info, pos, buf);
    if (ps < 0) {
      printf("failed to prepare time info\n");
      exit(1);
    }
    pos += ps;

    info->len = pos;
#ifdef WASI
    info->len += write_preopen_info(info, pos);
#endif

    // reopen drive files as we can't use stale file descriptors registered during initialize.
    for(i = 0; i < p->drive_count; i++) {
        FILE *f;
        BlockDeviceFile *bf = p->tab_drive[i].block_dev->opaque;
        char *fname;
        fname = get_file_path(p->cfg_filename, p->tab_drive[i].filename);
        f = fopen(fname, "rb");
        if (!f) {
          fprintf(stderr, "failed to open %s: %s\n", fname, strerror(errno));
          exit(1);
        }
        bf->f = f;
        p->tab_drive[i].block_dev->opaque = bf;
    }
    virt_machine_free_config(p);

    for(;;) {
      virt_machine_run(s, enable_stdin);
    }

    virt_machine_end(s);
    return 0;
}
