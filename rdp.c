#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "liblist/list.h"

#include "rdp.h"

// Queue size is the capacity of the ring buffer, in elements.
// Set it to 16 * 1024 cause of the number of selective ack bits is limited to
// the max UDP payload 1390(bytes) * 8(bits) = 11120(bits).
#define RDP_QUEUE_SIZE_MAX (16 * 1024)

// Shouldn't exceed the ring queue capacity.
#define RDP_BUFFER_SIZE_MAX (16 * 1024 * 1024)

// Default buffer size, in bytes.
#define RDP_SEND_BUFFER_SIZE_MAX RDP_BUFFER_SIZE_MAX
#define RDP_RECV_BUFFER_SIZE_MAX RDP_BUFFER_SIZE_MAX

// Window size.
#define RDP_WINDOW_SIZE_MAX RDP_BUFFER_SIZE_MAX
#define RDP_WINDOW_SIZE_DEFAULT (RDP_BUFFER_SIZE_MAX / 4)

// See resizeWindow() implemention.
#define RDP_WINDOW_SHRINK_FACTOR 2
#define RDP_WINDOW_EXPAND_FACTOR 2

// Max rdpConns per rdpSocket.
#define RDP_MAX_CONNS_PER_RDPSOCKET 1024

// In milliseconds.
#define RDP_RETRANSMIT_TIMEOUT_MIN 200
#define RDP_RETRANSMIT_TIMEOUT_MAX 1000
#define RDP_RETRANSMIT_TIMEOUT_DEFAULT 500
// #define RDP_RETRANSMIT_TIMEOUT_STEP 50

// Keep alive probes interval.
#define RDP_KEEPALIVE_INTERVAL 29000

// rdpConn can wait up to seconds in these states.
#define RDP_WAIT_SYN_RECV 10000
#define RDP_WAIT_FIN_SENT 10000

// Limits of vec number.
#define RDP_MAX_VEC 1024

#define RDP_ACK_NR_RECV_BEHIND_ALLOWED 10

#define SIXTEEN_MASK 0xFFFF
#define RDP_SEQ_NR_MASK SIXTEEN_MASK
#define RDP_ACK_NR_MASK SIXTEEN_MASK

#define ETHERNET_MTU 1500
#define IPV4_HEADER_SIZE 20
#define IPV6_HEADER_SIZE 40
#define UDP_HEADER_SIZE 8
#define GRE_HEADER_SIZE 24
#define PPPOE_HEADER_SIZE 8
#define MPPE_HEADER_SIZE 2
#define FUDGE_HEADER_SIZE 36
#define UDP_IPV4_MTU                                                           \
  (ETHERNET_MTU - IPV4_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE -       \
   PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)
#define UDP_IPV6_MTU                                                           \
  (ETHERNET_MTU - IPV6_HEADER_SIZE - UDP_HEADER_SIZE - GRE_HEADER_SIZE -       \
   PPPOE_HEADER_SIZE - MPPE_HEADER_SIZE - FUDGE_HEADER_SIZE)

// Log levels.
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
// Only used by config options, rdpSocket.verbosity specifically.
#define LL_SILIENT 9
// Modifier to log without timestamp.
#define LL_RAW (1 << 10)
// Default maximum length of log messages.
#define LOG_MAX_LEN 1024
// CS_DESTROY can only be set after rdpConnClose() was invoked, or on state
// CS_SYN_RECV.

enum connState {
  CS_UNINITIALIZED = 0,
  CS_SYN_SENT,
  CS_SYN_RECV,
  CS_CONNECTED,
  CS_CONNECTED_FULL,
  CS_FIN_SENT,
  CS_DESTROY
};

#ifdef RDP_DEBUG
static const char *connStateNames[] = {
    "CS_UNINTIALIZED",   "CS_SYN_SENT", "CS_SYN_RECV", "CS_CONNECTED",
    "CS_CONNECTED_FULL", "CS_FIN_SENT", "CS_DESTROY"};
#endif

// Packet types. See also: http://bittorrent.org/beps/bep_0029.html
#define ST_DATA 0
#define ST_FIN 1
#define ST_STATE 2
#define ST_RESET 3
#define ST_SYN 4

#ifdef RDP_DEBUG
static const char *packetStateNames[] = {"ST_DATA", "ST_FIN", "ST_STATE",
                                         "ST_RESET", "ST_SYN"};
#endif

struct __attribute__((packed)) packet {
  uint8_t versionAndType; // First 4 bits specify version, last 4 bits specify
                          // packet type.
  uint8_t reserve;
  uint16_t connId;
  uint32_t window;
  uint16_t seqnr;
  uint16_t acknr;
};

struct __attribute__((packed)) packetWithSAck {
  struct packet p;
  uint8_t next;
  uint8_t len;
  uint8_t mask[1];
};

struct packetWrap {
  size_t payload;    // Payload size does't include packet header size.
  uint64_t sentTime; // In microseconds.
  uint32_t transmissions : 31;
  uint32_t needResend : 1;
  unsigned char data[1]; // Packet bytes.
};

struct rdpSocket {
  void *userData;          // User data variable.
  list *conns;             // Record rdpConns.
  uint64_t mstime;         // Updated before used, in milliseconds.
  uint64_t lastCheck;      // Updated after every invoke on rdpConnCheck(), in
                           // milliseconds.
  uint32_t sendBufferSize; // In bytes.
  uint32_t recvBufferSize; // In bytes.
  int nextCheckTimeout;
  int fd;
  int8_t verbosity; // Log level.
};

// Ring buffer.
struct rbuffer {
  // Elements index mask.
  size_t mask;
  // The number of elements equals the mask value plus 1.
  void **elements;
};

struct rdpConn {
  struct rbuffer inbuf;
  struct rbuffer outbuf;
  rdpSocket *rdpSocket;
  void *userData; // User data variable.
  uint64_t lastReceivedPacket;
  uint64_t lastSentPacket;
  uint32_t rtt;
  uint32_t rttVar;
  uint32_t nextRetransmitTimeout;
  uint32_t retransmitTimeout;
  uint64_t retransmitTicker;
  enum connState state;
  uint32_t flightWindow;      // In bytes.
  uint32_t flightWindowLimit; // In bytes.
  uint32_t recvWindowPeer; // This is the window size we received from packets
                           // the other end sent.
  uint32_t recvWindowSelf; // This is our receive window.
  int32_t oldestResent;
  uint16_t idSeed;
  uint16_t recvId;
  uint16_t sendId;
  uint16_t queue;
  uint16_t outOfOrderCnt;
  uint16_t seqnr;
  uint16_t acknr;
  uint16_t eofseqnr;
  uint8_t receivedFinCompleted : 1;
  uint8_t receivedFin : 1;
  uint8_t needSendAck : 1;
  socklen_t addrlen;
  struct sockaddr_storage addr; // The address bound to this connection.

#ifdef RDP_DEBUG
  char errInfo[LOG_MAX_LEN];
  uint32_t outOfOrderSum;
  uint32_t duplicateSum;
  uint32_t outOfDateSum;
#endif
};

static inline size_t max(size_t a, size_t b) {
  if (a < b)
    return b;
  return a;
}

static inline size_t min(size_t a, size_t b) {
  if (a < b)
    return a;
  return b;
}

static inline uint8_t packetGetVersion(const struct packet *p) {
  return p->versionAndType & 0x0f;
}

