/*
 * V4L2 M2M decoding example application
 *
 * Copyright 2014 Endless Mobile, Inc.
 *
 * Based on Samsung public-apps
 * Copyright 2012 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "h264parser.h"

int in_fd;
off_t in_size;
off_t in_offs = 0;
unsigned char *in_map;

int m2m_fd;
unsigned char *out_buf_map[2];
int out_buf_queued[2];
int out_buf_cnt;
int out_buf_size;

int cap_buf_cnt;
int cap_buf_size[2];
unsigned char *cap_buf_map[10];
unsigned char *cap_buf_map2[10];
uint32_t cap_bytesperline;
int cap_width;
int cap_height;

int input_open(const char *name)
{
	struct stat st;

	in_fd = open(name, O_RDONLY);
	if (in_fd < 0) {
		perror("input open");
		return -1;
	}

	fstat(in_fd, &st);
	in_map = mmap(0, st.st_size, PROT_READ, MAP_SHARED, in_fd, 0);
	if (in_map == MAP_FAILED) {
		perror("input mmap");
	}

	in_size = st.st_size;
	return 0;
}

int m2m_open(const char *name)
{
	struct v4l2_capability cap = { 0, };

	m2m_fd = open(name, O_RDWR, 0);
	if (m2m_fd < 0) {
		perror("m2m open");
		return -1;
	}

	if (ioctl(m2m_fd, VIDIOC_QUERYCAP, &cap) != 0) {
		perror("querycap");
		return -1;
	}

	printf("M2M info: drivers='%s' bus_info='%s' card='%s' capabilities=%x\n",
		   cap.driver, cap.bus_info, cap.card, cap.capabilities);
	return 0;
}

/* "Output" feeds H264 into the m2m device */
int output_set_format(void)
{
	struct v4l2_format fmt = { 0, };

	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
	fmt.fmt.pix_mp.num_planes = 1;

	if (ioctl(m2m_fd, VIDIOC_S_FMT, &fmt) != 0) {
		perror("output S_FMT");
		return -1;
	}

	out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	printf("Output decoding buffer size: %u\n", out_buf_size);
	return 0;
}

int output_request_buffers(void)
{
	struct v4l2_requestbuffers reqbuf = { 0, };

	reqbuf.count = 2;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(m2m_fd, VIDIOC_REQBUFS, &reqbuf) != 0) {
		perror("output REQBUFS");
		return -1;
	}
	out_buf_cnt = reqbuf.count;

	printf("%d output buffers\n", reqbuf.count);
	return 0;
}

void map_output(void)
{
	int i;
	for (i = 0; i < out_buf_cnt; i++) {
		struct v4l2_buffer buf = { 0, };
		struct v4l2_plane planes[1];

		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = 1;

		if (ioctl(m2m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			fprintf(stderr, "querybuf output %d: %m\n", i);
			continue;
		}

		out_buf_map[i] = mmap(NULL, buf.m.planes[0].length,
							  PROT_READ | PROT_WRITE, MAP_SHARED, m2m_fd,
							  buf.m.planes[0].m.mem_offset);
		if (out_buf_map[i] == MAP_FAILED) {
			fprintf(stderr, "map buffer %d: %m\n", i);
			continue;
		}
	}
}

int capture_set_format(void)
{
	struct v4l2_format fmt = { 0, };

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;

	if (ioctl(m2m_fd, VIDIOC_S_FMT, &fmt) != 0) {
		perror("output S_FMT");
		return -1;
	}

	return 0;
}


void map_capture(void)
{
	int i;
	for (i = 0; i < cap_buf_cnt; i++) {
		struct v4l2_buffer buf = { 0, };
		struct v4l2_plane planes[2];

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = 2;

		if (ioctl(m2m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
			fprintf(stderr, "querybuf output %d: %m\n", i);
			continue;
		}

		cap_buf_map[i] = mmap(NULL, buf.m.planes[0].length,
							  PROT_READ | PROT_WRITE, MAP_SHARED, m2m_fd,
							  buf.m.planes[0].m.mem_offset);
		if (cap_buf_map[i] == MAP_FAILED) {
			fprintf(stderr, "map capture buffer %d: %m\n", i);
			continue;
		}
		cap_buf_map2[i] = mmap(NULL, buf.m.planes[1].length,
							  PROT_READ | PROT_WRITE, MAP_SHARED, m2m_fd,
							  buf.m.planes[1].m.mem_offset);
		if (cap_buf_map2[i] == MAP_FAILED) {
			fprintf(stderr, "map capture buffer 2 %d: %m\n", i);
			continue;
		}

	}
}


int setup_capture(void)
{
	struct v4l2_requestbuffers reqbuf = { 0, };
	struct v4l2_format fmt;

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(m2m_fd, VIDIOC_G_FMT, &fmt) != 0) {
		err("Failed to read format (after parsing header)");
		return -1;
	}
	printf("Got format: %dx%d (plane sizes: %d, %d)\n",
		   fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
		   fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
		   fmt.fmt.pix_mp.plane_fmt[1].sizeimage);

	cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	cap_buf_size[1] = fmt.fmt.pix_mp.plane_fmt[1].sizeimage;
	cap_bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	cap_width = fmt.fmt.pix_mp.width;
	cap_height = fmt.fmt.pix_mp.height;

	reqbuf.count = 10;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	reqbuf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(m2m_fd, VIDIOC_REQBUFS, &reqbuf) != 0) {
		perror("capture REQBUFS");
		return -1;
	}
	cap_buf_cnt = reqbuf.count;
	printf("%d capture buffers\n", reqbuf.count);
	return 0;
}

