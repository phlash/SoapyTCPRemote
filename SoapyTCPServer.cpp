//  SoapyTCPserver.cpp
//  Copyright (c) 2021 Phil Ashby
//  SPDX-License-Identifier: BSL-1.0

// Design approach is KISS, main thread accepts connections into a
// map, handles RPCs.
// Worker threads are created per data stream to pump in/out.
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Device.hpp>
#include "SoapyRPC.hpp"
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>

class ConnectionInfo
{
public:
/*    ~ConnectionInfo() {
        if (netFp)
            fclose(netFp);
        if (dev)
            SoapySDR::Device::unmake(dev);
    } */
    // NB: existance of an rpc object implies this is an RPC connection, otherwise data stream
    SoapyRPC *rpc;
    SoapySDR::Device *dev;
    FILE *netFp;
    int direction;
    int rate;
    size_t fSize;
    size_t numChans;
    SoapySDR::Stream *stream;
    volatile pthread_t pid;
};

static std::map<int, ConnectionInfo> s_connections;

int createRpc(int sock) {
    SoapySDR_log(SOAPY_SDR_TRACE, "createRpc");
    ConnectionInfo conn;
    conn.rpc = new SoapyRPC(sock);
    // TODO: BODGE!!
    conn.rate = 192000;
    // read driver and args..
    SoapySDR::Kwargs kwargs;
    kwargs["driver"] = conn.rpc->readString();
    std::string args = conn.rpc->readString();
    // args contains all driver name=value pairs, separated by '/',
    // splitting this is a faff as there is no native method..
    // http://www.cplusplus.com/faq/sequences/strings/split/
    size_t cur, nxt = -1;
    do {
        cur = nxt+1;
        nxt = args.find('/',cur);
        std::string arg = args.substr(cur, nxt-cur);
        // now we split this into name=value...
        size_t off = arg.find('=');
        if (off!=std::string::npos) {
            kwargs[arg.substr(0,off)]=arg.substr(off+1);
        }
    } while (nxt != std::string::npos);
    // make the device
    conn.dev = SoapySDR::Device::make(kwargs);
    if (!conn.dev) {
        // oops - report failure to client and drop connection
        SoapySDR_logf(SOAPY_SDR_ERROR,"failed to create SoapySDR::Device: %s", kwargs["driver"]);
        conn.rpc->writeInteger(-1);
        delete conn.rpc;
        return 0;
    }
    // all good - add to map and respond with map key
    s_connections[sock] = conn;
    conn.rpc->writeInteger(sock);
    return 0;
}

int createData(int sock, int type) {
    SoapySDR_logf(SOAPY_SDR_TRACE, "createData, type: %d", type);
    ConnectionInfo conn;
    conn.rpc = nullptr;     // ensure we aren't treated as RPC stream
    conn.netFp = fdopen(sock,TCPREMOTE_DATA_SEND==type? "w": "r");
    if (!conn.netFp) {
        SoapySDR_logf(SOAPY_SDR_ERROR,"failed to open stdio stream on data connection: %s", strerror(errno));
        close(sock);
        return -1;
    }
    // all good - add to map and respond with map key
    // NB: we write to raw socket as stdio stream may be read-only..
    s_connections[sock] = conn;
    char id[10];
    int ilen = sprintf(id,"%d\n",sock);
    write(sock, id, ilen);
    return 0;
}

