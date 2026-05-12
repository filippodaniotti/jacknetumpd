#ifndef __UMP_MDNS_H__
#define __UMP_MDNS_H__

void initUMP_mDNS(int localPort, const char* interfaceName, const char* endpointName, const char* hostName);
void SendUMPmDNS(void);
void TerminatemDNS(void);

#endif // __UMP_MDNS_H__
