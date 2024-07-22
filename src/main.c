/*
 * Copyright (c) 2013-2015 Intel Corporation.  All rights reserved.
 * Copyright (c) 2014-2016, Cisco Systems, Inc. All rights reserved.
 *
 * This software is available to you under the BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include <rdma/fi_cm.h>

#include "shared.h"

#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>

#define TARGET_FPS 30
#define NANOSECONDS_IN_SECOND 1000000000
#define DESIRED_FRAME_DURATION (NANOSECONDS_IN_SECOND / TARGET_FPS)


FILE* yuv_file;
size_t frame_size;
static atomic_bool keep_running = true;
struct timespec start_time, current_time;
int frame_count = 0;
static int offset_rma_start = 0;

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;

int sdl_init(size_t width, size_t height) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  window = SDL_CreateWindow("RDMA Frame Display", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 426, 240, SDL_WINDOW_SHOWN);
  if (!window) {
    printf("Window could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  if (!renderer) {
    printf("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
                              width, height);
  if (!texture) {
    printf("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  return 0;
}

void sdl_display_frame(void* frame, size_t width, size_t height) {
  (void)(height);
  SDL_UpdateTexture(texture, NULL, frame,
                    width);  // Assuming UYVY (2 bytes per pixel)
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, NULL, NULL);
  SDL_RenderPresent(renderer);
}

void sdl_cleanup() {
  if (texture) SDL_DestroyTexture(texture);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
}

int ft_send_frame(struct fid_ep *ep, struct fi_rma_iov *remote)
{
	int ret;

	// snprintf(tx_buf + offset_rma_start, tx_size, "%s", greeting);
	while (fread(tx_buf + offset_rma_start, 1, frame_size, yuv_file) != frame_size) {
		if (feof(yuv_file)) {
			/* restart from the beginning if the end of file is reached */
			fseek(yuv_file, 0, SEEK_SET);
			continue;
		} else {
			printf("Failed to read frame from file\n");
			return -1;
		}
	}

	ft_sync();
	fprintf(stdout, "Sending frame...\n");

	ret = ft_post_rma(FT_RMA_WRITEDATA,
			tx_buf + offset_rma_start,
			opts.transfer_size,
			remote,	&tx_ctx);
	if (ret)
		return ret;

	ret = ft_get_tx_comp(tx_seq);
	if (ret)
		return ret;
	// ft_rx(ep, FT_RMA_SYNC_MSG_BYTES); // do I need this? from bw_tx_comp()

	fprintf(stdout, "Send completion received\n");
	return 0;
}