void *dataPump(void *ctx) {
    ConnectionInfo *conn = (ConnectionInfo *)ctx;
    // first - activate the underlying stream
    if (conn->dev->activateStream(conn->stream)) {
        SoapySDR_log(SOAPY_SDR_ERROR, "dataPump: failed to activate underlying stream");
        return nullptr;
    }
    // which direction?
    if (SOAPY_SDR_RX==conn->direction) {
        // calculate appropriate element count and block sizes for ~4Hz read rate
        size_t numElems = conn->rate / 4;
        size_t blkSize = numElems * conn->fSize;
        size_t bufSize = blkSize * conn->numChans;
        // allocate buffers for channel data and serialised network data
        void **buffs = (void **)alloca(conn->numChans);
        uint8_t *cbuf = (uint8_t *)alloca(bufSize);
        uint8_t *nbuf = (uint8_t *)alloca(bufSize);
        for (size_t c=0; c<conn->numChans; ++c)
            buffs[c] = cbuf+(c*blkSize);
        // read until told to stop!
        while (conn->pid!=0) {
            int flags = 0;
            long long time = 0;
            int nread = conn->dev->readStream(conn->stream, buffs, numElems, flags, time);
            if (nread<0) {
                SoapySDR_logf(SOAPY_SDR_ERROR, "dataPump: error reading underlying stream: %d", nread);
                break;
            }
            // interleave samples across channels for network format
            uint8_t *pn = nbuf;
            for (int idx=0; idx<nread; ++idx) {
                size_t eoff = idx*conn->fSize;
                for (size_t c=0; c<conn->numChans; ++c) {
                    uint8_t *pc = (uint8_t *)buffs[c];
                    memcpy(pn, pc+eoff, conn->fSize);
                    pn += conn->fSize;
                }
            }
            // write to network
            if ((int)fwrite(nbuf, conn->fSize*conn->numChans, nread, conn->netFp)!=nread) {
                SoapySDR_logf(SOAPY_SDR_ERROR, "dataPump: error writing to network: %s", strerror(errno));
                break;
            }
        }
    } else {
        // TODO:XXX:
        SoapySDR_log(SOAPY_SDR_ERROR, "dataPump: unimplemented data receive funtion :=(");
    }
    // dropping out - deactivate underlying stream
    conn->dev->deactivateStream(conn->stream);
    return nullptr;
}

int handleListen(struct pollfd *pfd) {
    // oops before expected..
    if (pfd->revents & (POLLERR|POLLHUP)) {
        SoapySDR_log(SOAPY_SDR_ERROR,"EOF or error in listen socket");
        return -1;
    }
    // connect pending
    if (pfd->revents & POLLIN) {
        struct sockaddr addr;
        socklen_t len = sizeof(addr);
        int sock = accept(pfd->fd, &addr, &len);
        if (sock<0) {
            SoapySDR_logf(SOAPY_SDR_ERROR,"error accepting connection: %s", strerror(errno));
            return -1;
        }
        // now we read an integer which types the connection
        char buf[2];
        if (read(sock, buf, 2)<=0) {
            SoapySDR_logf(SOAPY_SDR_ERROR,"error reading connection type: %s", strerror(errno));
            close(sock);
            return -1;
        }
        int type = buf[0]-'0';
        // create appropriate ConnectionInfo and insert into map..
        if (TCPREMOTE_RPC_LOAD==type)
            return createRpc(sock);
        else if (TCPREMOTE_DATA_SEND==type || TCPREMOTE_DATA_RECV==type)
            return createData(sock, type);
        // ..or drop it as unknown.
        SoapySDR_logf(SOAPY_SDR_ERROR, "unknown connection type: %d", type);
        close(sock);
    }
    return 0;
}

int handleGetHardwareKey(ConnectionInfo &conn) {
    // pass-thru
    conn.rpc->writeString(conn.dev->getHardwareKey());
    return 0;
}

int handleGetHardwareInfo(ConnectionInfo &conn) {
    // pass-thru
    conn.rpc->writeKwargs(conn.dev->getHardwareInfo());
    return 0;
}

int handleGetNumChannels(ConnectionInfo &conn) {
    // pass-thru
    conn.rpc->writeInteger(conn.dev->getNumChannels(conn.rpc->readInteger()));
    return 0;
}

int handleGetChannelInfo(ConnectionInfo &conn) {
    // pass-thru
    conn.rpc->writeKwargs(
        conn.dev->getChannelInfo(
            conn.rpc->readInteger(),
            conn.rpc->readInteger()
        )
    );
    return 0;
}

int handleGetStreamFormats(ConnectionInfo &conn) {
    // pass-thru (with some serialisation)
    for (auto &fmt: conn.dev->getStreamFormats(
        conn.rpc->readInteger(),
        conn.rpc->readInteger())) {
        conn.rpc->writeString(fmt);
    }
    // terminate list
    conn.rpc->writeString("");
    return 0;
}

int handleGetNativeStreamFormat(ConnectionInfo &conn) {
    // pass-thru
    double fullScale = 0.0;
    std::string fmt = conn.dev->getNativeStreamFormat(
        conn.rpc->readInteger(),
        conn.rpc->readInteger(),
        fullScale
    );
    conn.rpc->writeString(fmt);
    conn.rpc->writeInteger((int)fullScale);
    return 0;
}

