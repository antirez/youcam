/* Person segmentation: a tiny inference engine for MediaPipe Selfie
 * Segmentation, converted offline to .ysm by tools/convert_selfie.py.
 * Tensors are NHWC float32 with batch 1; one function per operator.
 * Convolutions keep the output channels innermost so the compiler can
 * vectorise them; HOT builds an AVX2/FMA clone picked at load time. */

#include <limits.h>
#include <math.h>
#include <string.h>
#include "youcam.h"

#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__)
#define HOT __attribute__((target_clones("arch=haswell", "default")))
#else
#define HOT
#endif

#define SEG_MIN_ROWS_PER_JOB 4   /* Smaller jobs cost more to hand out than they save. */
#define SEG_MAX_THREADS 8
#define SEG_PARK_WAIT_MS 50      /* Upper bound on waiting for freed threads to park. */

typedef struct {
    void (*fn)(const seg_conv_args *, int, int);
    const seg_conv_args *a;
    int y0, y1;
} seg_job;

/* One pool shared by all callers: segment_load, segment_free and segment_run
 * are only ever called from one thread at a time (the background worker, and
 * the tests), so LiveModels and Pool need no lock. */
static GThreadPool *Pool;
static GMutex PoolLock;
static GCond PoolDone;
static int PoolPending;
static gboolean PoolFailed;   /* Threads are unavailable: stay single-threaded. */
static int LiveModels;        /* The pool lives only while a model does. */

static void pool_work(gpointer data, gpointer unused) {
    (void)unused;
    seg_job *j = data;
    j->fn(j->a, j->y0, j->y1);
    g_mutex_lock(&PoolLock);
    if (--PoolPending == 0) g_cond_signal(&PoolDone);
    g_mutex_unlock(&PoolLock);
}

/* Splits the output rows across a few threads and waits for them. */
static void run_rows(void (*fn)(const seg_conv_args *, int, int), const seg_conv_args *a) {
    int rows = a->out_dims.h;
    int jobs = MIN(MIN(SEG_MAX_THREADS, (int)g_get_num_processors()), rows / SEG_MIN_ROWS_PER_JOB);
    if (jobs < 2) { fn(a, 0, rows); return; }
    if (!Pool && !PoolFailed) {
        GError *err = NULL;
        Pool = g_thread_pool_new_full(pool_work, NULL, NULL, jobs - 1, TRUE, &err);
        if (!Pool) {
            g_warning("Segmentation runs on one thread: %s", err->message);
            g_error_free(err);
            PoolFailed = TRUE;
        }
    }
    if (!Pool) { fn(a, 0, rows); return; }
    seg_job job[SEG_MAX_THREADS];
    g_mutex_lock(&PoolLock);
    PoolPending = jobs - 1;
    g_mutex_unlock(&PoolLock);
    for (int i = 0; i < jobs; i++) job[i] = (seg_job){fn, a, rows * i / jobs, rows * (i + 1) / jobs};
    /* The pool is exclusive and new_full already started every thread, so a
     * push never has to spawn one and cannot fail. A FALSE return would not
     * mean "not queued" anyway: GLib keeps the job and runs it later. */
    for (int i = 1; i < jobs; i++) g_thread_pool_push(Pool, &job[i], NULL);
    fn(a, job[0].y0, job[0].y1);   /* This thread does the first share. */
    g_mutex_lock(&PoolLock);
    while (PoolPending) g_cond_wait(&PoolDone, &PoolLock);
    g_mutex_unlock(&PoolLock);
}

HOT static void conv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *px = a->in + ((size_t)iy * a->in_dims.w + ix) * ic;
                    const float *wk = a->weights + (size_t)(ky * a->kw + kx) * ic * oc;
                    for (int c = 0; c < ic; c++) {
                        float v = px[c];
                        const float *restrict wr = wk + (size_t)c * oc;
                        for (int k = 0; k < oc; k++) o[k] += v * wr[k];
                    }
                }
            }
        }
}

