/******************************************************************************
Copyright (c) 2011-2012, Roman Arutyunyan (arut@qip.ru)
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, 
      this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, 
      this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT 
SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR 
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING 
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
OF SUCH DAMAGE.
*******************************************************************************/

/* 
    htstress - Fast HTTP Benchmarking tool
*/

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <fcntl.h>
#include <malloc.h>
#include <signal.h>
#include <inttypes.h>


#ifdef _WIN32
	#include <WS2tcpip.h>
	#pragma comment(lib,"WS2_32.lib")
	#define WIN32_LEAN_AND_MEAN
	#pragma warning(disable:4996)
	#include "wepoll.h"
	#include "getopt.h"
	#define ThreadEntry WINAPI
#else
	#include <netdb.h>
	#include <pthread.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <netinet/ip.h>
	#include <sys/time.h>
	#include <getopt.h>
	#include <sys/epoll.h>
	#define closesocket close
	#define SOCKET int
	#define ThreadEntry
#endif

//#include <stdatomic.h>

#define HTTP_REQUEST_PREFIX "http://"

#define HTTP_REQUEST_FMT "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n"

#define HTTP_REQUEST_DEBUG  0x01
#define HTTP_RESPONSE_DEBUG 0x02

#define INBUFSIZE 1024

#define BAD_REQUEST 0x1

#define MAX_EVENTS 256

struct econn {
	SOCKET fd;
	size_t offs;
	int flags;
};

char *outbuf;
size_t outbufsize;

struct sockaddr_in ssin;

int concurrency = 2;
int num_threads = 1;

volatile uint64_t num_requests = 0;
volatile uint64_t max_requests = 0;
volatile uint64_t good_requests = 0;
volatile uint64_t bad_requests = 0;
volatile uint64_t in_bytes = 0;
volatile uint64_t out_bytes = 0;

uint64_t ticks;

int debug = 0;

struct timeval tv, tve;

static const char short_options[] = "n:c:t:d";

static const struct option long_options[] = {
	{ "number",       1, NULL, 'n' },
	{ "concurrency",  1, NULL, 'c' },
	{ "threads",      0, NULL, 't' },
	{ "debug",        0, NULL, 'd' },
	{ "help",         0, NULL, '%' },
	{ NULL, 0, NULL, 0 }
};

static void sigint_handler(int arg)
{
	max_requests = num_requests;
}

static void start_time()
{
	if (gettimeofday(&tv, NULL)) {
		perror("gettimeofday");
		exit(1);
	}
}

static void end_time()
{
	if (gettimeofday(&tve, NULL)) {
		perror("gettimeofday");
		exit(1);
	}
}

static void init_conn(int efd, struct econn* ec) {

	struct epoll_event evt;
	int ret;

	ec->fd = socket(AF_INET, SOCK_STREAM, 0);
	ec->offs = 0;
	ec->flags = 0;

	if (ec->fd == -1) {
		perror("socket() failed");
		exit(1);
	}

#ifdef _WIN32
	//u_long iMode = 1;
	//int iResult = ioctlsocket(ec->fd, FIONBIO, &iMode);
	//if (iResult != NO_ERROR)
	//	printf("ioctlsocket failed with error: %ld\n", iResult);
#else
	fcntl(ec->fd, F_SETFL, O_NONBLOCK);
#endif

	ret = connect(ec->fd, (struct sockaddr*)&ssin, sizeof(ssin));

	if (ret && errno != EINPROGRESS) {
#ifdef _WIN32
		ret = WSAGetLastError();
		printf("connect() failed: %d\n", ret);
#else
		perror("connect() failed");
#endif
		exit(1);
	}

	evt.events = EPOLLOUT;
	evt.data.ptr = ec;

	if (epoll_ctl(efd, EPOLL_CTL_ADD, ec->fd, &evt)) {
		perror("epoll_ctl");
		exit(1);
	}
}

