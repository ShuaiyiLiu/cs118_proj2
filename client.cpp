/* headers for socket programming */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

/* std headers */
#include <string>
#include <iostream>

/* user-defined headers*/
#include "server.h"
#include "client.h"
#include "TCPOverUDP.h"

void TCPClient::setupAndRun() {
    /* This function sets up sockets, runs into event loop and transits the 
     * client state from CLOSED to SYN_SENT.
     **/
 
    /* now define remaddr, the address to whom we want to send messages */
    /* For convenience the host address is expressed as a numeric IP address */
    /* that we will convert to a binary format via inet_aton */

    struct sockaddr_in myaddr;

    memset((char *) &remaddr, 0, sizeof(remaddr));
    remaddr.sin_family = AF_INET;
    remaddr.sin_port = htons(server_port);
    if (inet_aton(server_host.c_str(), &remaddr.sin_addr)==0) {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }
    slen = sizeof remaddr;

    /* create a socket */
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        logger.logging(ERROR, "createing socket error");
    }
    /* bind it to all local addresses and pick any port number */
    memset((char *)&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(0);
    
    if (bind(sockfd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
        perror("bind failed");
        logger.logging(ERROR, "bind failed");
        return;
    } 

    // conjure a SYN packet here and send to server
    Packet syn_packet(INIT_SEQ, 0, RCVD_WINDOW_SIZE, 0, 1, 0);
    int nbytes = 0;
    if ((nbytes = sendto(sockfd, syn_packet.encode().c_str(), 
                    Packet::HEADER_LENGTH, 0,
                    (struct sockaddr *)&remaddr, slen)) == -1) {
        logger.logging(ERROR, "sendto error");
        return;
    }
    std::cout << "Sending packet " << syn_packet.getAckNumber() << " " 
              << "SYN" << std::endl;

    client_state = SYN_SENT;
    run();
}


int packetReceiver(int sockfd, char buf[], int max_len) {
    /* packet receiving helper function */
        int nbytes = 0;
        socklen_t addr_len;
        struct sockaddr_storage their_addr;
        if ((nbytes = recvfrom(sockfd, buf, max_len-1, 0,
            (struct sockaddr *)&their_addr, &addr_len)) == -1) {
            return -1;
        }
        return nbytes;
}


void TCPClient::run() {
    /* Main event loop for TCPClient.
     * This is where the client establishes connection with server, receives 
     * data packets, store the data, and etc.
     * */
    struct timeval tv;
    fd_set read_fds;
    FD_SET(sockfd, &read_fds);
    int nReadyFds = 0;

    while (true) {
        logger.logging(DEBUG, "Client running in state " + stateStringify());
        // set timeout timer depending on the state client is in
        switch(client_state) {
            case SYN_SENT: tv.tv_usec = RETRANS_TIMEOUT_USEC;
                           tv.tv_sec = 0;
                           break;
            case ESTABLISHED: tv.tv_usec = 0;
                              tv.tv_sec = ECHO_SEC;
                              break;
            case LAST_ACK: tv.tv_sec = FIN_TIME_WAIT;
                           tv.tv_usec = 0;
                           break;
            default: break;
        }

        // monitor sockfd file descriptor and timeout timer
        FD_SET(sockfd, &read_fds);
        if ((nReadyFds = select(sockfd+1, &read_fds, NULL, NULL, &tv)) == -1) {
            logger.logging(ERROR, "Select error.");
            continue;
        }
        
        // handle events from different state handlers
        switch(client_state) {
            case SYN_SENT: runningSynSent(nReadyFds); break;
            case ESTABLISHED: runningEstablished(nReadyFds); break;
            case LAST_ACK: runningLastAck(nReadyFds); break;
            case CLOSED: close(sockfd); return;
            default: break;
        }
    }
}


void TCPClient::runningSynSent(int nReadyFds) {
    /* Client behavior in SYN_SYNC state */
    if (nReadyFds == 0) {
        // timeout
        Packet syn_packet(INIT_SEQ, 0, RCVD_WINDOW_SIZE, 0, 1, 0);
        int nbytes = 0;
        if ((nbytes = sendto(sockfd, syn_packet.encode().c_str(), 
                        Packet::HEADER_LENGTH, 0,
                        (struct sockaddr *)&remaddr, slen)) == -1) {
            logger.logging(ERROR, "sendto error");
            return;
        }
        std::cout << "Sending packet " << syn_packet.getAckNumber() << " " 
                  << "Retransimission " << "SYN" << std::endl;
    } else {
        // we have a packet to receive
        char buf[MAX_BUF_LEN];
        int nbytes = packetReceiver(sockfd, buf, MAX_BUF_LEN);
        if (nbytes == -1) {
            logger.logging(ERROR, "recvfrom error.");
            return;
        }
        logger.logging(DEBUG, "got packet from server");

         /*       initialize recvbuffer
         *       change state to established
         **/
        std::string packet_encoded(buf, nbytes);
        Packet packet;
        packet.consume(packet_encoded);
        packet.payload_size = nbytes - HEADER_LENGTH;
        if (packet.getSyn() && packet.getAck() && !packet.getFin() 
                && packet.getAckNumber() == INIT_SEQ + 1) {
            std::cout << "Receiving packet " << packet.getSeqNumber() << std::endl;        
            Packet ack_packet(packet.getAckNumber(),
                              (packet.getSeqNumber() + 1) % MAX_SEQ,
                              RCVD_WINDOW_SIZE, 
                              1, 0, 0);

            if ((nbytes = sendto(sockfd, ack_packet.encode().c_str(), 
                            Packet::HEADER_LENGTH, 0,
                            (struct sockaddr *)&remaddr, slen)) == -1) {
                logger.logging(ERROR, "sendto error");
                return;
            }
            std::cout << "Sending packet " << ack_packet.getAckNumber() << std::endl;

            // initialize recvbuffer
            recv_buffer.setWindow(RCVD_WINDOW_SIZE); 
            recv_buffer.setCumAck((packet.getSeqNumber() + 1) % MAX_SEQ);
            std::string filename = "received.data";
            recv_buffer.openFile(filename);
            current_seq = INIT_SEQ + 1;
            client_state = ESTABLISHED;   
        }
    }
}


void TCPClient::runningEstablished(int nReadyFds) {
    /* Server behavior in ESTABLISHED state */
    if (nReadyFds == 0) {
        // timeout
        logger.logging(DEBUG, "waiting for data packets");
    } else {
        // we have a packet to receive
        char buf[MAX_BUF_LEN];
        int nbytes = packetReceiver(sockfd, buf, MAX_BUF_LEN);
        if (nbytes == -1) {
            logger.logging(ERROR, "recvfrom error.");
            return;
        }
        /* 0. see if the packet is SYN/ACK, if so we need retransmit 
         *    the first ack.
         * 1. see if the packet is a data packet
         * 1.1 if so, send back the right ack, and mantain recvbuffer
         * 2. if the packet is a FIN
         * 3. send ACK/FIN and change state to LAST_ACK
         **/

        std::string packet_encoded(buf, nbytes);
        Packet packet;
        packet.consume(packet_encoded);
        packet.payload_size = nbytes - HEADER_LENGTH;

        // data packet
        if (!packet.getSyn() && packet.getAck() && !packet.getFin()) {
            std::cout << "Receiving packet " << packet.getSeqNumber() << std::endl;        

            Segment new_seg;
            new_seg.setPacket(packet);
            int sinsert = recv_buffer.insert(new_seg);
            if (sinsert == 0) current_seq = (current_seq + 1) % MAX_SEQ;
            
            Packet ack_packet(current_seq,
                              recv_buffer.getCumAck(),
                              recv_buffer.getWindow(), 
                              1, 0, 0);

            if ((nbytes = sendto(sockfd, ack_packet.encode().c_str(), 
                            Packet::HEADER_LENGTH, 0,
                            (struct sockaddr *)&remaddr, slen)) == -1) {
                logger.logging(ERROR, "sendto error");
                return;
            }
            std::cout << "Sending packet " << ack_packet.getAckNumber();
            if (sinsert != 0) 
                std::cout << " Retransmission";
            std::cout << std::endl;
            return;
        } else if (packet.getFin()) {
            Packet fin_ack_packet(packet.getAckNumber(), 
                                  (packet.getSeqNumber() + 1) % MAX_SEQ,
                                  RCVD_WINDOW_SIZE, 
                                  1, 0, 1);
            if ((nbytes = sendto(sockfd, fin_ack_packet.encode().c_str(), 
                            Packet::HEADER_LENGTH, 0,
                            (struct sockaddr *)&remaddr, slen)) == -1) {
                logger.logging(ERROR, "sendto error");
                return;
            }
            std::cout << "Sending packet " << fin_ack_packet.getAckNumber() 
                      << " FIN" << std::endl;
            current_seq = packet.getAckNumber();
            client_state = LAST_ACK;
        }
    }
}


void TCPClient::runningLastAck(int nReadyFds) {
    /* Server behavior in ESTABLISHED state */
    if (nReadyFds == 0) {
        // timeout
        client_state = CLOSED;
    } else {
        // we have a packet to receive
        char buf[MAX_BUF_LEN];
        int nbytes = packetReceiver(sockfd, buf, MAX_BUF_LEN);
        if (nbytes == -1) {
            logger.logging(ERROR, "recvfrom error.");
            return;
        }
        logger.logging(DEBUG, "got packet from server");

        /*  see if the packet is a Fin-ACK\
         *  if so, go to closed
         **/
        std::string packet_encoded(buf, nbytes);
        Packet packet;
        packet.consume(packet_encoded);
        packet.payload_size = nbytes - HEADER_LENGTH;

        if (packet.getAck()) {
            std::cout << "Receiving packet " << packet.getSeqNumber() << std::endl;        
            client_state = CLOSED;
        }
        recv_buffer.closeFile();
    }
}


int main(int argc, char *argv[]) {

    std::string server_host = "10.0.0.1"; int server_port = 9999;
    if (argc == 3) {
        server_host = argv[1];
        server_port = atoi(argv[2]);
    }
    TCPClient tcp_client(server_host, server_port);
    tcp_client.setupAndRun();
    
}