HOT static void dwconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ch = a->in_dims.c;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * ch;
            memcpy(o, a->bias, ch * sizeof(float));
            for (int ky = 0; ky < a->kh; ky++) {
                int iy = oy * a->stride - a->pad_top + ky;
                if (iy < 0 || iy >= a->in_dims.h) continue;
                for (int kx = 0; kx < a->kw; kx++) {
                    int ix = ox * a->stride - a->pad_left + kx;
                    if (ix < 0 || ix >= a->in_dims.w) continue;
                    const float *restrict px = a->in + ((size_t)iy * a->in_dims.w + ix) * ch;
                    const float *restrict wk = a->weights + (size_t)(ky * a->kw + kx) * ch;
                    for (int c = 0; c < ch; c++) o[c] += px[c] * wk[c];
                }
            }
        }
}

HOT static void tconv_rows(const seg_conv_args *a, int y0, int y1) {
    int ic = a->in_dims.c, oc = a->out_dims.c, k = a->stride;
    for (int oy = y0; oy < y1; oy++)
        for (int ox = 0; ox < a->out_dims.w; ox++) {
            float *restrict o = a->out + ((size_t)oy * a->out_dims.w + ox) * oc;
            memcpy(o, a->bias, oc * sizeof(float));
            const float *px = a->in + ((size_t)(oy / k) * a->in_dims.w + ox / k) * ic;
            const float *wk = a->weights + (size_t)((oy % k) * k + ox % k) * ic * oc;
            for (int c = 0; c < ic; c++) {
                float v = px[c];
                const float *restrict wr = wk + (size_t)c * oc;
                for (int j = 0; j < oc; j++) o[j] += v * wr[j];
            }
        }
}

void seg_conv(const seg_conv_args *a) { run_rows(conv_rows, a); }
void seg_dwconv(const seg_conv_args *a) { run_rows(dwconv_rows, a); }
void seg_tconv(const seg_conv_args *a) { run_rows(tconv_rows, a); }

/* TFLite RESIZE_BILINEAR with half_pixel_centers and no align_corners. */
void seg_resize(const float *in, seg_dims di, float *out, seg_dims dout) {
    double sy = (double)di.h / dout.h, sx = (double)di.w / dout.w;
    for (int oy = 0; oy < dout.h; oy++) {
        double fy = fmax(0, (oy + 0.5) * sy - 0.5);
        int y0 = MIN((int)fy, di.h - 1), y1 = MIN(y0 + 1, di.h - 1);
        float dy = (float)(fy - y0);
        for (int ox = 0; ox < dout.w; ox++) {
            double fx = fmax(0, (ox + 0.5) * sx - 0.5);
            int x0 = MIN((int)fx, di.w - 1), x1 = MIN(x0 + 1, di.w - 1);
            float dx = (float)(fx - x0);
            const float *a = in + ((size_t)y0 * di.w + x0) * di.c, *b = in + ((size_t)y0 * di.w + x1) * di.c;
            const float *c = in + ((size_t)y1 * di.w + x0) * di.c, *d = in + ((size_t)y1 * di.w + x1) * di.c;
            float *o = out + ((size_t)oy * dout.w + ox) * dout.c;
            for (int k = 0; k < di.c; k++) {
                float top = a[k] + (b[k] - a[k]) * dx, bottom = c[k] + (d[k] - c[k]) * dx;
                o[k] = top + (bottom - top) * dy;
            }
        }
    }
}

void seg_mean(const float *in, seg_dims di, float *out) {
    size_t n = (size_t)di.h * di.w;
    for (int c = 0; c < di.c; c++) out[c] = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < di.c; c++) out[c] += in[i * di.c + c];
    for (int c = 0; c < di.c; c++) out[c] /= n;
}

void seg_relu(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] > 0 ? in[i] : 0;
}

void seg_hswish(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] * fminf(fmaxf(in[i] + 3, 0), 6) / 6;
}

void seg_sigmoid(const float *in, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = 1 / (1 + expf(-in[i]));
}

void seg_mul_channels(const float *in, const float *scale, seg_dims d, float *out) {
    size_t n = (size_t)d.h * d.w;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < d.c; c++) out[i * d.c + c] = in[i * d.c + c] * scale[c];
}

void seg_add(const float *a, const float *b, float *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i];
}

