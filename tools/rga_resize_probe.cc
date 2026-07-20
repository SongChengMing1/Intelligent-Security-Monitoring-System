#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "RgaUtils.h"
#include "im2d.h"
#include "rga.h"

struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

struct probe_case_t {
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
    const char *format_name;
    const char *memory_name;
    const char *heap_path;
};

static int bytes_per_pixel(int format)
{
    switch (format)
    {
    case RK_FORMAT_RGB_888:
        return 3;
    case RK_FORMAT_RGBA_8888:
    case RK_FORMAT_RGBX_8888:
        return 4;
    default:
        return 0;
    }
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

static void run_one_case(const probe_case_t &test_case)
{
    const int dst_width = 640;
    const int dst_height = 640;
    const int dst_wstride = 640;
    const int dst_hstride = 640;
    int bpp = bytes_per_pixel(test_case.format);
    size_t src_size = (size_t)test_case.wstride * test_case.hstride * bpp;
    size_t dst_size = (size_t)dst_wstride * dst_hstride * bpp;
    void *src_addr = NULL;
    void *dst_addr = NULL;
    int src_fd = -1;
    int dst_fd = -1;

    printf("CASE memory=%s heap=%s format=%s src=%dx%d stride=%dx%d dst=640x640\n",
           test_case.memory_name,
           test_case.heap_path == NULL ? "-" : test_case.heap_path,
           test_case.format_name,
           test_case.width,
           test_case.height,
           test_case.wstride,
           test_case.hstride);

    if (bpp == 0)
    {
        printf("  unsupported probe format\n");
        return;
    }

    if (strcmp(test_case.memory_name, "virtual") == 0)
    {
        if (posix_memalign(&src_addr, 4096, src_size) != 0 ||
            posix_memalign(&dst_addr, 4096, dst_size) != 0)
        {
            printf("  posix_memalign failed\n");
            free(src_addr);
            free(dst_addr);
            return;
        }
    }
    else
    {
        if (alloc_dma_heap(test_case.heap_path, src_size, &src_fd, &src_addr) < 0 ||
            alloc_dma_heap(test_case.heap_path, dst_size, &dst_fd, &dst_addr) < 0)
        {
            release_dma_heap(src_fd, src_addr, src_size);
            release_dma_heap(dst_fd, dst_addr, dst_size);
            return;
        }
    }

    fill_pattern((uint8_t *)src_addr, src_size);
    memset(dst_addr, 0, dst_size);

    rga_buffer_t src;
    rga_buffer_t dst;
    if (strcmp(test_case.memory_name, "virtual") == 0)
    {
        src = wrapbuffer_virtualaddr(src_addr, test_case.width, test_case.height, test_case.format,
                                     test_case.wstride, test_case.hstride);
        dst = wrapbuffer_virtualaddr(dst_addr, dst_width, dst_height, test_case.format,
                                     dst_wstride, dst_hstride);
    }
    else
    {
        src = wrapbuffer_fd(src_fd, test_case.width, test_case.height, test_case.format,
                            test_case.wstride, test_case.hstride);
        dst = wrapbuffer_fd(dst_fd, dst_width, dst_height, test_case.format,
                            dst_wstride, dst_hstride);
    }

    im_rect src_rect;
    im_rect dst_rect;
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    IM_STATUS check_status = (IM_STATUS)imcheck(src, dst, src_rect, dst_rect);
    printf("  imcheck=%d %s\n", check_status, imStrError(check_status));

    IM_STATUS resize_status = IM_STATUS_FAILED;
    if (check_status == IM_STATUS_NOERROR)
    {
        resize_status = imresize(src, dst);
    }
    printf("  imresize=%d %s\n", resize_status, imStrError(resize_status));

    if (strcmp(test_case.memory_name, "virtual") == 0)
    {
        free(src_addr);
        free(dst_addr);
    }
    else
    {
        release_dma_heap(src_fd, src_addr, src_size);
        release_dma_heap(dst_fd, dst_addr, dst_size);
    }
}

static void run_copy_case(const char *memory_name, const char *heap_path)
{
    const int width = 640;
    const int height = 640;
    const int channel = 3;
    const size_t size = (size_t)width * height * channel;
    void *src_addr = NULL;
    void *dst_addr = NULL;
    int src_fd = -1;
    int dst_fd = -1;

    printf("COPY_CASE memory=%s heap=%s format=RGB888 src=640x640 dst=640x640\n",
           memory_name,
           heap_path == NULL ? "-" : heap_path);

    if (strcmp(memory_name, "virtual") == 0)
    {
        if (posix_memalign(&src_addr, 4096, size) != 0 ||
            posix_memalign(&dst_addr, 4096, size) != 0)
        {
            printf("  posix_memalign failed\n");
            free(src_addr);
            free(dst_addr);
            return;
        }
    }
    else
    {
        if (alloc_dma_heap(heap_path, size, &src_fd, &src_addr) < 0 ||
            alloc_dma_heap(heap_path, size, &dst_fd, &dst_addr) < 0)
        {
            release_dma_heap(src_fd, src_addr, size);
            release_dma_heap(dst_fd, dst_addr, size);
            return;
        }
    }

    fill_pattern((uint8_t *)src_addr, size);
    memset(dst_addr, 0, size);

    rga_buffer_t src;
    rga_buffer_t dst;
    if (strcmp(memory_name, "virtual") == 0)
    {
        src = wrapbuffer_virtualaddr(src_addr, width, height, RK_FORMAT_RGB_888);
        dst = wrapbuffer_virtualaddr(dst_addr, width, height, RK_FORMAT_RGB_888);
    }
    else
    {
        src = wrapbuffer_fd(src_fd, width, height, RK_FORMAT_RGB_888);
        dst = wrapbuffer_fd(dst_fd, width, height, RK_FORMAT_RGB_888);
    }

    im_rect src_rect;
    im_rect dst_rect;
    memset(&src_rect, 0, sizeof(src_rect));
    memset(&dst_rect, 0, sizeof(dst_rect));

    IM_STATUS check_status = (IM_STATUS)imcheck(src, dst, src_rect, dst_rect);
    printf("  imcheck=%d %s\n", check_status, imStrError(check_status));

    IM_STATUS copy_status = IM_STATUS_FAILED;
    if (check_status == IM_STATUS_NOERROR)
    {
        copy_status = imcopy(src, dst);
    }
    printf("  imcopy=%d %s\n", copy_status, imStrError(copy_status));

    if (strcmp(memory_name, "virtual") == 0)
    {
        free(src_addr);
        free(dst_addr);
    }
    else
    {
        release_dma_heap(src_fd, src_addr, size);
        release_dma_heap(dst_fd, dst_addr, size);
    }
}

int main()
{
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

    const int formats[] = {RK_FORMAT_RGB_888, RK_FORMAT_RGBA_8888};
    const char *format_names[] = {"RGB888", "RGBA8888"};
    const int dims[][4] = {
        {1280, 720, 1280, 720},
        {1360, 765, 1360, 765},
        {1360, 765, 1360, 768},
        {1400, 788, 1400, 788},
        {1408, 800, 1408, 800},
    };
    const char *heaps[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/cma",
    };

    run_copy_case("virtual", NULL);
    for (size_t h = 0; h < sizeof(heaps) / sizeof(heaps[0]); ++h)
    {
        run_copy_case("dma_heap_fd", heaps[h]);
    }

    for (size_t d = 0; d < sizeof(dims) / sizeof(dims[0]); ++d)
    {
        for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f)
        {
            probe_case_t test_case;
            test_case.width = dims[d][0];
            test_case.height = dims[d][1];
            test_case.wstride = dims[d][2];
            test_case.hstride = dims[d][3];
            test_case.format = formats[f];
            test_case.format_name = format_names[f];
            test_case.memory_name = "virtual";
            test_case.heap_path = NULL;
            run_one_case(test_case);

            for (size_t h = 0; h < sizeof(heaps) / sizeof(heaps[0]); ++h)
            {
                test_case.memory_name = "dma_heap_fd";
                test_case.heap_path = heaps[h];
                run_one_case(test_case);
            }
        }
    }

    return 0;
}
