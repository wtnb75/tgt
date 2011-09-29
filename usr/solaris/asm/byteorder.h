#include <sys/byteorder.h>
#define __cpu_to_le32(x) htonl(x)
#define __le32_to_cpu(x) ntohl(x)
