/**
 * Xine input plugin for p2p video streams
 * 
 * parts of the code from original xine input plugins:
 *  - input_http: mrl handling
 *  - input_stdin_fifo: reading from stdin stuff
 *  - input_rtp: circular buffer stuff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define LOG
#define LOG_MODULE "input_p2p"
#define LOG_VERBOSE

#include <xine/xine_internal.h>
#include <xine/xineutils.h>
#include <xine/input_plugin.h>

#include <net_helper.h>
#include <chunk.h>
#include <chunkiser.h>

#define BUFFER_SIZE (1024*1024)
#define DEFAULT_HOST_PORT 55555
#define OWN_PORT 44444

typedef struct {
    input_plugin_t input_plugin;

    xine_stream_t *stream;

    char *interface;
    int own_port;
    char *host;
    int host_port;

    int fh;
    char *mrl;
    off_t curpos;

    uint8_t preview[MAX_PREVIEW_SIZE];
    off_t preview_size;
    int preview_read_done; /* boolean true after attempt to read input stream for preview */

    uint8_t *buffer; /* circular buffer */
    uint8_t *buffer_get_ptr; /* get pointer used by the reader */
    uint8_t *buffer_put_ptr; /* put pointer used by the writer */
    long buffer_count; /* number of bytes in the buffer */
    long buffer_max_size;


    pthread_mutex_t buffer_ring_mutex;
    pthread_cond_t writer_cond;
    pthread_cond_t reader_cond;

    // streamer data
    int period, done, id;
    struct input_stream *input;
    const int *in_fds;
    pthread_t inputThread;

    // for testing:
    int sizeChunkData;

} p2p_input_plugin_t;

typedef struct {
    input_class_t input_class;

    xine_t *xine;
    config_values_t *config;

} p2p_input_class_t;

/*
 * helper functions START
 * 
 * from GRAPES/src/Chunkizer/payload.h
 */
static inline uint32_t int_rcpy(const uint8_t *p) {
    uint32_t tmp;

    memcpy(&tmp, p, 4);
    tmp = ntohl(tmp);

    return tmp;
}

static inline uint16_t int16_rcpy(const uint8_t *p) {
    uint16_t tmp;

    memcpy(&tmp, p, 2);
    tmp = ntohs(tmp);
    return tmp;
}
// helper function END

static int streamer_init(p2p_input_plugin_t *this) {
    // from GRAPES: src/Test/chunkiser_test.c
    this->input = input_stream_open("movie5.mpg", &this->period, "chunkiser=avf,media=av"); // hardcoded filename
    if (this->input == NULL) {
        fprintf(stderr, "Cannot open input %s\n", "movie5.mpg");
        return -1;
    }
    if (this->period == 0) {
        this->in_fds = input_get_fds(this->input);
    } else {
        this->in_fds = NULL;
    }

    return 0;
}

// from GRAPES: src/Test/chunkiser_test.c

static void streamer_in_wait(const int *fd) {
    int my_fd[10];
    int i = 0;

    if (fd == NULL) {
        return;
    }
    while (fd[i] != -1) {
        my_fd[i] = fd[i];
        i++;
    }
    my_fd[i] = -1;

    wait4data(NULL, NULL, my_fd);
}

//parts from GRAPES: src/Test/chunkiser_test.c

