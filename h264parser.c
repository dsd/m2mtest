#include <string.h>
#include <stdio.h>

/* H264 parser states */
enum mfc_h264_parser_state {
	H264_PARSER_NO_CODE,
	H264_PARSER_CODE_0x1,
	H264_PARSER_CODE_0x2,
	H264_PARSER_CODE_0x3,
	H264_PARSER_CODE_1x1,
	H264_PARSER_CODE_SLICE,
};

/* H264 recent tag type */
enum mfc_h264_tag_type {
	H264_TAG_HEAD,
	H264_TAG_SLICE,
};

struct parser_context {
	int state;
	int last_tag;
	char bytes[6];
	int main_count;
	int headers_count;
	int tmp_code_start;
	int code_start;
	int code_end;
	char got_start;
	char got_end;
	char seek_end;
	int short_header;
};

struct parser_context ctx;

void parser_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
}

int parse_h264_stream(
	char* in, int in_size, char* out, int out_size,
	int *consumed, int *frame_size, char get_head)
{
	char *in_orig;
	char tmp;
	char frame_finished;
	int frame_length;

	in_orig = in;

	*consumed = 0;

	frame_finished = 0;

	while (in_size-- > 0) {
		switch (ctx.state) {
		case H264_PARSER_NO_CODE:
			if (*in == 0x0) {
				ctx.state = H264_PARSER_CODE_0x1;
				ctx.tmp_code_start = *consumed;
			}
			break;
		case H264_PARSER_CODE_0x1:
			if (*in == 0x0)
				ctx.state = H264_PARSER_CODE_0x2;
			else
				ctx.state = H264_PARSER_NO_CODE;
			break;
		case H264_PARSER_CODE_0x2:
			if (*in == 0x1) {
				ctx.state = H264_PARSER_CODE_1x1;
			} else if (*in == 0x0) {
				ctx.state = H264_PARSER_CODE_0x3;
			} else {
				ctx.state = H264_PARSER_NO_CODE;
			}
			break;
		case H264_PARSER_CODE_0x3:
			if (*in == 0x1)
				ctx.state = H264_PARSER_CODE_1x1;
			else if (*in == 0x0)
				ctx.tmp_code_start++;
			else
				ctx.state = H264_PARSER_NO_CODE;
			break;
		case H264_PARSER_CODE_1x1:
			tmp = *in & 0x1F;

			if (tmp == 1 || tmp == 5) {
				ctx.state = H264_PARSER_CODE_SLICE;
			} else if (tmp == 6 || tmp == 7 || tmp == 8) {
				ctx.state = H264_PARSER_NO_CODE;
				ctx.last_tag = H264_TAG_HEAD;
				ctx.headers_count++;
			}
			else
				ctx.state = H264_PARSER_NO_CODE;
			break;
		case H264_PARSER_CODE_SLICE:
			if ((*in & 0x80) == 0x80) {
				ctx.main_count++;
				ctx.last_tag = H264_TAG_SLICE;
			}
			ctx.state = H264_PARSER_NO_CODE;
			break;
		}

		if (get_head == 1 && ctx.headers_count >= 1 && ctx.main_count == 1) {
			ctx.code_end = ctx.tmp_code_start;
			ctx.got_end = 1;
			break;
		}

		if (ctx.got_start == 0 && ctx.headers_count == 1 && ctx.main_count == 0) {
			ctx.code_start = ctx.tmp_code_start;
			ctx.got_start = 1;
		}

		if (ctx.got_start == 0 && ctx.headers_count == 0 && ctx.main_count == 1) {
			ctx.code_start = ctx.tmp_code_start;
			ctx.got_start = 1;
			ctx.seek_end = 1;
			ctx.headers_count = 0;
			ctx.main_count = 0;
		}

		if (ctx.seek_end == 0 && ctx.headers_count > 0 && ctx.main_count == 1) {
			ctx.seek_end = 1;
			ctx.headers_count = 0;
			ctx.main_count = 0;
		}

		if (ctx.seek_end == 1 && (ctx.headers_count > 0 || ctx.main_count > 0)) {
			ctx.code_end = ctx.tmp_code_start;
			ctx.got_end = 1;
			if (ctx.headers_count == 0)
				ctx.seek_end = 1;
			else
				ctx.seek_end = 0;
			break;
		}

		in++;
		(*consumed)++;
	}


	*frame_size = 0;

	if (ctx.got_end == 1) {
		frame_length = ctx.code_end;
	} else
		frame_length = *consumed;


	if (ctx.code_start >= 0) {
		frame_length -= ctx.code_start;
		in = in_orig + ctx.code_start;
	} else {
		memcpy(out, ctx.bytes, -ctx.code_start);
		*frame_size += -ctx.code_start;
		out += -ctx.code_start;
		in_size -= -ctx.code_start;
		in = in_orig;
	}

	if (ctx.got_start) {
		if (out_size < frame_length) {
			fprintf(stderr, "Output buffer too small for current frame\n");
			return 0;
		}
		memcpy(out, in, frame_length);
		*frame_size += frame_length;

		if (ctx.got_end) {
			ctx.code_start = ctx.code_end - *consumed;
			ctx.got_start = 1;
			ctx.got_end = 0;
			frame_finished = 1;
			if (ctx.last_tag == H264_TAG_SLICE) {
				ctx.seek_end = 1;
				ctx.main_count = 0;
				ctx.headers_count = 0;
			} else {
				ctx.seek_end = 0;
				ctx.main_count = 0;
				ctx.headers_count = 1;
			}
			memcpy(ctx.bytes, in_orig + ctx.code_end, *consumed - ctx.code_end);
		} else {
			ctx.code_start = 0;
			frame_finished = 0;
		}
	}

	ctx.tmp_code_start -= *consumed;

	return frame_finished;
}
