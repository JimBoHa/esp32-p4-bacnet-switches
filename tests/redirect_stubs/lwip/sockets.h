#include <sys/socket.h>
#include <netinet/in.h>
int dashboard_test_getsockname(int, struct sockaddr *, socklen_t *);
#define getsockname dashboard_test_getsockname