static void streamer_write_chunk(p2p_input_plugin_t *this, struct chunk *c) {
    int header_size;
    int frames;
    int i;
    uint8_t codec;
    int offset;

    if (c->data[0] == 0) {
        fprintf(stderr, "Error! Strange chunk: %x!!!\n", codec);
        return;
    } else if (c->data[0] < 127) {
        int width, height, frame_rate_n, frame_rate_d;

        header_size = 1 + 2 + 2 + 2 + 2 + 1; // 1 Frame type + 2 width + 2 height + 2 frame rate num + 2 frame rate den + 1 number of frames
        //video_payload_header_parse(c->data, &codec, &width, &height, &frame_rate_n, &frame_rate_d);
        codec = c->data[0];
        width = int16_rcpy(c->data + 1);
        height = int16_rcpy(c->data + 3);
        frame_rate_n = int16_rcpy(c->data + 5);
        frame_rate_d = int16_rcpy(c->data + 7);
#ifdef DEBUG
        fprintf(stderr, "Chunk[%05d]: Frame size: %dx%d -- Frame rate: %d / %d\n", c->id, width, height, frame_rate_n, frame_rate_d);
#endif
    }

    frames = c->data[header_size - 1];
    for (i = 0; i < frames; i++) {
        int frame_size;
        int64_t pts, dts;

        //frame_header_parse(data, &frame_size, &pts, &dts);
        int i;

        frame_size = 0;
        /* FIXME: Maybe this can use int_coding? */

        for (i = 0; i < 3; i++) {
            frame_size = frame_size << 8;
            frame_size |= c->data[i];
        }
        dts = int_rcpy(c->data + 3);
        if (c->data[7] != 255) {
            pts = dts + c->data[7];
        } else {
            pts = -1;
        }
        //      dprintf("Frame %d has size %d\n", i, frame_size);
    }
    offset = header_size + frames * (3 + 4 + 1); // 3 Frame size + 4 PTS + 1 DeltaTS

    pthread_mutex_lock(&this->buffer_ring_mutex);

    // copy data to ring buffer
    /* wait for enough space to write the whole of the recv'ed data */
    while ((this->buffer_max_size - this->buffer_count) < c->size) {

        printf("%s: there is not enough space in buffer for current chunk (free: %d | needed: %d)\n", LOG_MODULE, (this->buffer_max_size - this->buffer_count), c->size);

        struct timeval tv;
        struct timespec timeout;

        gettimeofday(&tv, NULL);

        timeout.tv_nsec = tv.tv_usec * 1000;
        timeout.tv_sec = tv.tv_sec + 2;

        if (pthread_cond_timedwait(&this->writer_cond, &this->buffer_ring_mutex, &timeout) != 0) {
            fprintf(stdout, "input_p2p: buffer ring not read within 2 seconds!\n");
        }
    }

    /* Now there's enough space to write some bytes into the buffer
     * determine how many bytes can be written. If the buffer wraps
     * around, write in two pieces: from the head pointer to the
     * end of the buffer and from the base to the remaining number
     * of bytes.
     */

    long buffer_space_remaining = BUFFER_SIZE - (this->buffer_put_ptr - this->buffer);

    if (buffer_space_remaining >= c->size) {
        /* data fits inside the buffer */
        memcpy(this->buffer_put_ptr, c->data, c->size);
        this->buffer_put_ptr += c->size;
    } else {
        /* data wrapped around the end of the buffer */
        memcpy(this->buffer_put_ptr, c->data, buffer_space_remaining);
        memcpy(this->buffer, &c->data[buffer_space_remaining], c->size - buffer_space_remaining);
        this->buffer_put_ptr = &this->buffer[ c->size - buffer_space_remaining ];
    }

    this->buffer_count += c->size;

    /* signal the reader that there is new data	*/
    pthread_cond_signal(&this->reader_cond);
    pthread_mutex_unlock(&this->buffer_ring_mutex);
}

static void *streamer_thread(void *mut) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) mut;

    int done = 0;
    int id = 0;

    while (!done) {
        streamer_in_wait(this->in_fds);

        int res;

        struct chunk *c;
        c = (struct chunk *) malloc(sizeof (struct chunk));
        c->id = id++;
        //c->data = NULL;
        c->attributes = NULL;
        c->attributes_size = 0;
        res = chunkise(this->input, c);
        if (res > 0) {
            this->sizeChunkData += c->size;
            printf("%s: chunk[%d]: size: %d / timestamp: %llu\n", LOG_MODULE, c->id, c->size, c->timestamp);
            streamer_write_chunk(this, c);
        } else if (res < 0) {
            done = 1;
        }

        // cleanup chunk
        free(c->data);
        c->data = NULL;
        free(c);
        c = NULL;

        usleep((useconds_t) this->period);
    }

    return NULL;
}

/**
 * This function frees the memory used by the input plugin class object.
 * 
 * @param this_gen
 */
static void p2p_class_dispose(input_class_t *this_gen) {
    p2p_input_class_t *this = (p2p_input_class_t *) this_gen;
    // frees all memory used by the class
    free(this);
    lprintf("P2P Class disposed...\n");
}

/**
 * You should do any device-specific initialisation within this function.
 * 
 * @param this_gen
 * @return 
 */
static int p2p_plugin_open(input_plugin_t *this_gen) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;
    p2p_input_class_t *this_class = (p2p_input_class_t *) this->input_plugin.input_class;

    // for the streamer
    //    printf("%s: interface=%s\n", LOG_MODULE, this->interface);
    //    printf("%s: server address=%s\n", LOG_MODULE, this->host);
    //    printf("%s: server port=%d\n", LOG_MODULE, this->host_port);
    //    printf("%s: own port=%d\n", LOG_MODULE, this->own_port);
    //
    //    this->fh = init(this);
    //    if (this->fh == -1) {
    //        return 0;
    //    }
    //
    //    // start streamer threads
    //    threads_start(this);

    // for testing with local chunk creation
    this->fh = streamer_init(this);
    if (this->fh == -1) {
        return 0;
    }

    pthread_create(&this->inputThread, NULL, streamer_thread, (void *) this);

    return 1;
}

