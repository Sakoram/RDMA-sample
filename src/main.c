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

#define TARGET_FPS 1
#define NANOSECONDS_IN_SECOND 1000000000
#define DESIRED_FRAME_DURATION (NANOSECONDS_IN_SECOND / TARGET_FPS)


FILE* yuv_file;
size_t frame_size;
static atomic_bool keep_running = true;
struct timespec start_time, current_time;

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Texture* texture = NULL;

int sdl_init(size_t width, size_t height) {
  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    printf("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    return -1;
  }

  window = SDL_CreateWindow("RDMA Frame Display", SDL_WINDOWPOS_UNDEFINED,
                            SDL_WINDOWPOS_UNDEFINED, 1920, 1080, SDL_WINDOW_SHOWN);
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

int ft_send_frame(struct fid_ep *ep)
{
	int ret;
	fprintf(stdout, "Sending frame...\n");

	if (opts.iface == FI_HMEM_SYSTEM) {
		// snprintf(tx_buf, tx_size, "%s", greeting);
		while (fread(tx_buf, 1, frame_size, yuv_file) != frame_size) {
			if (feof(yuv_file)) {
				/* restart from the beginning if the end of file is reached */
				fseek(yuv_file, 0, SEEK_SET);
				continue;
			} else {
				printf("Failed to read frame from file\n");
				return -1;
			}
		}
	}

	ret = ft_post_tx(ep, remote_fi_addr, frame_size, NO_CQ_DATA, &tx_ctx);
	if (ret)
		return ret;

	ret = ft_get_tx_comp(tx_seq);
	return ret;

	fprintf(stdout, "Send completion received\n");
	return 0;
}

int ft_recv_frame(struct fid_ep *ep)
{
	int ret;
	double elapsed_time;
	int frame_count = 0;
	double fps = 0.0;

	fprintf(stdout, "Waiting for message from client...\n");
	ret = ft_get_rx_comp(rx_seq);
	if (ret)
		return ret;

	// sdl_display_frame(rx_buf, 1920, 1080);

	ret = ft_post_rx(ep, rx_size, &rx_ctx);
	if (ret)
		return ret;

	frame_count++;
	clock_gettime(CLOCK_MONOTONIC, &current_time);
	elapsed_time = current_time.tv_sec - start_time.tv_sec;
	elapsed_time += (current_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

	if (elapsed_time >= 5.0) {
		fps = frame_count / elapsed_time;
		printf("FPS: %.2f\n", fps);
		frame_count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start_time);
	}



	return 0;
}

static void control_fps(struct timespec* start_time) {
  struct timespec end_time;
  long long elapsed_time, time_to_wait;

  clock_gettime(CLOCK_MONOTONIC, &end_time);

  elapsed_time = (end_time.tv_sec - start_time->tv_sec) * NANOSECONDS_IN_SECOND +
                 (end_time.tv_nsec - start_time->tv_nsec);
  time_to_wait = DESIRED_FRAME_DURATION - elapsed_time;

  if (time_to_wait > 0) {
    struct timespec sleep_time;
    sleep_time.tv_sec = time_to_wait / NANOSECONDS_IN_SECOND;
    sleep_time.tv_nsec = time_to_wait % NANOSECONDS_IN_SECOND;
    nanosleep(&sleep_time, NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, start_time);
}


static int run(void)
{
	int ret;

	if (!opts.dst_addr) {
		ret = ft_start_server();
		if (ret)
			return ret;
	}

	ret = opts.dst_addr ? ft_client_connect() : ft_server_connect();
	if (ret) {
		return ret;
	}

	// ret = ft_send_recv_greeting(ep);
	if (opts.dst_addr) {
		yuv_file = fopen("/home/gta/videos/test-yuv420p-8bit.yuv", "rb");
		if (!yuv_file) {
			printf("Failed to open YUV file\n");
			return -1;
		}
		while (keep_running) {
			ft_send_frame(ep);
			control_fps(&start_time);
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


	fi_shutdown(ep, 0);
	return ret;
}

// void int_handler(int dummy) {
//   (void)(dummy);
//   keep_running = false;
// }

int main(int argc, char **argv)
{
	int op, ret;

	// signal(SIGINT, int_handler);

	opts = INIT_OPTS;
	opts.options |= FT_OPT_SIZE;

	frame_size = 1920 * 1080; // * 2; /* UYVY */
	frame_size = frame_size + (frame_size / 2); // SDL_PIXELFORMAT_YV12
	opts.transfer_size = frame_size;




	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "h" ADDR_OPTS INFO_OPTS)) != -1) {
		switch (op) {
		default:
			ft_parse_addr_opts(op, optarg, &opts);
			ft_parseinfo(op, optarg, hints, &opts);
			break;
		case '?':
		case 'h':
			ft_usage(argv[0], "A simple MSG client-sever example.");
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type		= FI_EP_MSG;
	hints->caps			= FI_MSG;
	hints->domain_attr->mr_mode 	= opts.mr_mode;
	hints->addr_format		= opts.address_format;

	clock_gettime(CLOCK_MONOTONIC, &start_time);
	ret = run();
	printf("EXIT\n");

	ft_free_res();
	return ft_exit_code(ret);
}
