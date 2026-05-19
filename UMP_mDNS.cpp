/*
 * UMP_mDNS.c
 *
 * dns_sd.h-based replacement for manual mDNS packet construction.
 * Registers a DNS-SD service of type _midi2._udp on all interfaces.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <dns_sd.h>

#ifndef SIOCGIFHWADDR
#define SIOCGIFHWADDR 0x8927
#endif

#include "NetUMP/Network.h"
#include "UMP_mDNS.h"

static char MIDI2ProtocolType[] = "_midi2._udp";
static char EndpointName[256];   // endpoint name, used in TXT and ProductInstanceID prefix
static char HostName[256];       // 

#define PRODUCT_INSTANCE_ID_LEN 17
static char ProductInstanceID[PRODUCT_INSTANCE_ID_LEN + 1];

static DNSServiceRef g_mDNSService = NULL;
static TXTRecordRef g_txtRecord;
static int g_txtRecordValid = 0;

//! Transforms hex digit into ASCII
static unsigned char hex2asc(unsigned char hex)
{
    if (hex < 0x0A) return (unsigned char)(0x30 + hex);   // '0'..'9'
    return (unsigned char)(55 + hex);                     // 'A'..'F'
}

static void DNSSD_API registerCallback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    DNSServiceErrorType errorCode,
    const char *name,
    const char *regtype,
    const char *domain,
    void *context)
{
    (void)sdRef;
    (void)flags;
    (void)context;

    if (errorCode == kDNSServiceErr_NoError)
    {
        fprintf(stdout, "jacknetumpd : DNS-SD registered '%s.%s%s'\n",
                name ? name : "",
                regtype ? regtype : "",
                domain ? domain : "");
    }
    else
    {
        fprintf(stderr, "jacknetumpd : DNS-SD registration failed: %d\n", errorCode);
    }
}

void initUMP_mDNS(int localPort, const char* interfaceName,
                  const char* endpointName)
{
    struct ifreq ifr;
    uint8_t mac_address[6];
    int sockfd = -1;
    uint32_t ifIndex = 0; // default: all interfaces

    // Copy names into static buffers
    strncpy(EndpointName, endpointName ? endpointName : "", sizeof(EndpointName) - 1);
    EndpointName[sizeof(EndpointName) - 1] = '\0';

    gethostname(HostName, sizeof(HostName));

    // Clean up any previous registration
    TerminatemDNS();

    memset(&ifr, 0, sizeof(ifr));
    memset(mac_address, 0, sizeof(mac_address));

    // We still use the interface name only to obtain a stable MAC address
    // for ProductInstanceID generation.
    if (interfaceName && interfaceName[0] != '\0')
    {
    	uint32_t idx = if_nametoindex(interfaceName);
		if (idx == 0)
		{
			fprintf(stderr, "jacknetumpd : unknown interface '%s', falling back to all interfaces\n",
			                    interfaceName);
			            // ifIndex stays 0, mac_address stays all zeros
		}
		else 
		{
			ifIndex = idx;
			sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	        if (sockfd >= 0)
	        {
	            strncpy(ifr.ifr_name, interfaceName, IFNAMSIZ - 1);
	            ifr.ifr_name[IFNAMSIZ - 1] = '\0';
	
	            if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) == 0)
	            {
	                memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
	            }
	            else
	            {
	                fprintf(stderr, "jacknetumpd : SIOCGIFHWADDR failed on %s: %s\n",
	                        interfaceName, strerror(errno));
	            }
	
	            close(sockfd);
	        }
	        else
	        {
	            fprintf(stderr, "jacknetumpd : socket() failed for MAC lookup: %s\n", strerror(errno));
	        }
		}
        
    }

	// Build ProdicInstanceID
    {
        char macHex[13];
        int i;
        int prefixLen = 0;
        int macPos = 0;

        for (i = 0; i < 6; i++)
        {
            macHex[i * 2]     = hex2asc((unsigned char)(mac_address[i] >> 4));
            macHex[i * 2 + 1] = hex2asc((unsigned char)(mac_address[i] & 0x0F));
        }
        macHex[12] = '\0';

        memset(ProductInstanceID, 0, sizeof(ProductInstanceID));

        for (i = 0; endpointName && endpointName[i] != '\0' && prefixLen < PRODUCT_INSTANCE_ID_LEN - 4; i++)
        {
            char c = endpointName[i];
            if (c == ' ') continue;
            ProductInstanceID[prefixLen++] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
        }

        if (prefixLen < PRODUCT_INSTANCE_ID_LEN)
            ProductInstanceID[prefixLen++] = '_';

        while (prefixLen < PRODUCT_INSTANCE_ID_LEN && macPos < 12)
            ProductInstanceID[prefixLen++] = macHex[macPos++];

        ProductInstanceID[PRODUCT_INSTANCE_ID_LEN] = '\0';
    }

    // Build TXT record
    TXTRecordCreate(&g_txtRecord, 0, NULL);
    g_txtRecordValid = 1;

    {
        DNSServiceErrorType err;

        err = TXTRecordSetValue(&g_txtRecord,
                                "UMPEndpointName",
                                (uint8_t)strlen(EndpointName),
                                EndpointName);
        if (err != kDNSServiceErr_NoError)
        {
            fprintf(stderr, "jacknetumpd : TXTRecordSetValue(UMPEndpointName) failed: %d\n", err);
            TerminatemDNS();
            return;
        }

        err = TXTRecordSetValue(&g_txtRecord,
                                "ProductInstanceId",
                                (uint8_t)strlen(ProductInstanceID),
                                ProductInstanceID);
        if (err != kDNSServiceErr_NoError)
        {
            fprintf(stderr, "jacknetumpd : TXTRecordSetValue(ProductInstanceId) failed: %d\n", err);
            TerminatemDNS();
            return;
        }

        // Register on all interfaces:
        // interfaceIndex = 0
        // domain = NULL => default domain(s), typically local.
        // host   = NULL => default host name of this machine.
        err = DNSServiceRegister(&g_mDNSService,
                                 0,                          // flags
                                 ifIndex,                    // all interfaces
                                 ProductInstanceID,          // service instance name
                                 MIDI2ProtocolType,          // "_midi2._udp"
                                 NULL,                       // default domain
                                 NULL,                       // default host
                                 htons((uint16_t)localPort), // network byte order
                                 TXTRecordGetLength(&g_txtRecord),
                                 TXTRecordGetBytesPtr(&g_txtRecord),
                                 registerCallback,
                                 NULL);

        if (err != kDNSServiceErr_NoError)
        {
            fprintf(stderr, "jacknetumpd : DNSServiceRegister failed: %d\n", err);
            TerminatemDNS();
            return;
        }

		fprintf(stdout, "jacknetumpd : DNS-SD registration submitted for '%s.%s' on interface: '%s', host: '%s'\n",
		        ProductInstanceID, MIDI2ProtocolType,
		        (ifIndex == 0) ? "[all]" : interfaceName, HostName );
    }
}

void TerminatemDNS(void)
{
    if (g_mDNSService != NULL)
    {
        DNSServiceRefDeallocate(g_mDNSService);
        g_mDNSService = NULL;
    }

    if (g_txtRecordValid)
    {
        TXTRecordDeallocate(&g_txtRecord);
        g_txtRecordValid = 0;
    }
}

void SendUMPmDNS(void)
{
	// No-op. Let's keep it just for retro-compativility. 
	// We will remove it later.
}
