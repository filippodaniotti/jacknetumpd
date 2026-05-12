/*
 * UMP_mDNS.c
 *
 *  Created on: 2 avr. 2023
 *      Author: Benoit
 */

#include <string.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>
#include "network.h"

typedef struct {
    unsigned short TransactionID;
    unsigned short Flags;
    unsigned short Questions;
    unsigned short AnswerRRs;
    unsigned short AuthorityRRs;
    unsigned short AdditionalRRs;
} TMDNS_Header;

static char MIDI2ProtocolName [] = "_midi2";
static char UDPProtocolName [] = "_udp";
static char LocalDomainName [] = "local";
// static char TargetName [] = "Nexus"; // REMEMBER: no spaces for a correct hostname, otherwise it will not work
// static char EndpointName [] = "Cube";
static char TargetName[256];    // hostname, used in SRV and A records
static char EndpointName[256];  // endpoint name, used in TXT record and ProductInstanceID prefix

#define PRODUCT_INSTANCE_ID_LEN     17
// static char ProductInstanceID [PRODUCT_INSTANCE_ID_LEN+1] = "CUBE_000000000000";
static char ProductInstanceID[PRODUCT_INSTANCE_ID_LEN + 1];

static char ProductInstanceIdTagStr [] = "ProductInstanceId=";
#define PRODUCT_INSTANCEID_TAG_LEN 18       // Length of ProductInstanceIdStr

static unsigned int mDNSPacketLen;
static unsigned char mDNSPacket_ [512];

static TSOCKTYPE mDNSSocket = INVALID_SOCKET;

//! Transforms hex digit into ASCII
static unsigned char hex2asc (unsigned char hex)
{
    if (hex<0x0A) return 0x30+hex;      // Digit 0..9
    else return 55+hex;                 // Letter A..F
}  // hex2asc
// -------------------------------------------------------------