static inline uint8_t packetGetType(const struct packet *p) {
  return p->versionAndType >> 4;
}

static inline void packetSetVersion(struct packet *p, uint8_t v) {
  p->versionAndType = (p->versionAndType & 0xf0) | (v & 0x0f);
}

static inline void packetSetType(struct packet *p, uint8_t t) {
  p->versionAndType = (p->versionAndType & 0x0f) | (t << 4);
}

// buf shall already be allocated as a two fields struct.
static inline void rbufferInit(struct rbuffer *buf) {
  buf->mask = 63;
  buf->elements = (void **)calloc(64, sizeof(void *));
}

static inline void *rbufferGet(struct rbuffer *buf, size_t i) {
  return buf->elements ? buf->elements[i & buf->mask] : NULL;
}

#ifdef RDP_DEBUG
// Get the number of filled elements.
static inline size_t rbufferGetFilled(struct rbuffer *buf) {
  size_t cnt = 0;
  void *item;
  for (size_t i = 0; i <= buf->mask; i++) {
    item = rbufferGet(buf, i);
    if (item != NULL) {
      cnt++;
    }
  }

  return cnt;
}
#endif

// Free the element items and the elements field, not buf itself.
static inline void rbufferFree(struct rbuffer *buf) {
  for (size_t i = 0; i <= buf->mask; i++) {
    free(rbufferGet(buf, i));
  }

  free(buf->elements);
}

static inline void rbufferPut(struct rbuffer *buf, size_t i, void *data) {
  buf->elements[i & buf->mask] = data;
}

// Expand the capacity of buf, shouldn't be invoked directly.
// Use rbufferEnsureSize() instead.
static inline void rbufferGrow(struct rbuffer *buf, size_t item, size_t index) {
  // Current size.
  size_t size = buf->mask + 1;
  // Calculate new size.
  do
    size *= 2;
  while (index >= size);

  void **newElements = (void **)calloc(size, sizeof(void *));

  // Size is new mask now.
  size--;

  for (size_t i = 0; i <= buf->mask; i++) {
    newElements[(item - index + i) & size] = rbufferGet(buf, item - index + i);
  }

  free(buf->elements);
  buf->elements = newElements;
  buf->mask = size;
}

// Ensure the capacity is enough.
static inline void rbufferEnsureSize(struct rbuffer *buf, size_t item,
                                     size_t index) {
  if (index > buf->mask)
    rbufferGrow(buf, item, index);
}

// Return the UNIX time in millisecond.
static inline uint64_t mstime(void) {
  struct timeval tv;
  uint64_t mst;

  gettimeofday(&tv, NULL);
  mst = ((uint64_t)tv.tv_sec) * 1000;
  mst += tv.tv_usec / 1000;
  return mst;
}

// This MTU limits the size of rdp header and payload, in bytes.
static inline size_t getUdpMtu() { return UDP_IPV4_MTU; }

static inline size_t getPacketHeaderSize() { return sizeof(struct packet); }

static inline size_t getPacketWithSAckHeaderSize() {
  return sizeof(struct packetWithSAck);
}

static inline size_t getPacketWrapSize() { return sizeof(struct packetWrap); }

static inline size_t getMaxPacketPayloadSize() {
  return getUdpMtu() - getPacketHeaderSize();
}

// Return a valid retransmit timeout.
// Return default timeout if t equals zero.
static inline uint32_t limitedRetransmitTimeout(uint32_t t) {
  if (t > 0) {
    return min(RDP_RETRANSMIT_TIMEOUT_MAX, max(RDP_RETRANSMIT_TIMEOUT_MIN, t));
  }
  return RDP_RETRANSMIT_TIMEOUT_DEFAULT;
}

// Return a valid window size.
// Return default window size if t equals zero.
static inline uint32_t limitedWindow(uint32_t t) {
  if (t > 0) {
    return min(RDP_WINDOW_SIZE_MAX, max(getMaxPacketPayloadSize(), t));
  }
  return RDP_WINDOW_SIZE_DEFAULT;
}

#ifdef RDP_DEBUG
static int isLeapYear(time_t year) {
  if (year % 4)
    return 0; /* A year not divisible by 4 is not leap. */
  else if (year % 100)
    return 1; /* If div by 4 and not 100 is surely leap. */
  else if (year % 400)
    return 0; /* If div by 100 *and* not by 400 is not leap. */
  else
    return 1; /* If div by 100 and 400 is leap. */
}
static void nolocksLocaltime(struct tm *tmp, time_t t, time_t tz, int dst) {
  const time_t secs_min = 60;
  const time_t secs_hour = 3600;
  const time_t secs_day = 3600 * 24;

  t -= tz;                       /* Adjust for timezone. */
  t += 3600 * dst;               /* Adjust for daylight time. */
  time_t days = t / secs_day;    /* Days passed since epoch. */
  time_t seconds = t % secs_day; /* Remaining seconds. */

  tmp->tm_isdst = dst;
  tmp->tm_hour = seconds / secs_hour;
  tmp->tm_min = (seconds % secs_hour) / secs_min;
  tmp->tm_sec = (seconds % secs_hour) % secs_min;

  /* 1/1/1970 was a Thursday, that is, day 4 from the POV of the tm structure
   * where sunday = 0, so to calculate the day of the week we have to add 4
   * and take the modulo by 7. */
  tmp->tm_wday = (days + 4) % 7;

  /* Calculate the current year. */
  tmp->tm_year = 1970;
  while (1) {
    /* Leap years have one day more. */
    time_t days_this_year = 365 + isLeapYear(tmp->tm_year);
    if (days_this_year > days)
      break;
    days -= days_this_year;
    tmp->tm_year++;
  }
  tmp->tm_yday = days; /* Number of day of the current year. */

  /* We need to calculate in which month and day of the month we are. To do
   * so we need to skip days according to how many days there are in each
   * month, and adjust for the leap year that has one more day in February. */
  int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  mdays[1] += isLeapYear(tmp->tm_year);

  tmp->tm_mon = 0;
  while (days >= mdays[tmp->tm_mon]) {
    days -= mdays[tmp->tm_mon];
    tmp->tm_mon++;
  }

  tmp->tm_mday = days + 1; /* Add 1 since our 'days' is zero-based. */
  tmp->tm_year -= 1900;    /* Surprisingly tm_year is year-1900. */
}

static void tlogRaw(rdpSocket *rdpSocket, int level, const char *msg) {
  const char *c = ".-*#";
  char buf[64];
  char outputMsg[LOG_MAX_LEN + 64];
  int n;
  int fd = STDOUT_FILENO;

  int rawmode = (level & LL_RAW);

  if ((level & 0xff) < rdpSocket->verbosity)
    return;

  if (rawmode) {
    n = snprintf(outputMsg, sizeof(outputMsg), "%s", msg);
    write(fd, outputMsg, n);
  } else {
    int off;
    struct timeval tv;
    time_t t;

    gettimeofday(&tv, NULL);
    struct tm tm;
    nolocksLocaltime(&tm, tv.tv_sec, 0, 0);
    off = strftime(buf, sizeof(buf), "%d %b %Y %H:%M:%S.", &tm);
    snprintf(buf + off, sizeof(buf) - off, "%03d", (int)tv.tv_usec / 1000);
    n = snprintf(outputMsg, sizeof(outputMsg), "[librdp] %s %c %s\n", buf,
                 c[level], msg);
    write(fd, outputMsg, n);
  }
}