int queue_buf(int index, int l1, int l2, int type, int nplanes)
{
	struct v4l2_buffer qbuf = { 0, };
	struct v4l2_plane planes[2];
	int ret;

	qbuf.type = type;
	qbuf.memory = V4L2_MEMORY_MMAP;
	qbuf.index = index;
	qbuf.m.planes = planes;
	qbuf.length = nplanes;
	qbuf.m.planes[0].bytesused = l1;
	qbuf.m.planes[1].bytesused = l2;
	ret = ioctl(m2m_fd, VIDIOC_QBUF, &qbuf);

	if (ret) {
		fprintf(stderr, "Failed to QBUF type=%d idx=%d: %m\n", type, index);
		return ret;
	}

	printf("Queued buffer %d (type %d)\n", index, type);
	return 0;
}

void queue_capture(void)
{
	int i;
	for (i = 0; i < cap_buf_cnt; i++) {
		queue_buf(i, cap_buf_size[0], cap_buf_size[1],
				  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 2);
	}
}

int dequeue_output(int *n)
{
	struct v4l2_buffer qbuf = { 0, };
	struct v4l2_plane planes[2] = { 0, };

	qbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	qbuf.memory = V4L2_MEMORY_MMAP;
	qbuf.m.planes = planes;
	qbuf.length = 1;

	if (ioctl(m2m_fd, VIDIOC_DQBUF, &qbuf)) {
		fprintf(stderr, "Output dequeue error: %m\n");
		return -1;
	}

	printf("Dequeued output buffer %d\n", qbuf.index);
	*n = qbuf.index;
	return 0;
}

int dequeue_capture(int *n, uint32_t *bytesused)
{
	struct v4l2_buffer qbuf = { 0, };
	struct v4l2_plane planes[2] = { 0, };

	qbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	qbuf.memory = V4L2_MEMORY_MMAP;
	qbuf.m.planes = planes;
	qbuf.length = 2;

	if (ioctl(m2m_fd, VIDIOC_DQBUF, &qbuf)) {
		fprintf(stderr, "Capture dequeue error: %m\n");
		return -1;
	}

	printf("Dequeued capture buffer %d, bytesused %d\n", qbuf.index, qbuf.m.planes[0].bytesused);
	*n = qbuf.index;
	*bytesused = qbuf.m.planes[0].bytesused;
	return 0;
}

int parse_one_nal(void)
{
	int n = 0;
	int ret;
	int used;
	int fs;

	while (n < out_buf_cnt && out_buf_queued[n])
		n++;

	if (n >= out_buf_cnt) {
		printf("All buffers queued, dequeing one\n");
		ret = dequeue_output(&n);
		if (ret)
			return ret;

		out_buf_queued[n] = 0;
	}

	ret = parse_h264_stream(in_map + in_offs, in_size - in_offs,
							out_buf_map[n], out_buf_size, &used, &fs, 0);
	used = fs; /* don't consume the next RBSP stop sequence */
	if (ret == 0 && in_offs == in_size) {
		printf("All frames extracted\n");
		return 1;
	}

	printf("Extracted frame with size %d, queue in outbuf %d\n", fs, n);
	queue_buf(n, fs, 0, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1);
	out_buf_queued[n] = 1;
	in_offs += used;

	return 0;
}

int stop_decoder(void)
{
	struct v4l2_decoder_cmd cmd = { 0, };

	cmd.cmd = V4L2_DEC_CMD_STOP;

	if (ioctl(m2m_fd, VIDIOC_DECODER_CMD, &cmd)) {
		fprintf(stderr, "Decoder command error: %m\n");
		return -1;
	}

	printf("Started decoder drain\n");
	return 0;
}

void *parser_thread_func(void *args)
{
	int ret;
	int i = 0;

	while (1) {
		if (parse_one_nal())
			break;
	}
	stop_decoder();
	printf("parser thread exit\n");
}

