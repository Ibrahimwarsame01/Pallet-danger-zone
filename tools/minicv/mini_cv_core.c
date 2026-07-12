/* Implementation for the mini OpenCV shim — see opencv2/core/core_c.h here.
 * 8-bit images only. ROI handling mirrors OpenCV: operations touch only the
 * ROI region of each image involved; regions must have equal sizes. */
#include <opencv2/core/core_c.h>
#include <opencv2/imgproc/imgproc_c.h>

#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------ */

typedef struct {
    const IplImage *img;
    int x, y, w, h;      /* effective region */
} Region;

static Region region_of(const CvArr *arr) {
    const IplImage *img = (const IplImage *)arr;
    Region r = { img, 0, 0, 0, 0 };
    if (!img) return r;
    if (img->roi) {
        r.x = img->roi->xOffset;
        r.y = img->roi->yOffset;
        r.w = img->roi->width;
        r.h = img->roi->height;
    } else {
        r.w = img->width;
        r.h = img->height;
    }
    return r;
}

static unsigned char *row_ptr(const IplImage *img, const Region *r, int y) {
    return (unsigned char *)img->imageData +
           (size_t)(r->y + y) * img->widthStep +
           (size_t)r->x * img->nChannels;
}

/* ---- allocation ---------------------------------------------------------- */

IplImage *cvCreateImage(CvSize size, int depth, int channels) {
    if (size.width <= 0 || size.height <= 0 || depth != IPL_DEPTH_8U ||
        (channels != 1 && channels != 3 && channels != 4)) {
        return NULL;
    }
    IplImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->nChannels = channels;
    img->depth     = depth;
    img->width     = size.width;
    img->height    = size.height;
    /* OpenCV aligns rows to 4 bytes */
    img->widthStep = ((size.width * channels) + 3) & ~3;
    img->imageData = calloc((size_t)img->widthStep, (size_t)size.height);
    if (!img->imageData) {
        free(img);
        return NULL;
    }
    return img;
}

void cvReleaseImage(IplImage **image) {
    if (!image || !*image) return;
    free((*image)->roi);
    free((*image)->imageData);
    free(*image);
    *image = NULL;
}

IplImage *cvCloneImage(const IplImage *image) {
    if (!image) return NULL;
    IplImage *copy = cvCreateImage(cvSize(image->width, image->height),
                                   image->depth, image->nChannels);
    if (!copy) return NULL;
    memcpy(copy->imageData, image->imageData,
           (size_t)image->widthStep * image->height);
    if (image->roi) {
        cvSetImageROI(copy, cvRect(image->roi->xOffset, image->roi->yOffset,
                                   image->roi->width, image->roi->height));
    }
    return copy;
}

CvSize cvGetSize(const CvArr *arr) {
    Region r = region_of(arr);
    return cvSize(r.w, r.h);
}

/* ---- ROI ----------------------------------------------------------------- */

void cvSetImageROI(IplImage *image, CvRect rect) {
    if (!image) return;
    if (rect.x < 0) rect.x = 0;
    if (rect.y < 0) rect.y = 0;
    if (rect.x + rect.width  > image->width)  rect.width  = image->width  - rect.x;
    if (rect.y + rect.height > image->height) rect.height = image->height - rect.y;
    if (rect.width <= 0 || rect.height <= 0) {
        cvResetImageROI(image);
        return;
    }
    if (!image->roi) {
        image->roi = calloc(1, sizeof(IplROI));
        if (!image->roi) return;
    }
    image->roi->xOffset = rect.x;
    image->roi->yOffset = rect.y;
    image->roi->width   = rect.width;
    image->roi->height  = rect.height;
}

void cvResetImageROI(IplImage *image) {
    if (!image) return;
    free(image->roi);
    image->roi = NULL;
}

/* ---- pixel operations ---------------------------------------------------- */