int handleGetStreamArgsInfo(ConnectionInfo &conn) {
    // not implemented, skeleton only
    conn.rpc->readInteger();
    conn.rpc->readInteger();
    conn.rpc->writeString("");
    return 0;
}

int handleSetupStream(ConnectionInfo &conn) {
    // The actually complex(ish) bit..
    int dataId = conn.rpc->readInteger();
    int direction = conn.rpc->readInteger();
    std::string fmt = conn.rpc->readString();
    std::string chans = conn.rpc->readString();
    SoapySDR::Kwargs args = conn.rpc->readKwargs();
    // find the data stream (client must connect a data stream first)
    if (s_connections.find(dataId)==s_connections.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "setupStream: no such data stream ID: %d", dataId);
        conn.rpc->writeInteger(-1);
        return 0;
    }
    // find the frame size
    if (g_frameSizes.find(fmt)==g_frameSizes.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "setupStream: unknown sample format: %s", fmt.c_str());
        conn.rpc->writeInteger(-2);
        return 0;
    }
    // parse the channel list
    std::vector<size_t> channels;
    size_t cur;
    size_t nxt = -1;
    do {
        cur = nxt+1;
        nxt = chans.find(' ',cur);
        channels.push_back(atoi(chans.substr(cur, nxt-cur).c_str()));
    } while (nxt!=std::string::npos);
    // fill out the connection details
    ConnectionInfo &data = s_connections.at(dataId);
    data.direction = direction;
    data.rate = conn.rate;
    data.fSize = g_frameSizes.at(fmt);
    data.numChans = channels.size();
    // open the channel
    data.stream = conn.dev->setupStream(direction, fmt, channels, args);
    if (!data.stream) {
        SoapySDR_log(SOAPY_SDR_ERROR, "setupStream: failed to create underlying stream");
        conn.rpc->writeInteger(-3);
    }
    // all good!
    conn.rpc->writeInteger(dataId);
    return 0;
}

int handleCloseStream(ConnectionInfo &conn) {
    int dataId = conn.rpc->readInteger();
    if (s_connections.find(dataId)==s_connections.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "closeStream: no such data stream ID: %d", dataId);
        return 0;
    }
    ConnectionInfo &data = s_connections.at(dataId);
    data.dev->closeStream(data.stream);
    // no response
    return 0;
}

int handleGetStreamMTU(ConnectionInfo &conn) {
    // pass-thru
    int dataId = conn.rpc->readInteger();
    if (s_connections.find(dataId)==s_connections.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "getStreamMTU: no such data stream ID: %d", dataId);
        conn.rpc->writeInteger(-1);
        return 0;
    }
    return conn.rpc->writeInteger(conn.dev->getStreamMTU(s_connections.at(dataId).stream));
}

int handleActivateStream(ConnectionInfo &conn) {
    int dataId = conn.rpc->readInteger();
    if (s_connections.find(dataId)==s_connections.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "activateStream: no such data stream ID: %d", dataId);
        conn.rpc->writeInteger(-1);
        return 0;
    }
    // start data pump thread
    ConnectionInfo &data = s_connections.at(dataId);
    pthread_t pid;
    if (pthread_create(&pid, nullptr, dataPump, &data)) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "activateStream: failed to start data pump thread: %s", strerror(errno));
        conn.rpc->writeInteger(-1);
        return 0;
    }
    data.pid = pid;
    conn.rpc->writeInteger(1);
    return 0;
}

int handleDeactivateStream(ConnectionInfo &conn) {
    int dataId = conn.rpc->readInteger();
    if (s_connections.find(dataId)==s_connections.end()) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "deactivateStream: no such data stream ID: %d", dataId);
        conn.rpc->writeInteger(-1);
        return 0;
    }
    // stop data pump thread
    ConnectionInfo &data = s_connections.at(dataId);
    pthread_t pid = data.pid;
    data.pid = 0;
    if (pthread_join(pid, nullptr)) {
        SoapySDR_logf(SOAPY_SDR_ERROR, "activateStream: failed to stop data pump thread: %s", strerror(errno));
        conn.rpc->writeInteger(-1);
        return 0;
    }
    conn.rpc->writeInteger(1);
    return 0;
}