/**
 * Returns a bit mask describing the input device's capabilities. You may logically
 * OR the INPUT_CAP_* constants together to get a suitable bit-mask (via the '|' operator).
 * 
 * @param this_gen
 * @return 
 */
static uint32_t p2p_plugin_get_capabilities(input_plugin_t *this_gen) {
    return INPUT_CAP_PREVIEW;
}

/**
 * Reads a specified number of bytes into a buffer and returns the number of bytes
 * actually copied.
 * 
 * @param this_gen
 * @param buf
 * @param nlen
 * @return 
 */
static off_t p2p_plugin_read(input_plugin_t *this_gen, char *buf_gen, off_t nlen) {
    printf("%s: called p2p_plugin_read\n", LOG_MODULE);

    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;
    uint8_t *buf = (uint8_t *) buf_gen;
    struct timeval tv;
    struct timespec timeout;
    off_t copied = 0;

    if (nlen < 0)
        return -1;

    if (this->curpos < this->preview_size) {
        off_t n;
        n = this->preview_size - this->curpos;
        if (n > (nlen - copied))
            n = nlen - copied;
        lprintf("%"PRId64" bytes from preview (which has %"PRId64" bytes)\n", n, this->preview_size);

        memcpy(&buf[copied], &this->preview[this->curpos], n);
        this->curpos += n;
        copied += n;
        nlen -= n;
    }

    while (nlen > 0) {

        off_t n;

        pthread_mutex_lock(&this->buffer_ring_mutex);

        /*
         * if nothing in the buffer, wait for data for 5 seconds. If
         * no data is received within this timeout, return the number
         * of bytes already received (which is likely to be 0)
         */

        if (this->buffer_count == 0) {
            gettimeofday(&tv, NULL);
            timeout.tv_nsec = tv.tv_usec * 1000;
            timeout.tv_sec = tv.tv_sec + 5;

            if (pthread_cond_timedwait(&this->reader_cond, &this->buffer_ring_mutex, &timeout) != 0) {
                /* we timed out, no data available */
                printf("%s: we timed out, no data available...\n", LOG_MODULE);
                pthread_mutex_unlock(&this->buffer_ring_mutex);
                return copied;
            }
        }

        /* Now determine how many bytes can be read. If the buffer
         * will wrap the buffer is read in two pieces, first read
         * to the end of the buffer, wrap the tail pointer and
         * update the buffer count. Finally read the second piece
         * from the base to the remaining count
         */
        if (nlen > this->buffer_count) {
            n = this->buffer_count;
        } else {
            n = nlen;
        }

        if (((this->buffer_get_ptr - this->buffer) + n) > BUFFER_SIZE) {
            n = BUFFER_SIZE - (this->buffer_get_ptr - this->buffer);
        }

        /* the actual read */
        memcpy(buf, this->buffer_get_ptr, n);

        buf += n;
        copied += n;
        nlen -= n;

        /* update the tail pointer, watch for wrap arounds */
        this->buffer_get_ptr += n;
        if (this->buffer_get_ptr - this->buffer >= BUFFER_SIZE)
            this->buffer_get_ptr = this->buffer;

        this->buffer_count -= n;

        /* signal the writer that there's space in the buffer again */
        pthread_cond_signal(&this->writer_cond);
        pthread_mutex_unlock(&this->buffer_ring_mutex);

        //printf("%s: data read (%d bytes) | buffer_count: %d\n", LOG_MODULE, n, this->buffer_count);
        lprintf("got %"PRId64" bytes (%"PRId64"/%"PRId64" bytes read)\n", n, copied, nlen);
    }

    this->curpos += copied;

    return copied;
}

/**
 * Should the input plugin set the block-oriented hint and if the demuxer supports
 * it, this function will be called to read a block directly into a xine buffer
 * from the buffer pool.
 * 
 * @param this_gen
 * @param fifo
 * @param len
 * @return 
 */
