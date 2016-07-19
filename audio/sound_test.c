#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "pcm_sound.h"

#define BROADCAST_ADDR "255.255.255.255"
#define ALL_RECEIVERS 255
#define HEARTBEAT_SOURCE 1
#define COMMAND_PORT 6660
#define HB_PORT 5550

/*
typedef unsigned char uint8_t;
typedef unsigned long uint32_t;
typedef unsigned short uint16_t;
*/

struct __BPMPulseData_t {
    uint8_t  pod_id; // which pod. pass as param in startup file.

    uint8_t  rolling_sequence;
    // just repeat every 256 iterations. for tracking gaps in
    // UDP packets in case we have some situation where it happens.

    uint16_t beat_interval_ms;
    // min BPM 30 has ms interval of 2000
    // max BPM 200 has ms interval of 300

    uint32_t elapsed_ms; // how long before now did this happen?
    
    uint32_t timestamp;  // timestamp, normalized to the pod that sent this. Do not assume that the pods are in sync.

    float est_BPM; // computed as 60*1000/beat_interval_ms  // XXX why do we have this? and why is it a float?
 
}__attribute__((packed)); 

typedef struct __BPMPulseData_t BPMPulseData_t;

typedef struct __attribute((packed)) {
    uint8_t  receiver_id;         // which unit is this command for. 255 means 'this is for everyone'
    uint8_t  command_tracking_id; // id specific to this command, can be used for ACK/NACK if we build that 
    uint16_t command_id;          // command for the unit; do not respond to commands that you don't understand
    uint32_t command_data;        // may or may not have anything in it, depending on the command
} PulseCommand_t;

uint8_t myId = 1; // XXX need to read this from a config file somewhere
uint8_t hbSource;   
int hbSocket;
int cmdSocket; 
    

static int initBroadcastSocketListener(unsigned short port);
static void pulseAudioListen();


int main(int argc, char **argv)
{
    if (pcmPlaybackInit() < 0) {
        printf("Failure to initialize sound system, aborting\n");
        return -1;
    }

    /*
    pcmPlayHeartBeat(60);
    sleep(5*60);
    pcmPlaybackStop();
    */
    
    // XXX TODO - read myId from config file
    hbSource = myId;
    
    hbSocket = initBroadcastSocketListener(HB_PORT);
    if (hbSocket < 0) {
        printf("Could not creating hb socket listener (error %d), aborting\n", hbSocket);
        return -1;
    }
 
    cmdSocket = initBroadcastSocketListener(COMMAND_PORT);
    if (cmdSocket < 0) {
        printf("Could not creating command socket listener (error %d), aborting\n", cmdSocket);
        return -1;
    }
    
    pulseAudioListen();
    return 0;
}



static int initBroadcastSocketListener(unsigned short port)
{
    int sockfd = 0;
    int rv;
    int optval = 1;
    struct sockaddr_in addr;

    if ((rv = socket(AF_INET, SOCK_DGRAM, 0)) <= 0) {
        return rv;
    }
    sockfd = rv;

    if ((rv = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval)) != 0) {
        printf("setsocketopt returns %d, %s\n", rv, strerror(errno));
        return rv;
    }
    
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(BROADCAST_ADDR);
    
    if ((rv = bind(sockfd, (const struct sockaddr *)&addr, sizeof(addr))) < 0) {
        printf("bind returns %s\n", strerror(errno));
        return -EINVAL;
    }

    return sockfd;
}

static void pulseAudioListen()
{
    struct timeval timeout;
    int rv;
    
    fd_set read_fds, except_fds;
    while(1) {
        FD_ZERO(&read_fds);
        FD_ZERO(&except_fds);
        FD_SET(cmdSocket, &read_fds  );
        FD_SET(cmdSocket, &except_fds);
        FD_SET(hbSocket, &read_fds);
        FD_SET(hbSocket, &except_fds);
    
        // come up for air every second
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        // wait for timeout, or event on a socket... 
        rv = select(2, &read_fds, NULL, &except_fds, &timeout);
        
        if (rv == 0) {
            printf("listen timeout... all is well...\n");
            continue; // timeout, nothing to do.
        }
        
        if (rv < 0) {
            printf("Select returns error %d, %s\n", errno, strerror(errno));
            continue;
        }
        
        if (FD_ISSET(cmdSocket, &read_fds)) {
            // receive command datagram
            PulseCommand_t command;
            int numBytesRead = recv(cmdSocket, &command, sizeof(command), 0);
            if (numBytesRead < 0) {
                printf("Command socket: recv returns error %d, %s\n", numBytesRead,strerror(numBytesRead));
            } else if (numBytesRead < sizeof(command)) {
                printf("WARNING: Command socket: Receiving fewer bytes than expected, ignoring\n"); // Do we want/need to do anything with this?
            } else {
                // a real live command to parse!
                if (command.receiver_id == myId || command.receiver_id == ALL_RECEIVERS) {
                    if (command.command_id == HEARTBEAT_SOURCE) {
                        hbSource = (uint8_t)(command.command_data);
                    } else {
                        // Don't currently know any other commands. Do nothing ... yet...
                    }
                }
            }
        }

        if (FD_ISSET(hbSocket, &read_fds)) {
            // receive hb datagram
            BPMPulseData_t hbData;
            int numBytesRead = recv(hbSocket, &hbData, sizeof(hbData), 0);
            if (numBytesRead < 0) {
                printf("HB socket: recv returns error %d, %s\n", numBytesRead, strerror(numBytesRead));
            } else if (numBytesRead < sizeof(hbData)) {
                printf("WARNING: HB socket: Receiving fewer bytes than expected, ignoring\n"); // Do we want/need to do anything with this?
            } else {
                // a real, live heartbeat!
                if (hbData.pod_id == hbSource) {
                    uint32_t hbRate = 60*1000/(hbData.beat_interval_ms); // yes, I am rounding here.
                    if (hbRate < 30 || hbRate > 200) { 
                        printf("Heartbeat rate %d is out of bounds\n", hbRate);
                    } else {
                        pcmPlayHeartBeat(hbRate);
                    }
                }
            }
        } 
        
        if (FD_ISSET(cmdSocket, &except_fds)) {
            printf("exception on cmd socket\n");
            // XXX not exactly sure what sort of exceptions we might see here. Leaving blank for now.
        }
        
        if (FD_ISSET(hbSocket, &except_fds)) {
            printf("exception on hb socket\n");
            // XXX not exactly sure what sort of exceptions we might see here. Leaving blank for now.
        }
    }
}