//  Printf-alike style log utility.
static void tlog(rdpSocket *rdpSocket, int level, const char *fmt, ...) {
  va_list ap;
  char msg[LOG_MAX_LEN];

  if ((level & 0xff) < rdpSocket->verbosity)
    return;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  tlogRaw(rdpSocket, level, msg);
}
#endif

// List node deletion callback.
static void listFreeRdpConn(void *value) {
  rdpConn *c = (rdpConn *)value;

  rbufferFree(&c->inbuf);
  rbufferFree(&c->outbuf);

  free(value);
}

// List node compare callback.
// Using three fields of rdpConn: recvId, addr, addrlen to determine
// equlity.
// Return 1 if equal, otherwise 0.
static int listEqualRdpConn(void *value, void *comparedValue) {
  rdpConn *currentV, *comparedV;

  currentV = (rdpConn *)value;
  comparedV = (rdpConn *)comparedValue;

  if (currentV->recvId != comparedV->recvId)
    return 0;

  if (currentV->addrlen != comparedV->addrlen)
    return 0;

  if (memcmp(&currentV->addr, &comparedV->addr, currentV->addrlen) != 0)
    return 0;

  return 1;
}

// Just Invoke listNodeDestroy() is enough, actions free internal struct is
// registerd in list.
static void rdpConnDestroy(rdpConn *c) {
  // Not registered in rdpSocket->conns.
  if (!(c->addrlen || c->idSeed)) {
    listFreeRdpConn((void *)c);
    return;
  }

  rdpSocket *s;
  listNode *node;

  s = c->rdpSocket;
  node = listNodeFind(s->conns, c);

  assert(node);

  listNodeDestroy(s->conns, node);
}

// Create a UDP socket, bound to address "node:service".
static int inetSocket(const char *node, const char *service,
                      socklen_t *addrlen) {
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int sfd, optval, s;

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  hints.ai_socktype = SOCK_DGRAM;
  // AF_UNSPEC allows IPv4 or IPv6
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_PASSIVE;

  s = getaddrinfo(node, service, &hints, &result);
  if (s != 0) {

#ifdef RDP_DEBUG
    write(STDOUT_FILENO, gai_strerror(s), strlen(gai_strerror(s)));
#endif

    return -1;
  }

  optval = 1;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sfd =
        socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
    if (sfd == -1)
      continue;

    if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0)
      // Success.
      break;

    close(sfd);
  }

  if (rp != NULL && addrlen != NULL)
    *addrlen = rp->ai_addrlen;

  freeaddrinfo(result);

  return (rp == NULL) ? -1 : sfd;
}

// Create a rdpSocket.
rdpSocket *rdpSocketCreate(int version, const char *node, const char *service) {
  if (version != 1)
    return NULL;

  rdpSocket *s;
  s = malloc(sizeof(*s));
  assert(s);
  if (s == NULL) {
    perror("malloc");
    return NULL;
  }

  s->userData = NULL;
  s->fd = inetSocket(node, service, NULL);
  if (s->fd == -1) {
    perror("inetSocket");
    exit(EXIT_FAILURE);
  }

  s->conns = listCreate();
  assert(s->conns);
  listMethodSetFree(s->conns, &listFreeRdpConn);
  listMethodSetEqual(s->conns, &listEqualRdpConn);

  s->mstime = mstime();
  s->lastCheck = s->mstime;
  s->nextCheckTimeout = RDP_SOCKET_CHECK_TIMEOUT_DEFAULT;
  s->sendBufferSize = RDP_SEND_BUFFER_SIZE_MAX;
  s->recvBufferSize = RDP_RECV_BUFFER_SIZE_MAX;
  s->verbosity = LL_DEBUG;

  srand((unsigned int)s->mstime);

  return s;
}

// Close fd, free conns list.
int rdpSocketDestroy(rdpSocket *rdpSocket) {
  if (!rdpSocket)
    return -1;

  close(rdpSocket->fd);

  listDestroy(rdpSocket->conns);

  free(rdpSocket);

  return 0;
}

// Make a fake rdpConn to compare.
static listNode *findRdpConnInRdpSocket(rdpSocket *s,
                                        const struct sockaddr *addr,
                                        socklen_t addrlen, uint16_t recvId) {
  rdpConn comparedValue;

  memcpy(&comparedValue.addr, addr, addrlen);
  comparedValue.addrlen = addrlen;
  comparedValue.recvId = recvId;

  return listNodeFind(s->conns, &comparedValue);
}

// Initialize rdpConn, attatch itself to rdpSocket->conns.
// Shouldn't be invoked directly.
void rdpConnInit(rdpConn *c, const struct sockaddr *addr, socklen_t addrlen,
                 int generateSeed, uint16_t idSeed, uint16_t recvId,
                 uint16_t sendId) {
  if (generateSeed) {
    do {
      idSeed = rand() & 0xffff;
    } while (findRdpConnInRdpSocket(c->rdpSocket, addr, addrlen, idSeed));

    recvId += idSeed;
    sendId += idSeed;
  }

  memcpy(&c->addr, addr, addrlen);
  c->addrlen = addrlen;
  c->idSeed = idSeed;
  c->recvId = recvId;
  c->sendId = sendId;

  c->lastReceivedPacket = c->rdpSocket->mstime;

  // Attach this socket to context->rdpConns list.
  listNode *node = listNodeAddHead(c->rdpSocket->conns, c);
  if (node == NULL) {
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "listNodeAddHead");
#endif

    exit(EXIT_FAILURE);
  }
}

// Create a rdpConn.
rdpConn *rdpConnCreate(rdpSocket *s) {
  if (!s)
    return NULL;

  rdpConn *c;
  c = malloc(sizeof(*c));
  if (c == NULL) {
    return NULL;
  }
  c->rdpSocket = s;
  c->userData = NULL;
  c->state = CS_UNINITIALIZED;

  memset(&c->addr, 0, sizeof(c->addr));
  c->addrlen = 0;
  c->lastReceivedPacket = 0;
  c->lastSentPacket = 0;
  c->idSeed = 0;
  c->recvId = 0;
  c->sendId = 0;
  c->outOfOrderCnt = 0;
  c->seqnr = rand();
  c->acknr = 0;
  c->eofseqnr = 0;
  c->receivedFinCompleted = 0;
  c->receivedFin = 0;
  c->needSendAck = 0;
  c->queue = 0;
  c->flightWindow = 0;
  c->flightWindowLimit = limitedWindow(0);
  c->recvWindowPeer = limitedWindow(RDP_WINDOW_SIZE_MAX);
  c->recvWindowSelf = limitedWindow(RDP_WINDOW_SIZE_MAX);
  c->rtt = 0;
  c->rttVar = 0;
  c->nextRetransmitTimeout = limitedRetransmitTimeout(0);
  c->retransmitTimeout = 0;
  c->retransmitTicker = 0;
  c->oldestResent = -1;
  rbufferInit(&c->inbuf);
  rbufferInit(&c->outbuf);

#ifdef RDP_DEBUG
  memset(c->errInfo, 0, LOG_MAX_LEN);

  c->outOfOrderSum = 0;
  c->duplicateSum = 0;
  c->outOfDateSum = 0;
#endif

  return c;
}

ssize_t sendToAddr(rdpConn *c, const unsigned char *buf, size_t len) {
  ssize_t n;

  n = sendto(c->rdpSocket->fd, buf, len, 0, (const struct sockaddr *)&c->addr,
             c->addrlen);

  return n;
}

