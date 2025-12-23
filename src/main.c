#include <gio/gio.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <libportal/portal.h>
#include <stdio.h>

#define CAPTURE_WIDTH 256
#define CAPTURE_HEIGHT 144
#define CAPTURE_DEPTH 2
#define CAPTURE_FPS 24

typedef struct {
        unsigned char r, g, b;
} RGB;

static RGB
    g_final_buffer[(2 * (CAPTURE_WIDTH / CAPTURE_DEPTH + CAPTURE_HEIGHT / CAPTURE_DEPTH)) * 3];

static XdpSession *g_session;
static GstElement *g_pipeline;

static void average_pixel_box(unsigned char *data, int start_x, int start_y, int box_size,
                              RGB *result) {
        int total_r = 0, total_g = 0, total_b = 0;
        int count = 0;

        for (int dy = 0; dy < box_size; dy++) {
                for (int dx = 0; dx < box_size; dx++) {
                        int x = start_x + dx;
                        int y = start_y + dy;

                        if (x >= 0 && x < CAPTURE_WIDTH && y >= 0 && y < CAPTURE_HEIGHT) {
                                int offset = (y * CAPTURE_WIDTH + x) * 3;
                                total_r += data[offset + 0];
                                total_g += data[offset + 1];
                                total_b += data[offset + 2];
                                count++;
                        }
                }
        }

        result->r = count > 0 ? total_r / count : 0;
        result->g = count > 0 ? total_g / count : 0;
        result->b = count > 0 ? total_b / count : 0;
}

static GstFlowReturn on_new_sample(GstElement *sink, gpointer data) {
        GstSample *sample;
        GstBuffer *buffer;
        GstMapInfo map;

        sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample) {
                return GST_FLOW_ERROR;
        }

        buffer = gst_sample_get_buffer(sample);
        if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                memset(g_final_buffer, '0', sizeof(g_final_buffer));

                int buffer_index = 0;

                // 1. BOTTOM EDGE: Right to left (bottom-right to bottom-left)
                for (int x = CAPTURE_WIDTH - CAPTURE_DEPTH; x >= 0; x -= CAPTURE_DEPTH) {
                        int y = CAPTURE_HEIGHT - CAPTURE_DEPTH;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // 2. LEFT EDGE: Bottom to top (bottom-left to top-left)
                for (int y = CAPTURE_HEIGHT - CAPTURE_DEPTH; y >= 0; y -= CAPTURE_DEPTH) {
                        int x = 0;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // 3. TOP EDGE: Left to right (top-left to top-right)
                for (int x = 0; x < CAPTURE_WIDTH; x += CAPTURE_DEPTH) {
                        int y = 0;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // 4. RIGHT EDGE: Top to bottom (top-right to bottom-right)
                for (int y = 0; y < CAPTURE_HEIGHT; y += CAPTURE_DEPTH) {
                        int x = CAPTURE_WIDTH - CAPTURE_DEPTH;
                        average_pixel_box(map.data, x, y, CAPTURE_DEPTH,
                                          &g_final_buffer[buffer_index++]);
                }

                // printf("\033[2J\033[H");
                // printf("Border samples (clockwise from bottom-right): %d\n", buffer_index);
                // for (int i = 0; i < buffer_index; i++) {
                //         printf("Sample %3d: RGB(%3d, %3d, %3d) ", i, g_final_buffer[i].r,
                //                g_final_buffer[i].g, g_final_buffer[i].b);
                //         printf("\033[48;2;%d;%d;%dm    \033[0m\n", g_final_buffer[i].r,
                //                g_final_buffer[i].g, g_final_buffer[i].b);
                // }

                gst_buffer_unmap(buffer, &map);
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
}

static void start_gstreamer(int fd, int node) {
        GstElement *pipewiresrc, *appsink, *videorate, *videoscale, *videoconvert;
        GstCaps *caps;
        gchar *path;

        gst_init(NULL, NULL);

        g_pipeline = gst_pipeline_new("capture");
        if (!g_pipeline) {
                g_printerr("Failed to create pipeline\n");
                return;
        }

        pipewiresrc = gst_element_factory_make("pipewiresrc", NULL);
        if (!pipewiresrc) {
                g_printerr("Failed to create pipewiresrc\n");
                return;
        }

        videoconvert = gst_element_factory_make("videoconvert", NULL);
        if (!videoconvert) {
                g_printerr("Failed to create videoconvert\n");
                return;
        }

        videoscale = gst_element_factory_make("videoscale", NULL);
        if (!videoscale) {
                g_printerr("Failed to create videoscale\n");
                return;
        }

        videorate = gst_element_factory_make("videorate", NULL);
        if (!videorate) {
                g_printerr("Failed to create videorate\n");
                return;
        }

        appsink = gst_element_factory_make("appsink", NULL);
        if (!appsink) {
                g_printerr("Failed to create appsink\n");
                return;
        }

        g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE, "max-buffers", 1, "drop", TRUE,
                     NULL);
        g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

        path = g_strdup_printf("%u", node);
        g_object_set(pipewiresrc, "fd", fd, "path", path, NULL);
        g_free(path);

        gst_bin_add_many(GST_BIN(g_pipeline), pipewiresrc, videoconvert, videoscale, videorate,
                         appsink, NULL);

        if (!gst_element_link_many(pipewiresrc, videoconvert, videoscale, videorate, NULL)) {
                g_printerr("Failed to link elements\n");
                return;
        }

        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                   G_TYPE_INT, CAPTURE_WIDTH, "height", G_TYPE_INT, CAPTURE_HEIGHT,
                                   "framerate", GST_TYPE_FRACTION, CAPTURE_FPS, 1, NULL);

        if (!gst_element_link_filtered(videorate, appsink, caps)) {
                g_printerr("Failed to link with caps filter\n");
                gst_caps_unref(caps);
                return;
        }

        gst_caps_unref(caps);

        gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
        g_print("Pipeline running, capturing frames...\n");
}

static void start_session_cb(GObject *source, GAsyncResult *res, gpointer data) {
        XdpSession *session = XDP_SESSION(source);
        GError *error = NULL;

        if (!xdp_session_start_finish(session, res, &error)) {
                g_printerr("Start session failed: %s\n", error->message);
                g_error_free(error);
                return;
        }

        GVariant *streams = xdp_session_get_streams(session);
        int node = 0;

        if (streams) {
                GVariantIter iter;
                g_variant_iter_init(&iter, streams);

                GVariant *options;

                g_variant_iter_next(&iter, "(u@a{sv})", &node, &options);
                g_print("PipeWire Node ID: %d\n", node);
                g_variant_unref(options);
        }

        int fd = xdp_session_open_pipewire_remote(session);
        g_print("PipeWire FD: %d\n", fd);

        start_gstreamer(fd, node);
}

static void create_session_cb(GObject *source, GAsyncResult *res, gpointer data) {
        XdpPortal *portal = XDP_PORTAL(source);
        GError *error = NULL;
        XdpSession *session = xdp_portal_create_screencast_session_finish(portal, res, &error);

        if (error) {
                g_printerr("Create session failed: %s\n", error->message);
                g_error_free(error);
                return;
        }

        g_print("Session created: %p\n", session);
        g_session = session;

        xdp_session_start(session, NULL, NULL, start_session_cb, NULL);
}

int main() {
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        XdpPortal *portal = xdp_portal_new();

        xdp_portal_create_screencast_session(portal, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
                                             XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_TRANSIENT,
                                             NULL, NULL, create_session_cb, NULL);

        g_main_loop_run(loop);
        return 0;
}