void cvCopy(const CvArr *src, CvArr *dst, const CvArr *mask) {
    (void)mask; /* project only uses mask=NULL */
    Region s = region_of(src), d = region_of(dst);
    if (!s.img || !d.img || s.w != d.w || s.h != d.h ||
        s.img->nChannels != d.img->nChannels) return;
    size_t rowbytes = (size_t)s.w * s.img->nChannels;
    for (int y = 0; y < s.h; y++) {
        memcpy(row_ptr(d.img, &d, y), row_ptr(s.img, &s, y), rowbytes);
    }
}

void cvAbsDiff(const CvArr *src1, const CvArr *src2, CvArr *dst) {
    Region a = region_of(src1), b = region_of(src2), d = region_of(dst);
    if (!a.img || !b.img || !d.img ||
        a.w != b.w || a.h != b.h || a.w != d.w || a.h != d.h ||
        a.img->nChannels != b.img->nChannels ||
        a.img->nChannels != d.img->nChannels) return;
    size_t n = (size_t)a.w * a.img->nChannels;
    for (int y = 0; y < a.h; y++) {
        const unsigned char *pa = row_ptr(a.img, &a, y);
        const unsigned char *pb = row_ptr(b.img, &b, y);
        unsigned char *pd = row_ptr(d.img, &d, y);
        for (size_t i = 0; i < n; i++) {
            int v = (int)pa[i] - (int)pb[i];
            pd[i] = (unsigned char)(v < 0 ? -v : v);
        }
    }
}

double cvThreshold(const CvArr *src, CvArr *dst,
                   double threshold, double max_value, int threshold_type) {
    (void)threshold_type; /* CV_THRESH_BINARY is the only mode used */
    Region s = region_of(src), d = region_of(dst);
    if (!s.img || !d.img || s.w != d.w || s.h != d.h ||
        s.img->nChannels != 1 || d.img->nChannels != 1) return 0.0;
    int thr = (int)threshold;
    unsigned char hi = (unsigned char)(max_value < 0 ? 0 :
                                       (max_value > 255 ? 255 : max_value));
    for (int y = 0; y < s.h; y++) {
        const unsigned char *ps = row_ptr(s.img, &s, y);
        unsigned char *pd = row_ptr(d.img, &d, y);
        for (int x = 0; x < s.w; x++) {
            pd[x] = ps[x] > thr ? hi : 0;
        }
    }
    return threshold;
}

int cvCountNonZero(const CvArr *arr) {
    Region r = region_of(arr);
    if (!r.img || r.img->nChannels != 1) return 0;
    int count = 0;
    for (int y = 0; y < r.h; y++) {
        const unsigned char *p = row_ptr(r.img, &r, y);
        for (int x = 0; x < r.w; x++) {
            count += (p[x] != 0);
        }
    }
    return count;
}

void cvCvtColor(const CvArr *src, CvArr *dst, int code) {
    if (code != CV_BGR2GRAY && code != CV_BGRA2GRAY &&
        code != CV_RGB2GRAY && code != CV_RGBA2GRAY) return;
    Region s = region_of(src), d = region_of(dst);
    if (!s.img || !d.img || s.w != d.w || s.h != d.h ||
        d.img->nChannels != 1 || s.img->nChannels < 3) return;
    int ch = s.img->nChannels;
    int rgb_order = (code == CV_RGB2GRAY || code == CV_RGBA2GRAY);
    for (int y = 0; y < s.h; y++) {
        const unsigned char *ps = row_ptr(s.img, &s, y);
        unsigned char *pd = row_ptr(d.img, &d, y);
        for (int x = 0; x < s.w; x++) {
            const unsigned char *p = ps + (size_t)x * ch;
            int b = rgb_order ? p[2] : p[0];
            int g = p[1];
            int r = rgb_order ? p[0] : p[2];
            /* OpenCV weights: 0.299 R + 0.587 G + 0.114 B */
            pd[x] = (unsigned char)((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
}