ssize_t sendData(rdpConn *c, unsigned char *buf, size_t len) {
  c->lastSentPacket = c->rdpSocket->mstime;

  return sendToAddr(c, buf, len);
}

ssize_t sendPacketWrap(rdpConn *c, struct packetWrap *pw) {
  assert(pw->transmissions == 0 || pw->needResend);

  c->flightWindow += pw->payload;

  pw->needResend = 0;

  struct packet *p = (struct packet *)pw->data;
  p->acknr = c->acknr;
  pw->sentTime = c->rdpSocket->mstime;
  pw->transmissions++;
  return sendData(c, (void *)pw->data, pw->payload + getPacketHeaderSize());
}

// Initialize rdpConn, send a syn packet to the other end.
int rdpConnect(rdpConn *c, const struct sockaddr *addr, socklen_t addrlen) {
  if (!c)
    return -1;

  if (c->state != CS_UNINITIALIZED) {
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "rdpConnect not expected state: %s",
         connStateNames[c->state]);
#endif

    return -1;
  }

  c->rdpSocket->mstime = mstime();

  rdpConnInit(c, addr, addrlen, 1, 0, 0, 1);
  c->state = CS_SYN_SENT;

  c->retransmitTimeout = c->nextRetransmitTimeout;
  c->retransmitTicker = c->rdpSocket->mstime + c->retransmitTimeout;

  struct packetWrap *pw = (struct packetWrap *)malloc(getPacketWrapSize() - 1 +
                                                      getPacketHeaderSize());
  assert(pw);
  pw->transmissions = 0;
  pw->payload = 0;

  struct packet *p = (struct packet *)pw->data;
  memset(p, 0, getPacketHeaderSize());
  packetSetVersion(p, 1);
  packetSetType(p, ST_SYN);
  p->reserve = 0;
  // ST_SYN is a special packet, it's connId is recvId, all subsequent packets'
  // connId is sendId.
  p->connId = c->recvId;
  p->window = c->recvWindowSelf;
  p->seqnr = c->seqnr;

  rbufferEnsureSize(&c->outbuf, c->seqnr, c->queue);
  rbufferPut(&c->outbuf, c->seqnr, pw);

  c->seqnr++;
  c->queue++;
  sendPacketWrap(c, pw);

  return 0;
}

static inline int sixteenAfter(uint16_t a, uint16_t b) {
  return ((int16_t)((int16_t)a - (int16_t)b) < 0);
}

// demand -1 means max packet payload size for parameter more.
// Return 1 if full, otherwise 0.
//
// Not full means the flight window has spaces for a maximum packet.
static int rdpConnFlightWindowFull(rdpConn *c) {
  if (c->flightWindow + (uint32_t)getMaxPacketPayloadSize() >
      (uint32_t)min(c->flightWindowLimit, c->recvWindowPeer)) {
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG,
         "connection is full, flightWindow: %d, limit: %d", c->flightWindow,
         c->flightWindowLimit);
#endif
    return 1;
  }

  return 0;
}

// Ack the packet registered in rdpConn->outbuf.
static int ackPacket(rdpConn *c, uint16_t i) {
  struct packetWrap *pw = (struct packetWrap *)rbufferGet(&c->outbuf, i);

  if (!pw)
    return -1;

  if (pw->transmissions == 0) {
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "ack packet not sent.");
#endif
    return -1;
  }

  rbufferPut(&c->outbuf, i, NULL);

  if (pw->transmissions == 1) {
    uint32_t packetRtt = (uint32_t)(c->rdpSocket->mstime - pw->sentTime);
    if (c->rtt == 0) {
      c->rtt = packetRtt;
      c->rttVar = packetRtt / 2;
    } else {
      c->rttVar += (abs((int)c->rtt - (int)packetRtt) - (int)c->rttVar) / 4;
      c->rtt += ((int)packetRtt - (int)c->rtt) / 8;
    }

    c->nextRetransmitTimeout = limitedRetransmitTimeout(c->rtt + c->rttVar * 4);
  }

  // Timeout packets are not included in flightWindow.
  if (!pw->needResend) {
    assert(c->flightWindow >= pw->payload);
    c->flightWindow -= pw->payload;
  }

  free(pw);

  return 0;
}

// Send an ack packet.
static ssize_t sendAck(rdpConn *c) {
  size_t packetLen;
  struct packet *p;

  if (c->outOfOrderCnt != 0 && c->state != CS_SYN_RECV &&
      !c->receivedFinCompleted) {

    // sackSize must be a multiple of 4, and at least 4.
    int sackSize =
        c->outOfOrderCnt / 8 + 1 + 3 - ((c->outOfOrderCnt / 8 + 1 + 3) % 4);

    packetLen = getPacketWithSAckHeaderSize() - 1 + sackSize;
    p = (struct packet *)malloc(packetLen);
    assert(p);
    memset(p, 0, packetLen);
    struct packetWithSAck *ps = (struct packetWithSAck *)p;

    // Out of order state check.
    assert(!rbufferGet(&c->inbuf, c->acknr + 1));

    ps->p.reserve = 1;
    ps->next = 0;
    ps->len = sackSize;

    // buf's size equals buf's mask plus 1.
    // The slot of s->acknr + 1 is always empty.
    size_t len = min(sackSize * 8, c->inbuf.mask);

    for (int group = 0; group < (sackSize / 4) && len > 0; len -= 32, group++) {
      uint32_t m = 0;

      for (size_t i = 0; i < min(32, len); i++) {
        if (rbufferGet(&c->inbuf, c->acknr + i + 2 + group * 32) != NULL) {
          m |= 1 << i;
        }
      }

      ((uint8_t *)ps->mask)[0 + group * 4] = (uint8_t)m;
      ((uint8_t *)ps->mask)[1 + group * 4] = (uint8_t)(m >> 8);
      ((uint8_t *)ps->mask)[2 + group * 4] = (uint8_t)(m >> 16);
      ((uint8_t *)ps->mask)[3 + group * 4] = (uint8_t)(m >> 24);
    }
  } else {
    packetLen = getPacketHeaderSize();
    p = (struct packet *)malloc(packetLen);
    assert(p);
    memset(p, 0, packetLen);

    p->reserve = 0;
  }

  packetSetVersion(p, 1);
  packetSetType(p, ST_STATE);
  p->connId = c->sendId;
  p->acknr = c->acknr;
  p->seqnr = c->seqnr;
  p->window = c->recvWindowSelf;

  ssize_t n = sendData(c, (void *)p, packetLen);

  c->needSendAck = 0;

  free(p);

  return n;
}

// Send ack packets on all rdpConns if needed.
static void rdpContextAck(rdpSocket *s) {
  if (!s)
    return;

  listIterator *iter = listIteratorCreate(s->conns, LIST_START_HEAD);
  listNode *node;
  rdpConn *c;
  while (node = listIteratorNext(iter)) {
    c = (rdpConn *)node->value;

    if (c->needSendAck) {
      sendAck(c);
    }
  }
}

// Return -1 if connection is full.
static int rdpConnFlushPackets(rdpConn *c) {
  for (uint16_t i = c->seqnr - c->queue; i != c->seqnr; i++) {
    struct packetWrap *pw = rbufferGet(&c->outbuf, i);
    if (pw == NULL || (pw->transmissions > 0 && pw->needResend == 0))
      continue;

    if (rdpConnFlightWindowFull(c))
      return -1;

    sendPacketWrap(c, pw);
  }

  return 0;
}

