/*
 *  ethernet_linux.c
 *
 *  Copyright 2013 Michael Zillgith
 *
 *  This file is part of libIEC61850.
 *
 *  libIEC61850 is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  libIEC61850 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with libIEC61850.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  See COPYING file for the complete license text.
 */

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <linux/net_tstamp.h>
#include <linux/sockios.h>

#include <string.h>

#include "libiec61850_platform_includes.h"
#include "hal_ethernet.h"

struct sEthernetSocket {
    int rawSocket;
    bool isBind;
    struct sockaddr_ll socketAddress;
};

struct sEthernetHandleSet {
    struct pollfd *handles;
    int nhandles;
};

EthernetHandleSet
EthernetHandleSet_new(void)
{
    EthernetHandleSet result = (EthernetHandleSet) GLOBAL_MALLOC(sizeof(struct sEthernetHandleSet));

    if (result != NULL) {
        result->handles = NULL;
        result->nhandles = 0;
    }

    return result;
}

void
EthernetHandleSet_addSocket(EthernetHandleSet self, const EthernetSocket sock)
{
    if (self != NULL && sock != NULL) {

        int i = self->nhandles++;

        self->handles = realloc(self->handles, self->nhandles * sizeof(struct pollfd));

        self->handles[i].fd = sock->rawSocket;
        self->handles[i].events = POLLIN;
    }
}

void
EthernetHandleSet_removeSocket(EthernetHandleSet self, const EthernetSocket sock)
{
    if ((self != NULL) && (sock != NULL)) {

        int i;

        for (i = 0; i < self->nhandles; i++) {
            if (self->handles[i].fd == sock->rawSocket) {
                memmove(&self->handles[i], &self->handles[i+1], sizeof(struct pollfd) * (self->nhandles - i - 1));
                self->nhandles--;
                return;
            }
        }
    }
}

int
EthernetHandleSet_waitReady(EthernetHandleSet self, unsigned int timeoutMs)
{
    int result;

    if ((self != NULL) && (self->nhandles >= 0)) {
        result = poll(self->handles, self->nhandles, timeoutMs);
    }
    else {
       result = -1;
    }

    return result;
}

void
EthernetHandleSet_destroy(EthernetHandleSet self)
{
    if (self->nhandles)
        free(self->handles);

    GLOBAL_FREEMEM(self);
}

static int
getInterfaceIndex(int sock, const char* deviceName)
{
    struct ifreq ifr;

    strncpy(ifr.ifr_name, deviceName, IFNAMSIZ);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) == -1) {
        perror("ETHERNET_LINUX: Failed to get interface index -> exit");
        exit(1);
    }

    int interfaceIndex = ifr.ifr_ifindex;

    if (ioctl (sock, SIOCGIFFLAGS, &ifr) == -1)
    {
        perror ("ETHERNET_LINUX: Problem getting device flags -> exit");
        exit (1);
    }

    ifr.ifr_flags |= IFF_PROMISC;
    if (ioctl (sock, SIOCSIFFLAGS, &ifr) == -1)
    {
        perror ("ETHERNET_LINUX: Setting device to promiscuous mode failed -> exit");
        exit (1);
    }

    return interfaceIndex;
}


void
Ethernet_getInterfaceMACAddress(const char* interfaceId, uint8_t* addr)
{
    struct ifreq buffer;

    int sock = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&buffer, 0x00, sizeof(buffer));

    strcpy(buffer.ifr_name, interfaceId);

    ioctl(sock, SIOCGIFHWADDR, &buffer);

    close(sock);

    int i;

    for(i = 0; i < 6; i++ )
    {
        addr[i] = (unsigned char)buffer.ifr_hwaddr.sa_data[i];
    }
}

EthernetSocket
Ethernet_createSocket(const char* interfaceId, uint8_t* destAddress)
{
    EthernetSocket ethernetSocket = GLOBAL_CALLOC(1, sizeof(struct sEthernetSocket));

    //以太帧目前应该无0xffff协议类型，意味着初始不收包，象goose_publisher只发包，无需收包。
    ethernetSocket->rawSocket = socket(AF_PACKET, SOCK_RAW, htons(0xffff));
    //ethernetSocket->rawSocket = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));

    if (ethernetSocket->rawSocket == -1) {
        printf("Error creating raw socket!\n");
        GLOBAL_FREEMEM(ethernetSocket);
        return NULL;
    }

    ethernetSocket->socketAddress.sll_family = PF_PACKET;
    ethernetSocket->socketAddress.sll_protocol = htons(ETH_P_IP);

    ethernetSocket->socketAddress.sll_ifindex = getInterfaceIndex(ethernetSocket->rawSocket, interfaceId);

    ethernetSocket->socketAddress.sll_hatype =  ARPHRD_ETHER;
    ethernetSocket->socketAddress.sll_pkttype = PACKET_OTHERHOST;

    ethernetSocket->socketAddress.sll_halen = ETH_ALEN;

    memset(ethernetSocket->socketAddress.sll_addr, 0, 8);

    if (destAddress != NULL)
        memcpy(ethernetSocket->socketAddress.sll_addr, destAddress, 6);

    ethernetSocket->isBind = false;

    int val =  destAddress ? SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE :
                             SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (setsockopt(ethernetSocket->rawSocket, SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(int)) < 0)
        printf("Error setsockopt SO_TIMESTAMPING!\n");

    return ethernetSocket;
}