static void* ThreadEntry worker(void* arg) 
{
	int efd, fd, ret, nevts, n, m;
	struct epoll_event evts[MAX_EVENTS];
	char inbuf[INBUFSIZE], c;
	struct econn *ecs=malloc(concurrency*sizeof(struct econn)), * ec;

	efd = epoll_create(concurrency);

	if (efd == -1) {
		perror("epoll");
		exit(1);
	}

	for(n = 0; n < concurrency; ++n)
		init_conn(efd, ecs + n);

	for(;;) {

		nevts = epoll_wait(efd, evts, sizeof(evts) / sizeof(evts[0]), -1);

		if (nevts == -1) {
			perror("epoll_wait");
			exit(1);
		}

		for(n = 0; n < nevts; ++n) {

			ec = (struct econn*)evts[n].data.ptr;

			if (ec == NULL) {
				fprintf(stderr, "fatal: NULL econn\n");
				exit(1);
			}

			if (evts[n].events & (EPOLLHUP | EPOLLERR)) {
				/* normally this should not happen */
				fprintf(stderr, "broken connection");
				exit(1);
			}

			if (evts[n].events & EPOLLOUT) {

				ret = send(ec->fd, outbuf + ec->offs, outbufsize - ec->offs, 0);

				if (ret == -1 && errno != EAGAIN) {
					/* TODO: something better than this */
					perror("send");
					exit(1);
				}

				if (ret > 0) {

					if (debug & HTTP_REQUEST_DEBUG)
						write(2, outbuf+ ec->offs, outbufsize - ec->offs);

					ec->offs += ret;

					/* write done? schedule read */
					if (ec->offs == outbufsize) {

						evts[n].events = EPOLLIN;
						evts[n].data.ptr = ec;

						ec->offs = 0;

						if (epoll_ctl(efd, EPOLL_CTL_MOD, ec->fd, evts + n)) {
							perror("epoll_ctl");
							exit(1);
						}
					}
				}

			} else if (evts[n].events & EPOLLIN) {

				for(;;) {

					ret = recv(ec->fd, inbuf, sizeof(inbuf), 0);

					if (ret == -1 && errno != EAGAIN) {
						perror("recv");
						exit(1);
					}

					if (ret <= 0)
						break;

					if (ec->offs <= 9 && ec->offs + ret > 10) {

						c = inbuf[9 - ec->offs];

						if (c == '4' || c == '5')
							ec->flags |= BAD_REQUEST;
					}

					if (debug & HTTP_RESPONSE_DEBUG)
						write(2, inbuf, ret);

					ec->offs += ret;

				}

				if (!ret) {

					if (epoll_ctl(efd, EPOLL_CTL_DEL, ec->fd, evts + n)) {
						perror("epoll_ctl");
						exit(1);
					} closesocket(ec->fd);

					//m = __sync_fetch_and_add(&num_requests, 1);
					m = num_requests;
					num_requests++;

					if (max_requests && m + 1 > max_requests)
						//__sync_fetch_and_sub(&num_requests, 1);
						num_requests--;

					else if (ec->flags & BAD_REQUEST)
						//__sync_fetch_and_add(&bad_requests, 1);
						bad_requests++;

					else
						//__sync_fetch_and_add(&good_requests, 1);
						good_requests++;

					if (max_requests && m + 1 >= max_requests) {
						end_time();
						return NULL;
					}

					if (ticks && m % ticks == 0)
						printf("%d requests\n", m);

					init_conn(efd, ec);
				}
			}
		}
	}
}

static void print_usage() 
{
	printf("Usage: htstress [options] [http://]hostname[:port]/path\n"
			"Options:\n"
			"   -n, total number of requests (0 for inifinite, Ctrl-C to abort)\n"
			"   -c, number of concurrent connections\n"
			"   -t, number of threads (set this to the number of CPU cores)\n"
			"   -d, debug HTTP response\n"
			"   -h, display this message\n"
		  );
	exit(0);
}

