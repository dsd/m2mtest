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

#include <fcntl.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "h264parser.h"

int in_fd;
off_t in_size;
unsigned char *in_map;

int m2m_fd;
char *out_buf_map[2];
int out_buf_cnt;
int out_buf_size;

int cap_buf_cnt;
int cap_buf_size[2] = { 2088960, 1044480 }; // FIXME detect this

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
int setup_output(void)
{
	struct v4l2_format fmt = { 0, };
	struct v4l2_requestbuffers reqbuf = { 0, };

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

int setup_capture(void)
{
	struct v4l2_requestbuffers reqbuf = { 0, };
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

void parse(void)
{
	off_t offset = 0;
	int used;
	int fs;
	int ret;
	int i = 0;

	for (i = 0; i < out_buf_cnt; i++) {
		ret = parse_h264_stream(in_map + offset, in_size - offset,
								out_buf_map[i], out_buf_size, &used, &fs, 0);
		if (ret == 0 && offset == in_size) {
			printf("All frames extracted\n");
			break;
		}

		printf("Extracted frame with size %d, queue in outbuf %d\n", fs, i);
		queue_buf(i, fs, 0, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 1);
		offset += used;
	}
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

int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <input file> <m2m device>\n", argv[0]);
		return 1;
	}

	if (input_open(argv[1]) < 0)
		return -1;
	
	if (m2m_open(argv[2]) < 0)
		return -1;

	if (setup_output() < 0)
		return -1;

	map_output();

	if (setup_capture() < 0)
		return -1;

	queue_capture();
	parser_init();
	parse();
	stream(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, VIDIOC_STREAMON);
	stream(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, VIDIOC_STREAMON);

	return 0;
}