static buf_element_t *p2p_plugin_read_block(input_plugin_t *this_gen, fifo_buffer_t *fifo, off_t len) {
    buf_element_t *buf = fifo->buffer_pool_alloc(fifo);
    int total_bytes;

    if (len > buf->max_size)
        len = buf->max_size;
    if (len < 0) {
        buf->free_buffer(buf);
        return NULL;
    }

    buf->content = buf->mem;
    buf->type = BUF_DEMUX_BLOCK;

    total_bytes = p2p_plugin_read(this_gen, buf->content, len);

    if (total_bytes != len) {
        buf->free_buffer(buf);
        return NULL;
    }

    buf->size = total_bytes;

    return buf;
}

/**
 * This function is called by xine when it is required that subsequent reads come
 * from another part of the stream.
 * 
 * @param this_gen
 * @param offset
 * @param origin
 * @return 
 */
static off_t p2p_plugin_seek(input_plugin_t *this_gen, off_t offset, int origin) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;

    return -1;
}

/**
 * Returns the current position within a finite length stream.
 * 
 * @param this_gen
 * @return 
 */
static off_t p2p_plugin_get_current_pos(input_plugin_t *this_gen) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;

    return this->curpos;
}

/**
 * Similarly this function returns the length of the stream.
 * 
 * @param this_gen
 * @return 
 */
static off_t p2p_plugin_get_length(input_plugin_t *this_gen) {
    return -1;
}

/**
 * Returns the device's prefered block-size if applicable.
 * 
 * @param this_gen
 * @return 
 */
static uint32_t p2p_plugin_get_blocksize(input_plugin_t *this_gen) {
    return 0;
}

/**
 * Returns the current MRL.
 * 
 * @param this_gen
 * @return 
 */
static char *p2p_plugin_get_mrl(input_plugin_t *this_gen) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;

    return this->mrl;
}

/**
 * This function allows the input to advertise extra information that is not
 * available through other API functions. See INPUT_OPTIONAL_* defines.
 * 
 * @param this_gen
 * @param data
 * @param data_type
 * @return 
 */
static int p2p_plugin_get_optional_data(input_plugin_t *this_gen, void *data, int data_type) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;

    /* Since this input plugin deals with stream data, we
     * are not going to worry about retaining the data packet
     * retrieved for review purposes. Hence, the first non-preview
     * packet read made will return the 2nd packet from the UDP/RTP stream.
     * The first packet is only used for the preview.
     */

    if (data_type == INPUT_OPTIONAL_DATA_PREVIEW) {
        if (!this->preview_read_done) {
            this->preview_size = p2p_plugin_read((input_plugin_t*) this, this->preview, MAX_PREVIEW_SIZE);
            if (this->preview_size < 0)
                this->preview_size = 0;
            lprintf("Preview data length = %d\n", this->preview_size);

            this->preview_read_done = 1;
        }
        if (this->preview_size)
            memcpy(data, this->preview, this->preview_size);
        return this->preview_size;
    } else {
        return INPUT_OPTIONAL_UNSUPPORTED;
    }
}

/**
 * This function closes all resources and frees the input_plugin_t object.
 * 
 * @param this_gen
 */
void p2p_plugin_dispose(input_plugin_t *this_gen) {
    p2p_input_plugin_t *this = (p2p_input_plugin_t *) this_gen;

    // free everythink in this
    free(this->mrl);
    free(this->host);
    free(this->interface);
    free(this->buffer);
    free(this->preview);
    // ...
    
    // close input-stream
    input_stream_close(this->input);
    
    free(this);
    lprintf("P2P Plugin disposed...\n");
}

/**
 * The plugin should try, if it can handle the specified MRL and return an instance
 * of itself if so. If not, NULL should be returned. When a new MRL is to be played,
 * xine engine asks all the available input plugins one by one if they can handle
 * the MRL. Note that input plugins are not guaranteed to be queried in any particular
 * order and the first input plugin to claim an MRL gets control so try not to
 * duplicate MRLs already found within xine.
 * 
 * @param class_gen
 * @param stream
 * @param mrl
 * @return 
 */