static void buildSendPacket(rdpConn *c, size_t payload, uint type,
                            struct rdpVec *vec, size_t vecCnt) {
  assert(c->queue > 0 || (c->queue == 0 && c->flightWindow == 0));

  size_t maxPacketPayloadSize = getMaxPacketPayloadSize();

  assert(payload <= maxPacketPayloadSize);

  do {
    assert(c->queue < RDP_QUEUE_SIZE_MAX);

    struct packetWrap *pw = NULL;
    if (c->queue > 0) {
      pw = (struct packetWrap *)rbufferGet(&c->outbuf, c->seqnr - 1);
    }

    const size_t packetHeaderSize = getPacketHeaderSize();
    const size_t packetWrapSize = getPacketWrapSize();
    size_t roundPayload = 0;
    int appendQueue;

    if (payload && pw && !pw->transmissions &&
        pw->payload < maxPacketPayloadSize) {
      roundPayload =
          min(payload + pw->payload, maxPacketPayloadSize) - pw->payload;

      pw = (struct packetWrap *)realloc(pw, (packetWrapSize - 1) +
                                                packetHeaderSize + pw->payload +
                                                roundPayload);
      assert(pw);

      rbufferPut(&c->outbuf, c->seqnr - 1, pw);

      appendQueue = 0;
    } else {
      roundPayload = payload;
      pw = (struct packetWrap *)malloc((packetWrapSize - 1) + packetHeaderSize +
                                       roundPayload);
      assert(pw);
      pw->payload = 0;
      pw->transmissions = 0;
      pw->needResend = 0;

      appendQueue = 1;
    }

    if (roundPayload) {
      assert(type == ST_DATA);

      unsigned char *p = pw->data + packetHeaderSize + pw->payload;
      size_t needed = roundPayload;

      for (size_t i = 0; i < vecCnt && needed; i++) {
        if (vec[i].len == 0)
          continue;

        size_t num = min(needed, vec[i].len);
        memcpy(p, vec[i].base, num);

        p += num;

        vec[i].len -= num;
        vec[i].base = (unsigned char *)vec[i].base + num;
        needed -= num;
      }

      assert(needed == 0);
    }
    pw->payload += roundPayload;

    struct packet *p = (struct packet *)pw->data;
    packetSetVersion(p, 1);
    packetSetType(p, type);
    p->reserve = 0;
    p->connId = c->sendId;
    p->window = c->recvWindowSelf;
    p->acknr = c->acknr;

    if (appendQueue) {
      rbufferEnsureSize(&c->outbuf, c->seqnr, c->queue);
      rbufferPut(&c->outbuf, c->seqnr, pw);
      p->seqnr = c->seqnr;
      c->seqnr++;
      c->queue++;
    }

    payload -= roundPayload;
  } while (payload);
}

// CS_CONNECTED -> CS_CONNECTED_FULL can happen in this function only.
ssize_t rdpWriteVec(rdpConn *c, struct rdpVec *vec, size_t vecCnt) {
  if (!c) {
    errno = EINVAL;
    return -1;
  }

  if (!vec) {
    errno = EINVAL;
    return -1;
  }

  if (!vecCnt) {
    errno = EINVAL;
    return -1;
  }

  if (vecCnt > RDP_MAX_VEC) {
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "vecCnt: %d exceeded RDP_MAX_VEC: %d", vecCnt,
         RDP_MAX_VEC);
#endif

    errno = EINVAL;
    return -1;
  }

  switch (c->state) {
  case CS_UNINITIALIZED:
  case CS_SYN_RECV:
  case CS_DESTROY:
  case CS_FIN_SENT:

#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "connection not expceted state: %s",
         connStateNames[c->state]);
#endif

    errno = EINVAL;
    return -1;
  case CS_SYN_SENT:
  case CS_CONNECTED_FULL:

#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "connection EAGAIN, state: %s, id: %d",
         connStateNames[c->state], c->recvId);
#endif

    errno = EAGAIN;
    return -1;
  case CS_CONNECTED:
    break;
  default:
    assert(0);
  }

  size_t total = 0;
  size_t sent = 0;
  for (size_t i = 0; i < vecCnt; i++)
    total += vec[i].len;

  if (rdpConnFlightWindowFull(c)) {
    c->state = CS_CONNECTED_FULL;
    errno = EAGAIN;
    return -1;
  }

  c->rdpSocket->mstime = mstime();

  size_t maxPacketPayloadSize = getMaxPacketPayloadSize();
  size_t validSend = min(total, maxPacketPayloadSize);
  // Reserve a slot for ST_FIN.
  while (c->queue < RDP_QUEUE_SIZE_MAX - 1) {
    total -= validSend;
    sent += validSend;

    buildSendPacket(c, validSend, ST_DATA, vec, vecCnt);

    validSend = min(total, maxPacketPayloadSize);

    if (validSend == 0) {
      // Success, all sent.
      break;
    }
  }

  if (rdpConnFlushPackets(c) == -1) {
    c->state = CS_CONNECTED_FULL;
  }

  if (sent == 0) {
    if (total == 0) {
      return 0;
    } else {
      errno = EAGAIN;
      return -1;
    }
  } else {
    return sent;
  }
}

ssize_t rdpWrite(rdpConn *c, const void *buf, size_t len) {
  struct rdpVec vec = {buf, len};
  return rdpWriteVec(c, &vec, 1);
}

// Change rdpConn state, invoke rdpConnDestroy() in rdpIntervalAction() later.
int rdpConnClose(rdpConn *c) {
  if (!c) {
    errno = EINVAL;
    return -1;
  }

  switch (c->state) {
  case CS_UNINITIALIZED:
  case CS_SYN_RECV:
  case CS_DESTROY:
  case CS_FIN_SENT:
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG, "not expected conn state. errInfo: %s",
         c->errInfo);
#endif
    errno = EINVAL;
    return -1;
  case CS_CONNECTED:
  case CS_CONNECTED_FULL:

    // Passive close.
    if (c->receivedFin) {
      c->state = CS_DESTROY;

#ifdef RDP_DEBUG
      tlog(c->rdpSocket, LL_DEBUG,
           "change state to CS_DESTROY, passive close receivedFinCompleted.");
#endif

      return 0;
    }

    // Send ack before send fin packet if required.
    if (c->needSendAck) {
      sendAck(c);
    }

    // One slot is reserved for ST_FIN, see rdpWriteVec().
    assert(c->queue < RDP_QUEUE_SIZE_MAX);
    buildSendPacket(c, 0, ST_FIN, NULL, 0);
    rdpConnFlushPackets(c);

    c->state = CS_FIN_SENT;

    return 0;
  case CS_SYN_SENT:
    c->state = CS_DESTROY;
#ifdef RDP_DEBUG
    tlog(c->rdpSocket, LL_DEBUG,
         "change state to CS_DESTROY, invoked rdpConnClose() on CS_SYN_SENT.");
#endif
    return 0;
  default:
    assert(0);
  }

  return 0;
}