int main(int argc, char* argv[])  {
	char *rq, *s=NULL;
	double delta, rps;
	int next_option;
	int n;
#ifdef _WIN32
	HANDLE useless_thread;
#else
	pthread_t useless_thread;
#endif
	int port = 80;
	char *host = NULL;
	struct hostent *h;

	if (argc == 1)
		print_usage();

	//do {
	//	next_option = getopt_long(argc-1, argv+1, short_options, long_options, NULL);
	//
	//	switch (next_option) {
	//
	//		case 'n':
	//			max_requests = strtoull(optarg, 0, 10);
	//			break;
	//
	//		case 'c':
	//			concurrency = atoi(optarg);
	//			break;
	//
	//		case 't':
	//			num_threads = atoi(optarg);
	//			break;
	//
	//		case 'd':
	//			debug = 0x03;
	//			break;
	//
	//		case '%':
	//			print_usage();
	//
	//		case -1:
	//			break;
	//
	//		default:
	//			printf("Unexpected argument: '%c'\n", next_option);
	//			return 1;
	//	}
	//} while (next_option != -1);

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] != '-') { // Treat as end of options (so, the URL)
			s = argv[i]; break;
		}

		switch (argv[i][1]) {
			case 'n':
				max_requests = strtoull(argv[i+1], 0, 10);
				i++; break;
			
			case 'c':
				concurrency = atoi(argv[i + 1]);
				i++; break;
			
			case 't':
				num_threads = atoi(argv[i + 1]);
				i++; break;
			
			case 'd':
				debug = 0x03;
				break;

			case 'h':
			case '?':
				print_usage(); return 0; break;
		}
	}

	if (!s) {
		printf("Missing URL\n");
		return 1;
	}

	if (!strncmp(s, HTTP_REQUEST_PREFIX, sizeof(HTTP_REQUEST_PREFIX) - 1))
		s += (sizeof(HTTP_REQUEST_PREFIX) - 1);

	host = s;

	rq = strpbrk(s, ":/");

	if (rq == NULL)
		rq = "/";

	else if (*rq == '/') {
		host = malloc(rq - s);
		memcpy(host, rq, rq - s);

	} else if (*rq == ':') {
		*rq++ = 0;
		port = atoi(rq);
		rq = strchr(rq, '/');
		if (rq == NULL)
			rq = "/";
	}

#ifdef _WIN32
	WSADATA wd;
	if (WSAStartup(MAKEWORD(2, 2), &wd) != NO_ERROR) {
		wprintf(L"Error at WSAStartup()\n");
		return 1;
	}
#endif

	int ret = inet_pton(AF_INET, host, &ssin.sin_addr);
	if (ret == -1) {
		h = gethostbyname(host);
		if (!h || !h->h_length) {
			printf("gethostbyname failed\n");
			return 1;
		}

		ssin.sin_addr.s_addr = *(unsigned int*)h->h_addr;
	}

	
	ssin.sin_family = PF_INET;
	ssin.sin_port = htons(port);

	/* prepare request buffer */
	outbuf = malloc(strlen(rq) + sizeof(HTTP_REQUEST_FMT) + strlen(host));
	outbufsize = sprintf(outbuf, HTTP_REQUEST_FMT, rq, host);

	ticks = max_requests / 10;

	signal(SIGINT, &sigint_handler);

	if (!max_requests) {
		ticks = 1000;
		printf("[Press Ctrl-C to finish]\n");
	}

	start_time();

	/* run test */
	for (n = 0; n < num_threads - 1; ++n)
#ifdef _WIN32
		useless_thread = CreateThread(NULL, NULL, worker, 0, 0, 0);
#else
		pthread_create(&useless_thread, 0, &worker, 0);
#endif

	worker(0);

	/* output result */
	delta = tve.tv_sec - tv.tv_sec + ((double)(tve.tv_usec - tv.tv_usec)) / 1e6;

	printf("\n"
			"requests:      %"PRIu64"\n"
			"good requests: %"PRIu64" [%d%%]\n"
			"bad requests:  %"PRIu64" [%d%%]\n"
			"seconds:       %.3f\n"
			"requests/sec:  %.3f\n"
			"\n",
			num_requests,
			good_requests, (int)(num_requests ? good_requests * 100 / num_requests : 0),
			bad_requests, (int)(num_requests ? bad_requests * 100 / num_requests: 0),
			delta,
			delta > 0
				? max_requests / delta
				: 0
		  );

	return 0;
}

