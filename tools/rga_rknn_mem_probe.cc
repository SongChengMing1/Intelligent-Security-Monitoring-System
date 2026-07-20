#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "RgaUtils.h"
#include "im2d.h"
#include "rga.h"
#include "rknn_api.h"

struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

static unsigned char *load_file(const char *filename, int *model_size)
{
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        printf("open model failed: %s\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char *data = (unsigned char *)malloc(size);
    if (data == NULL)
    {
        fclose(fp);
        printf("malloc model failed\n");
        return NULL;
    }

    size_t read_size = fread(data, 1, size, fp);
    fclose(fp);
    if (read_size != (size_t)size)
    {
        free(data);
        printf("read model failed\n");
        return NULL;
    }

    *model_size = size;
    return data;
}

static void dump_tensor_attr(const rknn_tensor_attr *attr)
{
    printf("input attr: index=%d, n_dims=%d, dims=[%d,%d,%d,%d], n_elems=%d, size=%d, size_with_stride=%d, "
           "w_stride=%d, fmt=%s, type=%s\n",
           attr->index,
           attr->n_dims,
           attr->dims[0],
           attr->dims[1],
           attr->dims[2],
           attr->dims[3],
           attr->n_elems,
           attr->size,
           attr->size_with_stride,
           attr->w_stride,
           get_format_string(attr->fmt),
           get_type_string(attr->type));
}

static int get_nhwc_shape(const rknn_tensor_attr *attr, int *height, int *width, int *channel)
{
    if (attr->fmt == RKNN_TENSOR_NHWC)
    {
        *height = attr->dims[1];
        *width = attr->dims[2];
        *channel = attr->dims[3];
        return 0;
    }

    if (attr->fmt == RKNN_TENSOR_NCHW)
    {
        *channel = attr->dims[1];
        *height = attr->dims[2];
        *width = attr->dims[3];
        return 0;
    }

    printf("unsupported input format: %s\n", get_format_string(attr->fmt));
    return -1;
}

static void fill_pattern(uint8_t *buffer, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        buffer[i] = (uint8_t)(i * 31 + 17);
    }
}

static int alloc_dma_heap(const char *heap_path, size_t size, int *fd, void **addr)
{
    int heap_fd = open(heap_path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        printf("  dma_heap open failed: %s: %s\n", heap_path, strerror(errno));
        return -1;
    }

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
    {
        printf("  dma_heap alloc failed: %s: %s\n", heap_path, strerror(errno));
        close(heap_fd);
        return -1;
    }
    close(heap_fd);

    void *mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, data.fd, 0);
    if (mapped == MAP_FAILED)
    {
        printf("  dma_heap mmap failed: %s\n", strerror(errno));
        close((int)data.fd);
        return -1;
    }

    *fd = (int)data.fd;
    *addr = mapped;
    return 0;
}

static void release_dma_heap(int fd, void *addr, size_t size)
{
    if (addr != NULL && addr != MAP_FAILED)
    {
        munmap(addr, size);
    }
    if (fd >= 0)
    {
        close(fd);
    }
}

static double elapsed_ms(const timeval &start, const timeval &stop)
{
    return (stop.tv_sec - start.tv_sec) * 1000.0 + (stop.tv_usec - start.tv_usec) / 1000.0;
}