int selectiveAck(rdpConn *c, uint32_t startSeqnr, const uint8_t *mask,
                 uint8_t len) {
  int offset = len * 8 - 1;

#ifdef RDP_DEBUG_2
  char bitmask[4096] = {0};
  int counter = offset;
  for (int i = 0; i <= offset; ++i) {
    uint8_t b = counter >= 0 && mask[counter >> 3] & (1 << (counter & 7));
    bitmask[i] = b ? '1' : '0';
    --counter;
  }

  tlog(c->rdpSocket, LL_DEBUG | LL_RAW, "sack bits:\n%s\n", bitmask,
       startSeqnr);
#endif

  do {
    uint v = startSeqnr + offset;

    if (((c->seqnr - v - 1) & RDP_ACK_NR_MASK) >= (uint16_t)(c->queue - 1))
      continue;

    uint8_t b = offset >= 0 && mask[offset >> 3] & (1 << (offset & 7));

    struct packetWrap *pw = (struct packetWrap *)rbufferGet(&c->outbuf, v);
    if (!pw) {
      continue;
    }

    if (pw->transmissions == 0) {
      assert(0);
      continue;
    }

    if (b) {
      assert((v & c->outbuf.mask) != ((c->seqnr - c->queue) & c->outbuf.mask));

      ackPacket(c, v);

      continue;
    }

  } while (--offset >= -1);

  return 0;
}

// buf and len are similar to read().
// The corresponding rdpConn is returned by parameter c(connection).
// Result type is returned by parameter events.
//
// CS_CONNECTED_FULL -> CS_CONNECTED can happen in this function only.
ssize_t rdpReadPoll(rdpSocket *s, void *buf, size_t len, rdpConn **conn,
                    int *events) {
  if (!events) {
    return -1;
  }
  // This is the default events returns;
  *events = RDP_CONTINUE;

  if (!conn) {
    *events = RDP_ERROR;
    return -1;
  }
  *conn = NULL;

  if (!s) {
    *events = RDP_ERROR;
    return -1;
  }

  if (!buf) {
    *events = RDP_ERROR;
    return -1;
  }

  if (len <= 0) {
    *events = RDP_ERROR;
    return -1;
  }

  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  ssize_t read;
  ssize_t rawRead;
  uint inbufPrefix;

  listNode *node;
  listIterator *iter = listIteratorCreate(s->conns, LIST_START_HEAD);

  // Drain ordered buffer on each connection.
  while (node = listIteratorNext(iter)) {
    *conn = (rdpConn *)node->value;

    if ((*conn)->state != CS_CONNECTED && (*conn)->state != CS_CONNECTED_FULL) {
      continue;
    }

    // receivedFin and eofseqnr are related fields.
    if (!(*conn)->receivedFinCompleted && (*conn)->receivedFin &&
        (*conn)->eofseqnr == (*conn)->acknr) {
      (*conn)->receivedFinCompleted = 1;

      sendAck(*conn);

      (*conn)->outOfOrderCnt = 0;

      *events = RDP_DATA;

#ifdef RDP_DEBUG
      tlog(s, LL_DEBUG, "receivedFinCompleted");
#endif

      // EOF
      return 0;
    }

    if ((*conn)->outOfOrderCnt == 0)
      continue;

    uint8_t *p = (uint8_t *)rbufferGet(&(*conn)->inbuf, (*conn)->acknr + 1);
    if (p == NULL)
      continue;

    inbufPrefix = *(uint *)p;
    if (inbufPrefix > 0) {
      if (inbufPrefix > len) {
        *events = RDP_ERROR;
#ifdef RDP_DEBUG
        tlog(s, LL_NOTICE, "user supplied len is not enough.");
#endif
        return -1;
      }

      memcpy(buf, p + sizeof(uint), inbufPrefix);

      *events = RDP_DATA;
    }

    rbufferPut(&(*conn)->inbuf, (*conn)->acknr + 1, NULL);
    free(p);
    (*conn)->acknr++;
    (*conn)->needSendAck = 1;

    assert((*conn)->outOfOrderCnt > 0);
    (*conn)->outOfOrderCnt--;

    return inbufPrefix > 0 ? inbufPrefix : -1;
  }
  listIteratorDestroy(iter);
  *conn = NULL;

  // Read from socket only after drained ordered buffer in queue.
  rawRead = recvfrom(s->fd, buf, len, 0, (struct sockaddr *)&addr, &addrlen);
  if (rawRead == -1) {
    if (errno == EAGAIN) {
      *events = RDP_AGAIN;

      rdpContextAck(s);
    } else {
      *events = RDP_ERROR;
    }

    return -1;
  }

  if (rawRead < getPacketHeaderSize()) {
    return -1;
  }

  const struct packet *p = (struct packet *)buf;
  const uint8_t version = packetGetVersion(p);
  if (version != 1) {
    return -1;
  }

  const uint16_t connId = p->connId;
  const uint8_t type = packetGetType(p);
  const uint16_t pseqnr = p->seqnr;
  const uint16_t packnr = p->acknr;

  s->mstime = mstime();

  if (type == ST_SYN) {
    node = findRdpConnInRdpSocket(s, (const struct sockaddr *)&addr, addrlen,
                                  connId + 1);
    if (node) {
      *conn = (rdpConn *)node->value;

      if ((*conn)->state != CS_SYN_RECV) {
        return -1;
      }
    } else {
      if (listLengthGet(s->conns) > RDP_MAX_CONNS_PER_RDPSOCKET) {
        *events = RDP_ERROR;
        return -1;
      }

      *conn = rdpConnCreate(s);
      rdpConnInit(*conn, (const struct sockaddr *)&addr, addrlen, 0, connId,
                  connId + 1, connId);
      (*conn)->state = CS_SYN_RECV;

      (*conn)->acknr = pseqnr;
    }

    (*conn)->lastReceivedPacket = (*conn)->rdpSocket->mstime;
    (*conn)->retransmitTimeout = (*conn)->nextRetransmitTimeout;
    (*conn)->retransmitTicker =
        (*conn)->rdpSocket->mstime + (*conn)->retransmitTimeout;

    sendAck(*conn);

    return -1;
  } else if (type == ST_STATE || type == ST_DATA || type == ST_FIN) {
    node = findRdpConnInRdpSocket(s, (const struct sockaddr *)&addr, addrlen,
                                  connId);
    if (!node)
      return -1;

    *conn = (rdpConn *)node->value;
    rdpConn *c = *conn;

    if (c->state == CS_DESTROY) {
      return -1;
    }

#ifdef RDP_DEBUG
    if (c->queue == 0)
      assert(c->flightWindow == 0);
    assert(c->queue == 0 || rbufferGet(&c->outbuf, c->seqnr - c->queue));
#endif

    // Ignore packets with invalid acknr.
    if ((sixteenAfter(c->seqnr - 1, packnr) ||
         sixteenAfter(packnr, c->seqnr - 1 - c->queue -
                                  RDP_ACK_NR_RECV_BEHIND_ALLOWED))) {
#ifdef RDP_DEBUG_2
      tlog(c->rdpSocket, LL_DEBUG, "wrong acknr: %d, packet type: %d",
           c->seqnr - 1 - c->queue - packnr, type);
#endif
      return -1;
    }

    const uint8_t *sackMask = NULL;
    uint8_t extension = p->reserve;
    const uint8_t *payloadStart = (const uint8_t *)p + getPacketHeaderSize();
    const uint8_t *payloadEnd = buf + rawRead;
    ssize_t payload = payloadEnd - payloadStart;

    if (extension != 0) {
      do {
        payloadStart += 2;

        assert((payloadEnd - payloadStart) >= payloadStart[-1]);

        switch (extension) {
        case 1:
          sackMask = payloadStart;
          break;
        default:
#ifdef RDP_DEGUG
          tlog(c->rdpSocket, LL_DEBUG, "unknown reserved bits.");
#endif
          break;
        }
        extension = payloadStart[-2];
        payloadStart += payloadStart[-1];
      } while (extension);
    }

    if (c->state == CS_SYN_SENT) {
      c->acknr = (pseqnr - 1) & RDP_SEQ_NR_MASK;
    }

    const uint seqCnt = (pseqnr - c->acknr - 1) & RDP_SEQ_NR_MASK;

    if (seqCnt >= RDP_QUEUE_SIZE_MAX) {
      if (seqCnt >= (RDP_SEQ_NR_MASK + 1) - RDP_QUEUE_SIZE_MAX &&
          type != ST_STATE) {
        // Some ack packets we send are lost.
        c->needSendAck = 1;

#ifdef RDP_DEBUG
        c->outOfDateSum++;
#endif
      } else {
        // Not expected seqnr.
#ifdef RDP_DEBUG
        tlog(c->rdpSocket, LL_DEBUG, "packet wrong seqnr: %d.", seqCnt);
#endif
      }

      return -1;
    }

    c->lastReceivedPacket = c->rdpSocket->mstime;

    uint16_t ackCnt = (packnr - (c->seqnr - c->queue) + 1) & RDP_ACK_NR_MASK;

    if (ackCnt > c->queue) {
      ackCnt = 0;
    }

    c->recvWindowPeer = p->window;

    // Connection handshake.
    if (type == ST_DATA && c->state == CS_SYN_RECV) {
      c->state = CS_CONNECTED;
      *events = RDP_ACCEPT;
    }

    if (type == ST_STATE && c->state == CS_SYN_SENT) {
      // Outgoing connection completion.
      c->state = CS_CONNECTED;
      *events = RDP_CONNECTED;
    }

    if (c->state == CS_FIN_SENT && c->queue == ackCnt) {
      // Active close completion.
      c->state = CS_DESTROY;

#ifdef RDP_DEBUG
      tlog(c->rdpSocket, LL_DEBUG,
           "change state to CS_DESTROY, active close completion.");
#endif
    }

    for (int i = 0; i < ackCnt; ++i) {
      ackPacket(c, c->seqnr - c->queue);
      c->queue--;
    }

    while (c->queue > 0 && !rbufferGet(&c->outbuf, c->seqnr - c->queue)) {
      // Something is wrong.
      assert(0);
    }

    if (c->queue > 0 && sackMask) {
      selectiveAck(c, packnr + 2, sackMask, sackMask[-1]);
    }

#ifdef RDP_DEBUG
    if (c->queue == 0)
      assert(c->flightWindow == 0);
    assert(c->queue == 0 || rbufferGet(&c->outbuf, c->seqnr - c->queue));
#endif

    if (c->state == CS_CONNECTED_FULL && !rdpConnFlightWindowFull(c)) {
      c->state = CS_CONNECTED;

      *events |= RDP_POLLOUT;

#ifdef RDP_DEBUG
      tlog(c->rdpSocket, LL_DEBUG, "full -----------> not full");
#endif
    }

    if (type == ST_STATE) {
      return -1;
    }

    if (c->state != CS_CONNECTED && c->state != CS_CONNECTED_FULL &&
        c->state != CS_FIN_SENT) {
#ifdef RDP_DEBUG
      tlog(c->rdpSocket, LL_DEBUG, "connection not connected. state: %s",
           connStateNames[c->state]);
#endif
      return -1;
    }

    if (type == ST_FIN) {
      if (c->state == CS_FIN_SENT) {
        c->state = CS_DESTROY;

        return -1;
      }

      if (!c->receivedFin) {
        // Passive close received.
        c->receivedFin = 1;
        c->eofseqnr = pseqnr;
      }
    }

    if (c->state == CS_FIN_SENT) {
      return -1;
    }

    // Right next packet expected.
    if (seqCnt == 0) {
      if (payload > 0 && c->state != CS_FIN_SENT) {

        // Exceeded supplied buffer length.
        if (payload > len) {
          *events = RDP_ERROR;

#ifdef RDP_DEBUG
          tlog(c->rdpSocket, LL_DEBUG,
               "payloadCnt exceeded supplied buf length.");
#endif

          return -1;
        } else {
          memmove(buf, payloadStart, payload);
          *events |= RDP_DATA;
        }
      }
      c->acknr++;
      c->needSendAck = 1;

      return payload == 0 ? -1 : payload;
    } else {
      if (c->receivedFin && pseqnr > c->eofseqnr) {

#ifdef RDP_DEBUG
        tlog(c->rdpSocket, LL_DEBUG, "seqnr bigger than eof packet.");
#endif
        return -1;
      }

      rbufferEnsureSize(&c->inbuf, pseqnr + 1, seqCnt + 1);

      if (rbufferGet(&c->inbuf, pseqnr) != NULL) {

#ifdef RDP_DEBUG
        c->duplicateSum++;
#endif

        c->needSendAck = 1;

        return -1;
      }

      uint8_t *packetWithPrefix = (uint8_t *)malloc((payload) + sizeof(uint));
      assert(packetWithPrefix);
      *(uint *)packetWithPrefix = (uint)payload;
      memcpy(packetWithPrefix + sizeof(uint), payloadStart, payload);

      assert(rbufferGet(&c->inbuf, pseqnr) == NULL);
      assert((pseqnr & c->inbuf.mask) != ((c->acknr + 1) & c->inbuf.mask));

      rbufferPut(&c->inbuf, pseqnr, packetWithPrefix);

      c->outOfOrderCnt++;

#ifdef RDP_DEBUG
      c->outOfOrderSum++;
#endif

      c->needSendAck = 1;

      return -1;
    }

    assert(0);
  }

  return -1;
}