int handleRPC(struct pollfd *pfd) {
    // oops before expected..
    if (pfd->revents & (POLLERR|POLLHUP)) {
        SoapySDR_log(SOAPY_SDR_ERROR,"EOF or error in RPC socket");
        return -1;
    }
    // get connection..
    ConnectionInfo &conn = s_connections.at(pfd->fd);
    // dispatch requested RPC..
    int call = conn.rpc->readInteger();
    SoapySDR_logf(SOAPY_SDR_TRACE, "handleRPC: call=%d", call);
    switch (call) {
    case TCPREMOTE_GET_HARDWARE_KEY:
        return handleGetHardwareKey(conn);
    case TCPREMOTE_GET_HARDWARE_INFO:
        return handleGetHardwareInfo(conn);
    case TCPREMOTE_GET_NUM_CHANNELS:
        return handleGetNumChannels(conn);
    case TCPREMOTE_GET_CHANNEL_INFO:
        return handleGetChannelInfo(conn);
    case TCPREMOTE_GET_STREAM_FORMATS:
        return handleGetStreamFormats(conn);
    case TCPREMOTE_GET_STREAM_NATIVE_FORMAT:
        return handleGetNativeStreamFormat(conn);
    case TCPREMOTE_GET_STREAM_ARGS_INFO:
        return handleGetStreamArgsInfo(conn);
    case TCPREMOTE_SETUP_STREAM:
        return handleSetupStream(conn);
    case TCPREMOTE_CLOSE_STREAM:
        return handleCloseStream(conn);
    case TCPREMOTE_GET_STREAM_MTU:
        return handleGetStreamMTU(conn);
    case TCPREMOTE_ACTIVATE_STREAM:
        return handleActivateStream(conn);
    case TCPREMOTE_DEACTIVATE_STREAM:
        return handleDeactivateStream(conn);
    default:
        SoapySDR_logf(SOAPY_SDR_ERROR,"Unknown RPC call: %d", call);
        conn.rpc->writeInteger(-1);
    }
    return 0;
}

int usage() {
    puts("usage: SoapyTCPServer [-?|--help] [-l <listen host/IP:default *>] [-p <listen port: default 20655>]");
    return 0;
}

int main(int argc, char **argv) {
    const char *host = "0.0.0.0";
    const char *port = "20655";           // 0x50AF
    for (int arg=1; arg<argc; ++arg) {
        if (strncmp(argv[arg],"-?",2)==0 || strncmp(argv[arg],"--h",3)==0)
            return usage();
        else if (strncmp(argv[arg],"-h",2)==0)
            host = argv[++arg];
        else if (strncmp(argv[arg],"-p",2)==0)
            port = argv[++arg];
    }
    printf("SoapyTCPServer: listening on: %s:%s\n", host, port);
    // Set up listen socket
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host, port, nullptr, &res)) {
        SoapySDR_logf(SOAPY_SDR_ERROR,"parsing listen host");
        return 1;
    }
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int));
    if (lsock<0 || bind(lsock, res->ai_addr, res->ai_addrlen)!=0) {
        SoapySDR_logf(SOAPY_SDR_ERROR,"binding listen socket");
        return 2;
    }
    listen(lsock, 5);
    freeaddrinfo(res);
    // Wait for connections / requests on RPC sockets
    while (true) {
        // allocate maximum possible number of pollfds, then omit data stream connections
        size_t nfds = s_connections.size()+1;
        struct pollfd *pfds = (struct pollfd *)alloca(nfds);
        pfds[0].fd = lsock;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        nfds = 1;
        for (auto it=s_connections.begin(); it!=s_connections.end(); ++it) {
            if (it->second.rpc) {  // RPC connection
                pfds[nfds].fd = it->first;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                ++nfds;
            }
        }
        if (poll(pfds, nfds, -1)<=0) {
            SoapySDR_logf(SOAPY_SDR_ERROR,"waiting for input");
            return 3;
        }
        // Handle listen socket events
        if (pfds[0].revents && handleListen(pfds)<0)
            break;
        // Handle RPC socket events
        for (size_t idx=1; idx<nfds; ++idx)
            if (pfds[idx].revents && handleRPC(pfds+idx)<0)
                return 4;
    }
    s_connections.clear();
    close(lsock);
    return 0;
}