/* ---------------------------------------------------------------- .ysm
 *
 * The format written by tools/convert_selfie.py. Everything is little
 * endian, 4 bytes per field (40-byte header, struct '<4s2I2f5I'):
 *   "YSM1", u32 in_h, u32 in_w, f32 norm_scale, f32 norm_offset
 *   (model input = byte * scale + offset), u32 invert (1: the model
 *   outputs background, so the mask is 1 - output), u32 tensor_count,
 *   layer_count, input (tensor index), output (tensor index).
 * Then tensor_count shapes, u32 h, w, c each (NHWC, batch 1), then
 * layer_count layers, each 11 u32: op, in0, in1, out, kh, kw, stride,
 * pad_top, pad_left, weight_count, bias_count, followed by weight_count
 * then bias_count f32. Layers run in file order; a layer reads tensors
 * in0 (and in1 for MUL/ADD, 0xFFFFFFFF otherwise) and writes out.
 * Ops: 1 CONV, 2 DWCONV, 3 RELU, 4 HSWISH, 5 SIGMOID, 6 MEAN (to 1x1),
 * 7 MUL (in0 times the 1x1xC tensor in1, per channel), 8 ADD, 9 RESIZE
 * (bilinear to the out shape), 10 TCONV (kernel == stride, no padding).
 * Only the kernel ops carry weights. Layouts: CONV [kh][kw][in c][out c],
 * DWCONV [kh][kw][c], TCONV [ky][kx][in c][out c]; bias has out c floats.
 *
 * The file is untrusted: every count, shape, index and size is checked
 * before use, so a bad file is a GError, never a crash. */

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error ".ysm files are little endian"
#endif

#define YSM_MAX_TENSORS 1024
#define YSM_MAX_LAYERS 1024
#define YSM_MAX_DIM 4096
#define YSM_MAX_ELEMENTS (1 << 24)
#define YSM_MAX_TOTAL_ELEMENTS (1 << 25)   /* The real models need 7.5M. */
#define YSM_MAX_KERNEL 15
#define YSM_LAYER_FIELDS 11

enum { OP_CONV = 1, OP_DWCONV, OP_RELU, OP_HSWISH, OP_SIGMOID, OP_MEAN, OP_MUL, OP_ADD, OP_RESIZE, OP_TCONV };

typedef struct {
    int op, in0, in1, out, kh, kw, stride, pad_top, pad_left;
    float *weights, *bias;   /* One allocation; bias points inside it. */
} seg_layer;

struct segment_model {
    int in_w, in_h, invert;
    float norm_scale, norm_offset;
    int tensor_count, layer_count, input, output;
    seg_dims *dims;
    float **act;
    seg_layer *layers;
};

typedef struct { const guint8 *p; size_t left; int ok; } reader;

static guint32 read_u32(reader *r) {
    guint32 v = 0;
    if (r->left < 4) { r->ok = 0; return 0; }
    memcpy(&v, r->p, 4);
    r->p += 4;
    r->left -= 4;
    return v;
}

