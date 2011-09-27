#include <sys/endian.h>
#define __cpu_to_le32(x) htole32(x)
#define __le32_to_cpu(x) le32toh(x)