static input_plugin_t *p2p_class_get_instance(input_class_t *class_gen, xine_stream_t *stream, const char *mrl) {
    printf("%s: p2p_class_get_instance (START)\n", LOG_MODULE);

    p2p_input_class_t *class = (p2p_input_class_t *) class_gen;
    p2p_input_plugin_t *this;

    printf("%s: check if protocol is supported\n", LOG_MODULE);

    if (strncasecmp(mrl, "p2p://", 6) != 0) {
        printf("%s: protocol not supported", LOG_MODULE);
        return NULL;
    }

    printf("%s: calloc memory for input_plugin\n", LOG_MODULE);
    this = (p2p_input_plugin_t *) malloc(sizeof (p2p_input_plugin_t)); // calloc(1, sizeof (p2p_input_plugin_t));

    this->mrl = strdup(mrl);

    this->stream = stream;
    this->fh = -1;

    this->buffer = (uint8_t *) malloc(BUFFER_SIZE);
    this->buffer_put_ptr = this->buffer;
    this->buffer_get_ptr = this->buffer;
    this->buffer_count = 0;
    this->curpos = 0;
    this->buffer_max_size = BUFFER_SIZE;

    pthread_mutex_init(&this->buffer_ring_mutex, NULL);
    pthread_cond_init(&this->reader_cond, NULL);
    pthread_cond_init(&this->writer_cond, NULL);

    // for testing:
    this->sizeChunkData = 0;

    // TODO: get the right interface
    this->interface = "lo0";

    this->own_port = OWN_PORT;

    ////////////////////
    // parse the mrl and write to host and host_port
    ////////////////////
    printf("%s: start parsing the mrl (%s) [size=%d]\n", LOG_MODULE, this->mrl, strlen(this->mrl));

    // copy everything without p2p:// to url
    int urlLength = strlen(this->mrl) + 1 - 6; // +1 for \0
    char *url = (char *) malloc(urlLength);
    printf("%s: malloc url (size=%d)\n", LOG_MODULE, urlLength);
    strncpy(url, this->mrl + 6, urlLength - 1);
    url[urlLength - 1] = '\0';
    printf("%s: url is %s\n", LOG_MODULE, url);

    // host
    char *colon = strchr(url, ':');
    int positionColon = (int) (colon - url);
    printf("%s: position of : is %d\n", LOG_MODULE, positionColon);

    this->host = (char*) malloc(positionColon + 1); // + 1 for \0
    strncpy(this->host, url, positionColon);
    this->host[positionColon] = '\0';
    printf("%s: host is %s\n", LOG_MODULE, this->host);

    // port
    if (positionColon) {
        // port
        char *port = malloc(strlen(url) - positionColon); // 1 more for \0
        strncpy(port, url + positionColon + 1, strlen(url) - positionColon);
        port[strlen(url) - positionColon] = '\0';
        this->host_port = atoi(port);
        printf("%s: port is %d\n", LOG_MODULE, this->host_port);
        free(port);
    } else {
        // use default port
        this->host_port = DEFAULT_HOST_PORT;
        printf("%s: using default port (%d)\n", LOG_MODULE, this->host_port);
    }

    free(colon);
    free(url);

    printf("%s: parsing mrl finished\n", LOG_MODULE);

    this->input_plugin.open = p2p_plugin_open;
    this->input_plugin.get_capabilities = p2p_plugin_get_capabilities;
    this->input_plugin.read = p2p_plugin_read;
    this->input_plugin.read_block = p2p_plugin_read_block;
    this->input_plugin.seek = p2p_plugin_seek;
    this->input_plugin.get_current_pos = p2p_plugin_get_current_pos;
    this->input_plugin.get_length = p2p_plugin_get_length;
    this->input_plugin.get_blocksize = p2p_plugin_get_blocksize;
    this->input_plugin.get_mrl = p2p_plugin_get_mrl;
    this->input_plugin.get_optional_data = p2p_plugin_get_optional_data;
    this->input_plugin.dispose = p2p_plugin_dispose;
    this->input_plugin.input_class = class_gen;

    return &this->input_plugin;
}

/**
 * This function initializes an input plugin class object
 * 
 * @param xine
 * @param data
 * @return 
 */
static void *p2p_init_class(xine_t *xine, void *data) {
    p2p_input_class_t *this;

    this = calloc(1, sizeof (p2p_input_class_t));

    this->xine = xine;
    this->config = xine->config;

    // set functions for xine access
    this->input_class.get_instance = p2p_class_get_instance;
    this->input_class.identifier = "p2p"; //p2p_class_get_identifier;
    this->input_class.description = N_("p2p input plugin"); //p2p_class_get_description;
    this->input_class.get_dir = NULL;
    this->input_class.get_autoplay_list = NULL;
    this->input_class.dispose = p2p_class_dispose;
    this->input_class.eject_media = NULL;

    return this;
}

const plugin_info_t xine_plugin_info[] = {
    /* type, API, "name", version, special_info, init_function */
    { PLUGIN_INPUT, 18, "p2p", XINE_VERSION_CODE, NULL, p2p_init_class},
    { PLUGIN_NONE, 0, "", 0, NULL, NULL}
};