void initUMP_mDNS(int localPort, const char* interfaceName,
                  const char* endpointName, const char* hostName)
{
    unsigned char* mDNSPacket;
    char ProductName[64] = "UMPEndpointName=";
    char* ProductInstanceIDPtr;
    struct ifreq ifr{};
    uint8_t mac_address[6];

    // Copy names into static buffers
    strncpy(EndpointName, endpointName, sizeof(EndpointName) - 1);
    EndpointName[sizeof(EndpointName) - 1] = '\0';
    strncpy(TargetName, hostName, sizeof(TargetName) - 1);
    TargetName[sizeof(TargetName) - 1] = '\0';

    CreateUDPSocket(&mDNSSocket, 0, false);

    // Set outgoing multicast interface
    strcpy(ifr.ifr_name, interfaceName);
    ioctl(mDNSSocket, SIOCGIFINDEX, &ifr);
    struct ip_mreqn mreq{};
    mreq.imr_ifindex = ifr.ifr_ifindex;
    // IP_MULTICAST_IF — which interface to send from
    if (setsockopt(mDNSSocket, IPPROTO_IP, IP_MULTICAST_IF, &mreq, sizeof(mreq)) < 0)
        fprintf(stderr, "mDNS: IP_MULTICAST_IF failed: %s\n", strerror(errno));

    // IP_MULTICAST_TTL — how far the packet can travel
    unsigned char ttl = 255;
    if (setsockopt(mDNSSocket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
        fprintf(stderr, "mDNS: IP_MULTICAST_TTL failed: %s\n", strerror(errno));

    // Get MAC address
    ioctl(mDNSSocket, SIOCGIFHWADDR, &ifr);
    memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);

    // Build ProductInstanceID: endpointName chars (uppercased, no spaces) + MAC suffix
    // filling exactly PRODUCT_INSTANCE_ID_LEN characters.
    // Example: "Zynthian NetUMP" (13 usable chars) → "ZYNTHIANNETUMP_DC" (17 chars)
    // Example: "AB"              (2 usable chars)   → "AB_DCA6322AF15200" (17 chars)

    // Build the MAC hex string (12 chars)
    char macHex[13];
    for (int i = 0; i < 6; i++)
    {
        macHex[i*2]   = hex2asc(mac_address[i] >> 4);
        macHex[i*2+1] = hex2asc(mac_address[i] & 0x0F);
    }
    macHex[12] = '\0';

    // Copy usable chars from endpointName (skip spaces, uppercase)
    memset(ProductInstanceID, 0, sizeof(ProductInstanceID));
    int prefixLen = 0;
    for (int i = 0; endpointName[i] != '\0' && prefixLen < PRODUCT_INSTANCE_ID_LEN - 4; i++) // Let's maintain at least 4 chars for the MAC suffix and separator
    {
        char c = endpointName[i];
        if (c == ' ') continue;
        ProductInstanceID[prefixLen++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }

    // Add separator
    if (prefixLen < PRODUCT_INSTANCE_ID_LEN)
        ProductInstanceID[prefixLen++] = '_';

    // Fill remaining slots with MAC hex digits
    int macPos = 0;
    while (prefixLen < PRODUCT_INSTANCE_ID_LEN && macPos < 12)
        ProductInstanceID[prefixLen++] = macHex[macPos++];

    ProductInstanceID[PRODUCT_INSTANCE_ID_LEN] = '\0';

    // Build TXT UMPEndpointName= field from endpointName
    strcat(&ProductName[0], &EndpointName[0]);  // "UMPEndpointName=Zynthian NetUMP"

    mDNSPacket = &mDNSPacket_[0];
    ProductInstanceIDPtr = (char*)&ProductInstanceID[0];

    // Some debug output to check the generated ProductInstanceID
    fprintf(stdout, "jacknetumpd : ProductInstanceID set to '%s'\n", ProductInstanceID);

    // Build mDNS response packet with 1 PTR record (answer) and 3 additional records (SRV, TXT, A)
    TMDNS_Header* Header = (TMDNS_Header*)mDNSPacket;

    // Compute size of the various strings
    unsigned int MIDI2ProtocolNameLen = strlen (&MIDI2ProtocolName[0]);
    unsigned int UDPProtocolNameLen = strlen (&UDPProtocolName[0]);
    unsigned int LocalDomainNameLen = strlen (&LocalDomainName[0]);
    unsigned int TargetNameLen = strlen (&TargetName[0]);
    unsigned int ProductNameLen = strlen (&ProductName[0]);
    unsigned int BufferPos;

    Header->TransactionID = 0;
    Header->Flags = htons (0x8400);
    Header->Questions = 0;
    // 1 answer (PTR), 3 additional records (SRV, TXT, A)
    Header->AnswerRRs = htons(1);
    Header->AuthorityRRs = 0;
    Header->AdditionalRRs = htons(3);

    // --- PTR record (answer) ---
    // Name: _midi2._udp.local.
    BufferPos = 13;
    mDNSPacket[12] = MIDI2ProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &MIDI2ProtocolName[0], MIDI2ProtocolNameLen);
    BufferPos+=MIDI2ProtocolNameLen;
    mDNSPacket[BufferPos++] = UDPProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &UDPProtocolName[0], UDPProtocolNameLen);
    BufferPos+=UDPProtocolNameLen;
    mDNSPacket[BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket[BufferPos++] = 0;        // NULL terminator

    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x0C;    // Type PTR
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x01;    // Class IN
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x78;    // TTL = 120 seconds
    mDNSPacket [BufferPos++] = 0x00;    // Data length MSB
    mDNSPacket [BufferPos++] = PRODUCT_INSTANCE_ID_LEN+MIDI2ProtocolNameLen+UDPProtocolNameLen+LocalDomainNameLen+4+1;

    // PTR target: ZYV5_xxxx._midi2._udp.local.
    mDNSPacket [BufferPos++] = PRODUCT_INSTANCE_ID_LEN;
    memcpy (&mDNSPacket[BufferPos], ProductInstanceIDPtr, PRODUCT_INSTANCE_ID_LEN);
    BufferPos+=PRODUCT_INSTANCE_ID_LEN;
    mDNSPacket[BufferPos++] = MIDI2ProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &MIDI2ProtocolName[0], MIDI2ProtocolNameLen);
    BufferPos+=MIDI2ProtocolNameLen;
    mDNSPacket[BufferPos++] = UDPProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &UDPProtocolName[0], UDPProtocolNameLen);
    BufferPos+=UDPProtocolNameLen;
    mDNSPacket[BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket[BufferPos++] = 0;        // NULL terminator

    // --- SRV record (additional) ---
    // Name: ZYV5_xxxx._midi2._udp.local.
    mDNSPacket [BufferPos++] = PRODUCT_INSTANCE_ID_LEN;
    memcpy (&mDNSPacket[BufferPos], ProductInstanceIDPtr, PRODUCT_INSTANCE_ID_LEN);
    BufferPos+=PRODUCT_INSTANCE_ID_LEN;
    mDNSPacket[BufferPos++] = MIDI2ProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &MIDI2ProtocolName[0], MIDI2ProtocolNameLen);
    BufferPos+=MIDI2ProtocolNameLen;
    mDNSPacket[BufferPos++] = UDPProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &UDPProtocolName[0], UDPProtocolNameLen);
    BufferPos+=UDPProtocolNameLen;
    mDNSPacket[BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket[BufferPos++] = 0;        // NULL terminator

    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x21;    // Type SRV
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x01;    // Class IN
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x78;    // TTL = 120 seconds
    mDNSPacket [BufferPos++] = 0x00;
    // Data length = 6 (priority, weight, port) + 1 (target name length) + target name + 1 (local domain name length) + local domain name + 1 (NULL terminator)
    mDNSPacket [BufferPos++] = 6 + 1 + TargetNameLen + 1 + LocalDomainNameLen + 1;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;    // Priority
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;    // Weight
    mDNSPacket [BufferPos++] = (localPort >> 8) & 0xFF;
    mDNSPacket [BufferPos++] = localPort & 0xFF;
    // TargetName should end with ".local" for a correct hostname, otherwise it will not work
    mDNSPacket [BufferPos++] = TargetNameLen;
    memcpy (&mDNSPacket[BufferPos], &TargetName[0], TargetNameLen);
    BufferPos+=TargetNameLen;
    mDNSPacket [BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket [BufferPos++] = 0;       // NULL terminator

    // --- TXT record (additional) ---
    // Name: CUBE_xxxx._midi2._udp.local.
    mDNSPacket [BufferPos++] = PRODUCT_INSTANCE_ID_LEN;
    memcpy (&mDNSPacket[BufferPos], ProductInstanceIDPtr, PRODUCT_INSTANCE_ID_LEN);
    BufferPos+=PRODUCT_INSTANCE_ID_LEN;
    mDNSPacket[BufferPos++] = MIDI2ProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &MIDI2ProtocolName[0], MIDI2ProtocolNameLen);
    BufferPos+=MIDI2ProtocolNameLen;
    mDNSPacket[BufferPos++] = UDPProtocolNameLen;
    memcpy (&mDNSPacket[BufferPos], &UDPProtocolName[0], UDPProtocolNameLen);
    BufferPos+=UDPProtocolNameLen;
    mDNSPacket[BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket[BufferPos++] = 0;        // NULL terminator

    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x10;    // Type TXT
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x01;    // Class IN
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x78;    // TTL = 120 seconds
    mDNSPacket [BufferPos++] = 0x00;    // Data length MSB
    mDNSPacket [BufferPos++] = ProductNameLen+1+PRODUCT_INSTANCE_ID_LEN+1+PRODUCT_INSTANCEID_TAG_LEN;
    mDNSPacket [BufferPos++] = ProductNameLen;
    memcpy (&mDNSPacket[BufferPos], &ProductName[0], ProductNameLen);
    BufferPos+=ProductNameLen;
    mDNSPacket [BufferPos++] = PRODUCT_INSTANCE_ID_LEN+PRODUCT_INSTANCEID_TAG_LEN;
    memcpy (&mDNSPacket[BufferPos], &ProductInstanceIdTagStr, PRODUCT_INSTANCEID_TAG_LEN);
    BufferPos+=PRODUCT_INSTANCEID_TAG_LEN;
    memcpy (&mDNSPacket[BufferPos], ProductInstanceIDPtr, PRODUCT_INSTANCE_ID_LEN);
    BufferPos+=PRODUCT_INSTANCE_ID_LEN;

    // --- A record (additional) ---
    mDNSPacket [BufferPos++] = TargetNameLen;
    memcpy (&mDNSPacket[BufferPos], &TargetName[0], TargetNameLen);
    BufferPos+=TargetNameLen;
    mDNSPacket [BufferPos++] = LocalDomainNameLen;
    memcpy (&mDNSPacket[BufferPos], &LocalDomainName[0], LocalDomainNameLen);
    BufferPos+=LocalDomainNameLen;
    mDNSPacket [BufferPos++] = 0x00;    // NULL terminator
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x01;    // Type A
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x01;    // Class IN
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x78;    // TTL = 120 seconds
    mDNSPacket [BufferPos++] = 0x00;
    mDNSPacket [BufferPos++] = 0x04;    // Length = 4 bytes

    // Get system IP address
    strcpy (ifr.ifr_name, interfaceName);
    ifr.ifr_addr.sa_family = AF_INET;
    ioctl (mDNSSocket, SIOCGIFADDR, &ifr);

    struct in_addr in = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;
    memcpy(&mDNSPacket[BufferPos], &in.s_addr, 4);
    BufferPos+=4;

    mDNSPacketLen = BufferPos;
}  // initUMP_mDNS
// -------------------------------------------------------------

void TerminatemDNS (void)
{
    if (mDNSSocket!=INVALID_SOCKET)
    {
        CloseSocket(&mDNSSocket);
        mDNSSocket = INVALID_SOCKET;
    }
}  // TerminatemDNS
// -------------------------------------------------------------

void SendUMPmDNS (void)
{
    sockaddr_in AdrEmit;

    if (mDNSSocket==INVALID_SOCKET) return;

    memset (&AdrEmit, 0, sizeof(sockaddr_in));
    AdrEmit.sin_family=AF_INET;
    AdrEmit.sin_addr.s_addr=htonl(0xE00000FB);  // 224.0.0.251
    AdrEmit.sin_port=htons(5353);
    sendto(mDNSSocket, (const char*)&mDNSPacket_, mDNSPacketLen, 0, (const sockaddr*)&AdrEmit, sizeof(sockaddr_in));
}  // SendUMPmDNS
// -------------------------------------------------------------