int ft_recv_frame(struct fid_ep *ep)
{
	int ret;
	double elapsed_time;
	double fps = 0.0;

	ft_sync();
	// fprintf(stdout, "Waiting for message from client...\n");


	if (fi->rx_attr->mode & FI_RX_CQ_DATA)
		ret = ft_post_rx(ep, 0, &rx_ctx);
	else
		/* Just increment the seq # instead of
			* posting recv so that we wait for
			* remote write completion on the next
			* iteration */
		rx_seq++;

	/* rx_seq is always one ahead */
	ret = ft_get_rx_comp(rx_seq - 1);
	if (ret)
		return ret;

	sdl_display_frame(rx_buf + offset_rma_start, 1920, 1080);


	frame_count++;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	elapsed_time = current_time.tv_sec - start_time.tv_sec;
	elapsed_time += (current_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

	if (elapsed_time >= 2.0) {
		fps = frame_count / elapsed_time;
		printf("frame_count: %d\n", frame_count);
		printf("FPS: %.2f\n", fps);
		frame_count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}



	return 0;
}

// static void control_fps(struct timespec* start_time) {
//   struct timespec end_time;
//   long long elapsed_time, time_to_wait;

//   clock_gettime(CLOCK_MONOTONIC, &end_time);

//   elapsed_time = (end_time.tv_sec - start_time->tv_sec) * NANOSECONDS_IN_SECOND +
//                  (end_time.tv_nsec - start_time->tv_nsec);
//   time_to_wait = DESIRED_FRAME_DURATION - elapsed_time;

//   if (time_to_wait > 0) {
//     struct timespec sleep_time;
//     sleep_time.tv_sec = time_to_wait / NANOSECONDS_IN_SECOND;
//     sleep_time.tv_nsec = time_to_wait % NANOSECONDS_IN_SECOND;
//     nanosleep(&sleep_time, NULL);
//   }

//   clock_gettime(CLOCK_MONOTONIC, start_time);
// }


static int run(void)
{
	int ret;

	if (hints->ep_attr->type == FI_EP_MSG) {
		if (!opts.dst_addr) {
			ret = ft_start_server();
			if (ret)
				return ret;
		}

		ret = opts.dst_addr ? ft_client_connect() : ft_server_connect();
	} else {
		ret = ft_init_fabric();
	}
	if (ret)
		return ret;

	ret = ft_exchange_keys(&remote);
	if (ret)
		return ret;

	offset_rma_start = FT_RMA_SYNC_MSG_BYTES +
			   MAX(ft_tx_prefix_size(), ft_rx_prefix_size());

	// ret = ft_send_recv_greeting(ep);
	if (opts.dst_addr) {
		yuv_file = fopen("/home/gta/videos/test-yuv420p-8bit.yuv", "rb");
		if (!yuv_file) {
			printf("Failed to open YUV file\n");
			return -1;
		}
		while (keep_running) {
			ret = ft_send_frame(ep, &remote);
			if (ret) {
				return ret;
			}
			// control_fps(&start_time);
		}
	} else {
		if (sdl_init(1920, 1080) != 0) {
			fprintf(stderr, "Failed to initialize SDL.\n");
			return -1;
			}
		while(keep_running) {
			ret = ft_recv_frame(ep);
			if (ret) {
				return ret;
			}
		}
		sdl_cleanup();
	}


	ft_finalize();
	return ret;
}

// void int_handler(int dummy) {
//   (void)(dummy);
//   keep_running = false;
// }




	// signal(SIGINT, int_handler);

int main(int argc, char **argv)
{
	int op, ret;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_BW;

	frame_size = 1920 * 1080; // * 2; /* UYVY */
	frame_size = frame_size + (frame_size / 2); // SDL_PIXELFORMAT_YV12
	opts.transfer_size = frame_size;


	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	opts.rma_op = FT_RMA_WRITEDATA;
	hints->fabric_attr->prov_name = strdup("verbs");
	// hints->ep_attr->type = FI_EP_MSG;  // makes performance worst

	hints->caps = FI_MSG | FI_RMA;
	hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->threading = FI_THREAD_DOMAIN;
	hints->addr_format = opts.address_format;

	while ((op = getopt_long(argc, argv, "Uh" CS_OPTS INFO_OPTS API_OPTS
			    /*BENCHMARK_OPTS*/, long_opts, &lopt_idx)) != -1) {
		switch (op) {
		default:
			if (!ft_parse_long_opts(op, optarg))
				continue;
			// ft_parse_benchmark_opts(op, optarg);
			ft_parseinfo(op, optarg, hints, &opts);
			ft_parsecsopts(op, optarg, &opts);
			ret = ft_parse_api_opts(op, optarg, hints, &opts);
			if (ret)
				return ret;
			break;
		case 'U':
			hints->tx_attr->op_flags |= FI_DELIVERY_COMPLETE;
			break;
		case '?':
		case 'h':
			ft_csusage(argv[0], "Bandwidth test using RMA operations.");
			//ft_benchmark_usage();
			FT_PRINT_OPTS_USAGE("-o <op>", "rma op type: read|write|"
					"writedata (default: write)\n");
			fprintf(stderr, "Note: read/write bw tests are bidirectional.\n"
					"      writedata bw test is unidirectional"
					" from the client side.\n");
			ft_longopts_usage();
			return EXIT_FAILURE;
		}
	}

	/* data validation on read and write ops requires delivery_complete semantics. */
	if (opts.rma_op != FT_RMA_WRITEDATA && ft_check_opts(FT_OPT_VERIFY_DATA))
		hints->tx_attr->op_flags |= FI_DELIVERY_COMPLETE;

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->domain_attr->mr_mode = opts.mr_mode;
	hints->tx_attr->tclass = FI_TC_BULK_DATA;

	ret = run();

	ft_free_res();
	return -ret;
}