int stream(enum v4l2_buf_type type, int status)
{
	if (ioctl(m2m_fd, status, &type) != 0) {
		fprintf(stderr, "Failed to set stream type=%d status=%d: %m\n",
				type, status);
		return -1;
	}

	printf("Stream: set type=%d status=%d\n", type, status);
	return 0;
}

int subscribe_source_change(void)
{
	struct v4l2_event_subscription sub = { 0, };
	sub.type = V4L2_EVENT_SOURCE_CHANGE;

	if (ioctl(m2m_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		fprintf(stderr, "Failed to subscribe to src change: %m\n");
		return -1;
	}

	printf("Subscribed to source change event.\n");
	return 0;
}

int wait_for_source_change(void)
{
	struct v4l2_event ev = { 0, };
	int ret;
	int i;

	printf("Waiting for source change event...\n");
	for (i = 0; i < 5; i++) {
		ret = ioctl(m2m_fd, VIDIOC_DQEVENT, &ev);
		if (ret == 0) {
			printf("Got event type %d.\n", ev.type);
			return 0;
		} else if (ret && errno == ENOENT) {
			printf("No event yet, try again.\n");
		} else if (ret) {
			fprintf(stderr, "DQEVENT error %m\n");
			return -1;
		}
		sleep(1);
	}

	fprintf(stderr, "Didn't receive source change event.\n");
	return -1;

}

void save_image(int n)
{
	char filename[PATH_MAX];
	FILE *fd;
	static int ctr = 0;
	unsigned char *data = cap_buf_map[n];
	uint32_t row, col;

	sprintf(filename, "img%03d.1", ++ctr);

	fd = fopen(filename, "w");
	if (!fd)
		return;

	for (row = 0; row < cap_height; row++) {
		fwrite(data, cap_width, 1, fd);
		data += cap_bytesperline;
	}

	fclose(fd);
	printf("Written to output file %s.\n", filename);

	sprintf(filename, "img%03d.2", ctr);
	fd = fopen(filename, "w");
	if (!fd)
		return;

	data = cap_buf_map2[n];
	for (row = 0; row < (cap_height / 2); row++) {
		fwrite(data, cap_width, 1, fd);
		data += cap_bytesperline;
	}


	fclose(fd);
	printf("Written to output file %s.\n", filename);
}

void capture(void)
{
	while (1) {
		int n;
		uint32_t bytesused;
		if (dequeue_capture(&n, &bytesused))
			return;

		if (bytesused == 0) {
			printf("Capture finished.\n");
			break;
		}

		save_image(n);
		queue_buf(n, cap_buf_size[0], cap_buf_size[1],
				  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 2);
	}
}

int main(int argc, char *argv[])
{
	pthread_t parser_thread;

	if (argc < 3) {
		fprintf(stderr, "Usage: %s <input file> <m2m device>\n", argv[0]);
		return 1;
	}

	if (input_open(argv[1]) < 0)
		return -1;
	
	if (m2m_open(argv[2]) < 0)
		return -1;

	if (subscribe_source_change() < 0)
		return -1;

	/* Setup flow based on ELCE2014 slides and Nicolas Dufresne's knowledge:
	 * S_FMT(OUT)
	 * S_FMT(CAP) to suggest a fourcc for the raw format; may be changed later
	 * G_CTRL(MIN_BUF_FOR_OUTPUT)
	 * REQBUFS(OUT)
	 * QBUF (the header)
	 * STREAMON(OUT)
	 * QBUF/DQBUF frames on OUT
	 * source change event, DQEVENT
	 * G_FMT(CAP)
	 * ENUM_FMT(CAP)
	 * S_FMT(CAP) to set fourcc chosen from ENUM_FMT; also get resolution from returned values?
	 * G_SELECTION to get visible size
	 * G_CTRL(MIN_BUF_FOR_CAPTURE)
	 * REQBUFS(CAP)
	 * STREAMON(CAP)
	 */


	if (output_set_format() < 0)
		return -1;

	if (capture_set_format() < 0)
		return -1;

	// FIXME G_CTRL(MIN_BUF_FOR_OUTPUT)

	if (output_request_buffers() < 0)
		return -1;

	map_output();

	//if (parse_and_queue_header() < 0)
	//	return -1;

	parse_one_nal();
	stream(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMON);

	parser_init();
	if (pthread_create(&parser_thread, NULL, parser_thread_func, NULL)) {
		fprintf(stderr, "Failed to launch parser thread\n");
		return -1;
	}

	if (wait_for_source_change() < 0)
		return -1;

	if (setup_capture() < 0)
		return -1;

	map_capture();
	queue_capture();
	stream(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMON);
	capture();

	pthread_join(parser_thread, 0);
	return 0;
}