void *rdpConnGetUserData(rdpConn *c) {
  assert(c);
  if (!c)
    return NULL;

  return c->userData;
}

int rdpConnSetUserData(rdpConn *c, void *userData) {
  assert(c);
  if (!c)
    return -1;

  c->userData = userData;

  return 0;
}

int rdpSocketGetProp(rdpSocket *s, int opt) {
  assert(s);
  if (!s)
    return -1;

  switch (opt) {
  case RDP_PROP_FD:
    return s->fd;
  case RDP_PROP_SNDBUF:
    return s->sendBufferSize;
  case RDP_PROP_RCVBUF:
    return s->recvBufferSize;
  }
  return -1;
}

int rdpSocketSetProp(rdpSocket *s, int opt, int val) {
  if (!s)
    return -1;

  switch (opt) {
  case RDP_PROP_FD:
    s->fd = val;
    return 0;

  case RDP_PROP_SNDBUF:
    s->sendBufferSize = val;
    return 0;

  case RDP_PROP_RCVBUF:
    s->recvBufferSize = val;
    return 0;
  }
  return -1;
}

// Use ack packet as keep alive probe.
static void rdpConnKeepAlive(rdpConn *c) {
  c->acknr--;

  sendAck(c);

  c->acknr++;
}

static int resizeWindow(rdpConn *c) {
  struct packetWrap *pw =
      (struct packetWrap *)rbufferGet(&c->outbuf, c->seqnr - c->queue);

  assert(pw);

#ifdef RDP_DEBUG_2
  tlog(c->rdpSocket, LL_DEBUG,
       "id: %d, trans: %d, limit: %d, fliwin: %d, o: %d, s-e: %d", c->recvId,
       pw->transmissions, c->flightWindowLimit, c->flightWindow,
       c->oldestResent, c->seqnr - c->queue);
#endif

  // Have't start retransmit. Do nothing.
  if (c->oldestResent == -1) {

    c->oldestResent = c->seqnr - c->queue;

    return 0;
  }

  // Last retransmit failed.
  // Shrink the window until it have only one packet space.
  if (c->oldestResent == c->seqnr - c->queue) {
    c->flightWindowLimit =
        limitedWindow(c->flightWindowLimit / RDP_WINDOW_SHRINK_FACTOR);

    return 0;
  }

  // Last retransmit succeed. Expand it.
  if (c->oldestResent != c->seqnr - c->queue) {
    c->flightWindowLimit =
        limitedWindow(c->flightWindowLimit * RDP_WINDOW_EXPAND_FACTOR);

    c->oldestResent = c->seqnr - c->queue;

    return 0;
  }

  assert(0);

  return -1;
}