static void run_rga_to_rknn_mem_case(rknn_tensor_mem *input_mem, int dst_width, int dst_height, int dst_wstride,
                                     const char *src_name, const char *heap_path, int src_width, int src_height)
{
    const int channel = 3;
    const size_t src_size = (size_t)src_width * src_height * channel;
    void *src_addr = NULL;
    int src_fd = -1;

    printf("CASE src=%s heap=%s src=%dx%d dst_rknn_fd=%d dst=%dx%d dst_wstride=%d\n",
           src_name,
           heap_path == NULL ? "-" : heap_path,
           src_width,
           src_height,
           input_mem->fd,
           dst_width,
           dst_height,
           dst_wstride);

    if (strcmp(src_name, "virtual") == 0)
    {
        if (posix_memalign(&src_addr, 4096, src_size) != 0)
        {
            printf("  posix_memalign src failed\n");
            return;
        }
    }
    else
    {
        if (alloc_dma_heap(heap_path, src_size, &src_fd, &src_addr) < 0)
        {
            return;
        }
    }

    fill_pattern((uint8_t *)src_addr, src_size);
    memset(input_mem->virt_addr, 0, input_mem->size);

    rga_buffer_t src;
    if (strcmp(src_name, "virtual") == 0)
    {
        src = wrapbuffer_virtualaddr(src_addr, src_width, src_height, RK_FORMAT_RGB_888);
    }
    else
    {
        src = wrapbuffer_fd(src_fd, src_width, src_height, RK_FORMAT_RGB_888);
    }
    rga_buffer_t dst = wrapbuffer_fd_t(input_mem->fd, dst_width, dst_height, dst_wstride, dst_height,
                                       RK_FORMAT_RGB_888);

    im_rect src_rect;
    im_rect dst_rect;
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    IM_STATUS check_status = (IM_STATUS)imcheck(src, dst, src_rect, dst_rect);
    printf("  imcheck=%d %s\n", check_status, imStrError(check_status));

    IM_STATUS resize_status = IM_STATUS_FAILED;
    timeval start;
    timeval stop;
    gettimeofday(&start, NULL);
    if (check_status == IM_STATUS_NOERROR)
    {
        resize_status = imresize(src, dst);
    }
    gettimeofday(&stop, NULL);
    printf("  imresize=%d %s, elapsed=%.3f ms\n", resize_status, imStrError(resize_status),
           elapsed_ms(start, stop));

    if (strcmp(src_name, "virtual") == 0)
    {
        free(src_addr);
    }
    else
    {
        release_dma_heap(src_fd, src_addr, src_size);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s <rknn model>\n", argv[0]);
        return -1;
    }

    rga_info_table_entry info;
    memset(&info, 0, sizeof(info));
    IM_STATUS info_status = rga_get_info(&info);
    printf("rga_get_info=%d %s version=%d input_resolution=%u output_resolution=%u scale_limit=%u input_format=0x%x output_format=0x%x\n",
           info_status,
           imStrError(info_status),
           info.version,
           info.input_resolution,
           info.output_resolution,
           info.scale_limit,
           info.input_format,
           info.output_format);

    int model_size = 0;
    unsigned char *model_data = load_file(argv[1], &model_size);
    if (model_data == NULL)
    {
        return -1;
    }

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_data, model_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init failed ret=%d\n", ret);
        free(model_data);
        return -1;
    }

    rknn_sdk_version sdk_ver;
    memset(&sdk_ver, 0, sizeof(sdk_ver));
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
    if (ret == RKNN_SUCC)
    {
        printf("rknn api=%s driver=%s\n", sdk_ver.api_version, sdk_ver.drv_version);
    }

    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (ret != RKNN_SUCC)
    {
        printf("query input attr failed ret=%d\n", ret);
        rknn_destroy(ctx);
        free(model_data);
        return -1;
    }
    dump_tensor_attr(&input_attr);

    int dst_height = 0;
    int dst_width = 0;
    int channel = 0;
    if (get_nhwc_shape(&input_attr, &dst_height, &dst_width, &channel) < 0 || channel != 3)
    {
        rknn_destroy(ctx);
        free(model_data);
        return -1;
    }

    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt = RKNN_TENSOR_NHWC;
    uint32_t mem_size = input_attr.size_with_stride > 0 ? input_attr.size_with_stride : input_attr.size;
    rknn_tensor_mem *input_mem = rknn_create_mem(ctx, mem_size);
    if (input_mem == NULL)
    {
        printf("rknn_create_mem failed, size=%u\n", mem_size);
        rknn_destroy(ctx);
        free(model_data);
        return -1;
    }
    printf("rknn input_mem: fd=%d virt=%p size=%u logical_size=%u\n", input_mem->fd, input_mem->virt_addr,
           input_mem->size, input_attr.size);

    int dst_wstride = input_attr.w_stride > 0 ? input_attr.w_stride : dst_width;
    if (dst_wstride % 8 != 0)
    {
        dst_wstride = dst_width + (8 - dst_width % 8) % 8;
    }
    input_attr.w_stride = dst_wstride;
    input_attr.h_stride = dst_height;

    const int dims[][2] = {
        {1280, 720},
        {1400, 788},
        {1408, 800},
    };

    for (size_t i = 0; i < sizeof(dims) / sizeof(dims[0]); ++i)
    {
        run_rga_to_rknn_mem_case(input_mem, dst_width, dst_height, dst_wstride, "virtual", NULL, dims[i][0],
                                 dims[i][1]);
        run_rga_to_rknn_mem_case(input_mem, dst_width, dst_height, dst_wstride, "dma_heap_fd",
                                 "/dev/dma_heap/system", dims[i][0], dims[i][1]);
        run_rga_to_rknn_mem_case(input_mem, dst_width, dst_height, dst_wstride, "dma_heap_fd",
                                 "/dev/dma_heap/cma", dims[i][0], dims[i][1]);
    }

    rknn_destroy_mem(ctx, input_mem);
    rknn_destroy(ctx);
    free(model_data);
    return 0;
}
