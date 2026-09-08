#include "consts.h"
#include "esp_netif.h"

EventGroupHandle_t network_state_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
const int WIFI_FAIL_BIT = BIT1;
const int MQTT_CONNECTED_BIT = BIT2;

static const char *TAG = "consts";

Telemetry* telemetry;
SharedAttributes* shared_attributes;
Attributes* attributes;

void init_consts(void) {
	network_state_event_group = xEventGroupCreate();
	configASSERT(network_state_event_group);
	
	shared_attributes = new SharedAttributes();
	configASSERT(shared_attributes);

	attributes = new Attributes();
	configASSERT(attributes);

	telemetry = new Telemetry();
	configASSERT(telemetry);
}

void getMAC(char* out) {
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


void print_ip_info(const char* tag) {
	esp_netif_t* netif = esp_netif_get_default_netif();
	esp_netif_ip_info_t ip_info;
	esp_netif_get_ip_info(netif, &ip_info);

	esp_netif_dns_info_t dns;
	esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);

	ESP_LOGI(tag, "IP: " IPSTR " | NM: " IPSTR " | GW: " IPSTR " | DNS: " IPSTR,
		IP2STR(&ip_info.ip),
		IP2STR(&ip_info.netmask),
		IP2STR(&ip_info.gw),
		IP2STR(&dns.ip.u_addr.ip4));
}

void dns_debug(const char *host, const char *service)
{
	struct addrinfo hints = {0}, *res = NULL, *p;
	hints.ai_family = AF_UNSPEC;     // AF_INET for IPv4-only
	hints.ai_socktype = SOCK_STREAM; // TCP sockets

	int rc = getaddrinfo(host, service, &hints, &res);
	
	if (rc != 0) {
		ESP_LOGW(TAG, "getaddrinfo(\"%s\", \"%s\") rc=%d", host, service, rc);
		return;
	}
	ESP_LOGI(TAG, "getaddrinfo(\"%s\", \"%s\") rc=%d", host, service, rc);

	for (p = res; p != NULL; p = p->ai_next) {
		char addrstr[INET6_ADDRSTRLEN];
		void *addr = NULL;
		const char *type = "unknown";

		if (p->ai_family == AF_INET) {
			struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
			addr = &(ipv4->sin_addr);
			type = "IPv4";
		}
#if defined(LWIP_IPV6) && LWIP_IPV6
		else if (p->ai_family == AF_INET6) {
			struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
			addr = &(ipv6->sin6_addr);
			type = "IPv6";
		}
#endif

		if (addr) {
			inet_ntop(p->ai_family, addr, addrstr, sizeof(addrstr));
			ESP_LOGI(TAG, "DNS lookup: %s -> %s (socktype=%d proto=%d)", type, addrstr, p->ai_socktype, p->ai_protocol);
		}
	}

	if (res) freeaddrinfo(res);
}

void waitForBit(int bit) {
	xEventGroupWaitBits(network_state_event_group, bit, false, true, portMAX_DELAY);
}