// Only update after process the previous retransmit event.
static int updateRetransmitTimeout(rdpConn *c) {
  uint32_t afterLastSent = 0;
  if (c->queue != 0) {
    struct packetWrap *pw =
        (struct packetWrap *)rbufferGet(&c->outbuf, c->seqnr - c->queue);

    assert(pw);
    assert(pw->transmissions);
    assert(pw->sentTime);

    afterLastSent = c->rdpSocket->mstime - pw->sentTime;
  }

  // Update retransmitTimeout.
  c->retransmitTimeout = c->nextRetransmitTimeout - afterLastSent;
  if (c->retransmitTimeout < 0)
    c->retransmitTimeout = 0;

  // retransmitTicker of rdpConn can only be updated here.
  c->retransmitTicker = c->rdpSocket->mstime + c->retransmitTimeout;

  return 0;
}

// Flush packets and send acks.
static int rdpConnCheck(rdpConn *c) {

#ifdef RDP_DEBUG

  int inbufCnt = rbufferGetFilled(&c->inbuf);
  int outbufCnt = rbufferGetFilled(&c->outbuf);

  if (inbufCnt > 0 || outbufCnt > 0) {
    //    tlog(c->rdpSocket, LL_DEBUG,
    //         "conn id: %d, state: %s, outoforder: %d, inbuf: %d, outbuf: "
    //         "%d, flight window: "
    //         "%d, window "
    //         "queue: %d, errInfo: %s",
    //         c->recvId, connStateNames[c->state], c->outOfOrderCnt,
    //         inbufCnt, outbufCnt, c->flightWindow, c->queue, c->errInfo);
    //
    // tlog(c->rdpSocket, LL_DEBUG,
    //     "out of date sum: %d, out of order sum: %d, duplicate sum: %d",
    //     c->outOfDateSum, c->outOfOrderSum, c->duplicateSum);
  }

#endif

  assert(c->queue == 0 || rbufferGet(&c->outbuf, c->seqnr - c->queue));

  switch (c->state) {
  case CS_SYN_SENT:
  case CS_SYN_RECV:
  case CS_CONNECTED_FULL:
  case CS_CONNECTED:
  case CS_FIN_SENT: {
    // It's time for the connection timeout check.
    if (c->rdpSocket->mstime >= c->retransmitTicker) {

      if (c->state == CS_FIN_SENT &&
          c->rdpSocket->mstime >= c->lastReceivedPacket + RDP_WAIT_FIN_SENT) {
        c->state = CS_DESTROY;

        return 0;
      }

      if (c->state == CS_SYN_RECV &&
          c->rdpSocket->mstime >= c->lastReceivedPacket + RDP_WAIT_SYN_RECV) {
        c->state = CS_DESTROY;

        return 0;
      }

      if (c->queue > 0) {
        // Prepare to retransmit.
        for (uint16_t i = c->seqnr - c->queue; i != c->seqnr; i++) {
          struct packetWrap *pw = rbufferGet(&c->outbuf, i);

          if (pw == NULL || pw->transmissions == 0 || pw->needResend == 1 ||
              c->rdpSocket->mstime < pw->sentTime + c->retransmitTimeout)
            continue;

          pw->needResend = 1;

          assert(c->flightWindow >= pw->payload);
          c->flightWindow -= pw->payload;
        }

        resizeWindow(c);

        // Retransmitting.
        rdpConnFlushPackets(c);
      }

      // Update after retransmit.
      updateRetransmitTimeout(c);
    }

    if (c->state == CS_CONNECTED || c->state == CS_CONNECTED_FULL) {
      if (c->rdpSocket->mstime >= c->lastSentPacket + RDP_KEEPALIVE_INTERVAL) {

        rdpConnKeepAlive(c);
      }
    }

    break;
  }
  case CS_UNINITIALIZED:
  case CS_DESTROY:
    break;
  default:
    assert(0);
  }

  assert(c->retransmitTicker - c->rdpSocket->mstime >= 0);

  c->rdpSocket->nextCheckTimeout =
      min(RDP_SOCKET_CHECK_TIMEOUT_MAX,
          max(RDP_SOCKET_CHECK_TIMEOUT_MIN,
              min(c->rdpSocket->nextCheckTimeout,
                  c->retransmitTicker - c->rdpSocket->mstime)));
}

// Should be invoked periodically, before sleep.
// Return a timeout next time this function should be invoked again, in
// milliseconds.
int rdpSocketIntervalAction(rdpSocket *s) {
  if (!s)
    return -1;

  s->mstime = mstime();

  if (s->mstime < s->lastCheck + s->nextCheckTimeout) {
    return s->nextCheckTimeout - (s->mstime - s->lastCheck);
  }

  s->lastCheck = s->mstime;
  s->nextCheckTimeout = RDP_SOCKET_CHECK_TIMEOUT_DEFAULT;

  listIterator *iter = listIteratorCreate(s->conns, LIST_START_HEAD);
  listNode *node;
  rdpConn *c;
  while (node = listIteratorNext(iter)) {
    c = (rdpConn *)node->value;
    rdpConnCheck(c);

    if (c->state == CS_DESTROY) {
      rdpConnDestroy(c);
    }
  }
  listIteratorDestroy(iter);

  return s->nextCheckTimeout;
}

rdpConn *rdpNetConnect(rdpSocket *s, const char *host, const char *service) {
  rdpConn *c;
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int connectRes, flags, conn, optval;

  // Get address.
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  hints.ai_socktype = SOCK_DGRAM;
  // AF_UNSPEC allows IPv4 or IPv6
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_PASSIVE;

  int res = getaddrinfo(host, service, &hints, &result);
  if (res != 0) {
    errno = ENOSYS;

#ifdef RDP_DEBUG
    write(STDOUT_FILENO, gai_strerror(res), strlen(gai_strerror(res)));
#endif

    return NULL;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    c = rdpConnCreate(s);
    if (c == NULL) {
#ifdef RDP_DEBUG
      tlog(s, LL_DEBUG, "rdpConnCreate");
#endif
      return NULL;
    }

    connectRes = rdpConnect(c, result->ai_addr, result->ai_addrlen);
    if (connectRes == -1) {
      rdpConnClose(c);
      continue;
    }
    break;
  }

  freeaddrinfo(result);

  return (rp == NULL) ? NULL : c;
}