void
Ethernet_setProtocolFilter(EthernetSocket ethSocket, uint16_t etherType)
{
    ethSocket->socketAddress.sll_protocol = htons(etherType);

    if (ethSocket->isBind == false) {
        if (bind(ethSocket->rawSocket, (struct sockaddr*) &ethSocket->socketAddress, sizeof(ethSocket->socketAddress)) == 0)
            ethSocket->isBind = true;
        else {
            printf("Error bind raw socket!\n");
        }
    }
}


/* non-blocking receive */
int
Ethernet_receivePacket(EthernetSocket self, uint8_t* buffer, int bufferSize, int64_t *t)
{
    /* 在这里绑定, 函数被执行到前会收到些无用的包, 如telnet包, 虽然也无大碍
    if (self->isBind == false) {
        if (bind(self->rawSocket, (struct sockaddr*) &self->socketAddress, sizeof(self->socketAddress)) == 0)
            self->isBind = true;
        else
            return 0;
    }
    */

    struct iovec entry;
    entry.iov_base = buffer;
    entry.iov_len = bufferSize;

    struct {
        struct cmsghdr cm;
        char control[512];
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &entry;
    msg.msg_iovlen = 1;
    msg.msg_control = &control;
    msg.msg_controllen = sizeof(control);

    int ret = recvmsg(self->rawSocket, &msg, MSG_DONTWAIT);

    if (ret > 0) {
        struct cmsghdr *cmsg;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                struct timespec *stamp = (struct timespec *) CMSG_DATA(cmsg);
                if (t) *t = stamp->tv_sec * 1000000000LL + stamp->tv_nsec;
                //printf("---- rx SW ---- %ld.%09ld\n", (long) stamp->tv_sec, (long) stamp->tv_nsec);
                break;
            }
        }
    }

    return ret;
    //return recvfrom(self->rawSocket, buffer, bufferSize, MSG_DONTWAIT, 0, 0);
}

void
Ethernet_sendPacket(EthernetSocket ethSocket, uint8_t* buffer, int packetSize, int64_t *t)
{
    char buf[1518];
    while (recvfrom(ethSocket->rawSocket, buf, sizeof(buf), MSG_DONTWAIT, 0, 0) > 0);

    sendto(ethSocket->rawSocket, buffer, packetSize,
                0, (struct sockaddr*) &(ethSocket->socketAddress), sizeof(ethSocket->socketAddress));

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ethSocket->rawSocket, &readfds);
    struct timeval tv = {0, 500000};

    if (select(ethSocket->rawSocket + 1, &readfds, NULL, NULL, &tv) > 0) {
        struct {
            struct cmsghdr cm;
            char control[512];
        } control;
        memset(&control, 0, sizeof(control));

        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_control = &control;
        msg.msg_controllen = sizeof(control);

        int ret = recvmsg(ethSocket->rawSocket, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);

        if (ret >= 0) {
            struct cmsghdr *cmsg;
            for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
                    struct timespec *stamp = (struct timespec *) CMSG_DATA(cmsg);
                    if (t) *t = stamp->tv_sec * 1000000000LL + stamp->tv_nsec;
                    //printf("==== tx SW ==== %ld.%09ld\n", (long) stamp->tv_sec, (long) stamp->tv_nsec);
                    break;
                }
            }
        }
    }
}

void
Ethernet_destroySocket(EthernetSocket ethSocket)
{
    close(ethSocket->rawSocket);
    GLOBAL_FREEMEM(ethSocket);
}

bool
Ethernet_isSupported()
{
    return true;
}

int
Ethernet_socketDescriptor(EthernetSocket ethSocket)
{
    if (ethSocket)
        return ethSocket->rawSocket;
    return -1;
}