static float read_f32(reader *r) {
    guint32 u = read_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static int field(guint32 v) { return v > INT_MAX ? -1 : (int)v; }   /* 0xFFFFFFFF (unused) -> -1 */

static int same_dims(seg_dims a, seg_dims b) { return a.h == b.h && a.w == b.w && a.c == b.c; }

/* ready[t] is set once the input or an earlier layer has produced tensor t. */
static const char *check_layer(const segment_model *m, const seg_layer *l, guint32 wc, guint32 bc,
                               const unsigned char *ready) {
    int binary = l->op == OP_MUL || l->op == OP_ADD, kernel = l->op == OP_CONV || l->op == OP_DWCONV || l->op == OP_TCONV;
    if (l->op < OP_CONV || l->op > OP_TCONV) return "unknown operator";
    if (l->in0 < 0 || l->in0 >= m->tensor_count || l->out < 0 || l->out >= m->tensor_count ||
        (binary && (l->in1 < 0 || l->in1 >= m->tensor_count)))
        return "tensor index out of range";
    if (!ready[l->in0] || (binary && !ready[l->in1])) return "a layer reads a tensor nothing has produced";
    if (l->out == l->in0 || (binary && l->out == l->in1) || l->out == m->input) return "a layer overwrites its input";
    if (kernel && (l->kh < 1 || l->kw < 1 || l->kh > YSM_MAX_KERNEL || l->kw > YSM_MAX_KERNEL ||
                   l->stride < 1 || l->stride > YSM_MAX_KERNEL || l->pad_top < 0 || l->pad_left < 0 ||
                   l->pad_top >= l->kh || l->pad_left >= l->kw))
        return "bad kernel";
    if (!kernel && (wc || bc)) return "unexpected weights";
    seg_dims i = m->dims[l->in0], o = m->dims[l->out];
    guint64 taps = (guint64)l->kh * l->kw;
    switch (l->op) {
    case OP_CONV:
        return wc == taps * i.c * o.c && bc == (guint32)o.c ? NULL : "convolution weights do not match";
    case OP_DWCONV:
        return i.c == o.c && wc == taps * i.c && bc == (guint32)o.c ? NULL : "depthwise weights do not match";
    case OP_TCONV:
        return l->kh == l->kw && l->kh == l->stride && !l->pad_top && !l->pad_left &&
               o.h == i.h * l->stride && o.w == i.w * l->stride && wc == taps * i.c * o.c &&
               bc == (guint32)o.c ? NULL : "transposed convolution does not match";
    case OP_MEAN:
        return o.h == 1 && o.w == 1 && o.c == i.c ? NULL : "mean shape";
    case OP_MUL: {
        seg_dims s = m->dims[l->in1];
        return s.h == 1 && s.w == 1 && s.c == i.c && same_dims(i, o) ? NULL : "multiply shape";
    }
    case OP_ADD:
        return same_dims(i, m->dims[l->in1]) && same_dims(i, o) ? NULL : "add shape";
    case OP_RESIZE:
        return i.c == o.c ? NULL : "resize channels";
    default:   /* RELU, HSWISH, SIGMOID */
        return same_dims(i, o) ? NULL : "activation shape";
    }
}

static const char *read_tensors(segment_model *m, reader *r, guint32 count) {
    m->dims = g_new0(seg_dims, count);
    m->act = g_new0(float *, count);
    m->tensor_count = count;
    guint64 total = 0;
    for (guint32 t = 0; t < count; t++) {
        guint32 h = read_u32(r), w = read_u32(r), c = read_u32(r);
        if (!r->ok) return "truncated tensors";
        if (!h || !w || !c || h > YSM_MAX_DIM || w > YSM_MAX_DIM || c > YSM_MAX_DIM ||
            (guint64)h * w * c > YSM_MAX_ELEMENTS)
            return "bad tensor shape";
        m->dims[t] = (seg_dims){(int)h, (int)w, (int)c};
        total += (guint64)h * w * c;
    }
    /* Per-tensor caps are not enough: 1024 maximal tensors would be 64 GiB. */
    if (total > YSM_MAX_TOTAL_ELEMENTS) return "tensors too large";
    for (guint32 t = 0; t < count; t++) {
        m->act[t] = g_try_new0(float, (size_t)m->dims[t].h * m->dims[t].w * m->dims[t].c);
        if (!m->act[t]) return "out of memory";
    }
    return NULL;
}

static const char *read_layers(segment_model *m, reader *r, guint32 count) {
    m->layers = g_new0(seg_layer, count);
    unsigned char *ready = g_new0(unsigned char, m->tensor_count);
    ready[m->input] = 1;
    const char *bad = NULL;
    for (guint32 i = 0; i < count; i++) {
        guint32 f[YSM_LAYER_FIELDS];
        for (int k = 0; k < YSM_LAYER_FIELDS; k++) f[k] = read_u32(r);
        if (!r->ok) { bad = "truncated layer"; break; }
        seg_layer *l = &m->layers[i];
        m->layer_count = i + 1;
        *l = (seg_layer){field(f[0]), field(f[1]), field(f[2]), field(f[3]), field(f[4]), field(f[5]),
                         field(f[6]), field(f[7]), field(f[8]), NULL, NULL};
        if ((bad = check_layer(m, l, f[9], f[10], ready))) break;
        guint64 n = (guint64)f[9] + f[10];
        if (n * 4 > r->left) { bad = "truncated weights"; break; }
        l->weights = g_new(float, n + 1);
        memcpy(l->weights, r->p, n * 4);
        r->p += n * 4;
        r->left -= n * 4;
        l->bias = l->weights + f[9];
        ready[l->out] = 1;
    }
    g_free(ready);
    return bad;
}

static const char *read_model(segment_model *m, reader *r) {
    if (r->left < 4 || memcmp(r->p, "YSM1", 4) != 0) return "not a .ysm file";
    r->p += 4;
    r->left -= 4;
    guint32 in_h = read_u32(r), in_w = read_u32(r);
    m->norm_scale = read_f32(r);
    m->norm_offset = read_f32(r);
    guint32 invert = read_u32(r), tensors = read_u32(r), layers = read_u32(r);
    guint32 input = read_u32(r), output = read_u32(r);
    if (!r->ok) return "truncated header";
    if (tensors < 2 || tensors > YSM_MAX_TENSORS || layers < 1 || layers > YSM_MAX_LAYERS ||
        input >= tensors || output >= tensors || invert > 1 ||
        !isfinite(m->norm_scale) || !isfinite(m->norm_offset))
        return "bad header";
    m->in_h = field(in_h);
    m->in_w = field(in_w);
    m->invert = invert;
    m->input = input;
    m->output = output;
    const char *bad = read_tensors(m, r, tensors);
    if (bad) return bad;
    seg_dims in = m->dims[input], out = m->dims[output];
    if (in.h != m->in_h || in.w != m->in_w || in.c != 3 || out.h != m->in_h || out.w != m->in_w || out.c != 1)
        return "bad input or output shape";
    if ((bad = read_layers(m, r, layers))) return bad;
    return r->left ? "trailing bytes" : NULL;
}

segment_model *segment_load(const guint8 *data, size_t size, GError **err) {
    reader r = {data, size, 1};
    segment_model *m = g_new0(segment_model, 1);
    LiveModels++;
    const char *problem = read_model(m, &r);
    if (!problem) return m;
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Invalid segmentation model: %s", problem);
    segment_free(m);
    return NULL;
}

void segment_free(segment_model *m) {
    if (!m) return;
    /* Idle workers would otherwise outlive the background mode that wanted them. */
    if (--LiveModels == 0 && Pool) {
        /* The unused list is global (GIO parks its threads there too), so
         * count ours on top of what is already parked. */
        guint parked = g_thread_pool_get_num_unused_threads() + g_thread_pool_get_num_threads(Pool);
        g_thread_pool_free(Pool, FALSE, TRUE);
        Pool = NULL;
        /* Freed threads park as "unused" for ~15 s, but only a moment after
         * the free returns: stopping them at once would miss some. Off must
         * leave no thread, so wait (briefly, bounded) for them to park. */
        for (int ms = 0; ms < SEG_PARK_WAIT_MS && g_thread_pool_get_num_unused_threads() < parked; ms++)
            g_usleep(1000);
        g_thread_pool_stop_unused_threads();
    }
    for (int i = 0; i < m->tensor_count; i++) g_free(m->act[i]);
    for (int i = 0; i < m->layer_count; i++) g_free(m->layers[i].weights);
    g_free(m->act);
    g_free(m->dims);
    g_free(m->layers);
    g_free(m);
}

void segment_input_size(const segment_model *m, int *w, int *h) {
    *w = m->in_w;
    *h = m->in_h;
}

static void run_layer(segment_model *m, const seg_layer *l) {
    seg_dims di = m->dims[l->in0], dout = m->dims[l->out];
    const float *in = m->act[l->in0];
    float *out = m->act[l->out];
    size_t n = (size_t)dout.h * dout.w * dout.c;
    seg_conv_args a = {in, di, l->weights, l->bias, l->kh, l->kw, l->stride, l->pad_top, l->pad_left, out, dout};
    switch (l->op) {
    case OP_CONV: seg_conv(&a); break;
    case OP_DWCONV: seg_dwconv(&a); break;
    case OP_TCONV: seg_tconv(&a); break;
    case OP_RELU: seg_relu(in, out, n); break;
    case OP_HSWISH: seg_hswish(in, out, n); break;
    case OP_SIGMOID: seg_sigmoid(in, out, n); break;
    case OP_MEAN: seg_mean(in, di, out); break;
    case OP_MUL: seg_mul_channels(in, m->act[l->in1], di, out); break;
    case OP_ADD: seg_add(in, m->act[l->in1], out, n); break;
    case OP_RESIZE: seg_resize(in, di, out, dout); break;
    }
}

void segment_run(segment_model *m, const guint8 *rgb, float *mask) {
    float *in = m->act[m->input];
    size_t n = (size_t)m->in_w * m->in_h;
    for (size_t i = 0; i < n * 3; i++) in[i] = rgb[i] * m->norm_scale + m->norm_offset;
    for (int i = 0; i < m->layer_count; i++) run_layer(m, &m->layers[i]);
    const float *out = m->act[m->output];
    for (size_t i = 0; i < n; i++) mask[i] = m->invert ? 1 - out[i] : out[i];
}
