void parser_init(void);
int parse_h264_stream(
        char* in, int in_size, char* out, int out_size,
        int *consumed, int *frame_size, char get_head);
